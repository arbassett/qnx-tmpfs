/*
 * file.c  --  Regular file operations: read, write, truncate, shm growth.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/iofunc.h>
#include <sys/resmgr.h>

#include "../include/tmpfs_internal.h"
#include "memory.h"
#include "inode.h"
#include "file.h"

/* -------------------------------------------------------------------------
 * Internal: compute the desired backing capacity for a given file size.
 *
 * Uses exponential doubling up to TMPFS_SHM_DOUBLE_THRESHOLD,
 * then linear increments of TMPFS_SHM_LINEAR_INCREMENT.
 * Always aligns to the system page size.
 * ---------------------------------------------------------------------- */
static size_t desired_capacity(size_t file_size)
{
    static long page_size = 0;
    if (page_size == 0)
        page_size = sysconf(_SC_PAGE_SIZE);
    if (page_size <= 0)
        page_size = 4096;

    if (file_size == 0)
        return 0;

    size_t cap;
    if (file_size <= TMPFS_SHM_DOUBLE_THRESHOLD) {
        cap = TMPFS_SHM_INITIAL_SIZE;
        while (cap < file_size)
            cap <<= 1;
    } else {
        /* Round up to next multiple of linear increment */
        size_t inc = TMPFS_SHM_LINEAR_INCREMENT;
        cap = ((file_size + inc - 1) / inc) * inc;
    }

    /* Page-align */
    size_t ps = (size_t)page_size;
    cap = ((cap + ps - 1) / ps) * ps;
    return cap;
}

/* -------------------------------------------------------------------------
 * tmpfs_file_open_shm
 *
 * Initialise the SHM backing store for a newly created regular file.
 * Does NOT charge quota here -- the inode overhead was already charged
 * in inode_alloc; shm capacity is charged only when the file grows.
 * ---------------------------------------------------------------------- */
int tmpfs_file_open_shm(tmpfs_inode_t *ino)
{
    ino->shm_fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    if (ino->shm_fd == -1)
        return errno;
    ino->shm_cap = 0;
    ino->shm_ptr = MAP_FAILED;
    return 0;
}

/* -------------------------------------------------------------------------
 * Internal: grow (or shrink) the SHM backing store to `new_cap` bytes.
 *
 * Handles quota reservation for growth and release for shrink.
 * Re-maps the server-side pointer.
 * Returns 0 on success, errno on failure.
 * ---------------------------------------------------------------------- */
static int shm_resize(tmpfs_inode_t *ino, size_t new_cap)
{
    if (new_cap == ino->shm_cap)
        return 0;

    int rc;

    if (new_cap > ino->shm_cap) {
        size_t delta = new_cap - ino->shm_cap;
        rc = tmpfs_mem_reserve(ino->mount, delta);
        if (rc != 0)
            return rc;
    }

    /* Unmap old server-side mapping */
    if (ino->shm_ptr != MAP_FAILED && ino->shm_cap > 0) {
        munmap(ino->shm_ptr, ino->shm_cap);
        ino->shm_ptr = MAP_FAILED;
    }

    if (new_cap == 0) {
        ftruncate(ino->shm_fd, 0);
        if (ino->shm_cap > 0)
            tmpfs_mem_release(ino->mount, ino->shm_cap);
        ino->shm_cap = 0;
        return 0;
    }

    /* Resize the shm object */
    if (ftruncate(ino->shm_fd, (off_t)new_cap) == -1) {
        rc = errno;
        if (new_cap > ino->shm_cap)
            tmpfs_mem_release(ino->mount, new_cap - ino->shm_cap);
        return rc;
    }

    /* Release excess quota when shrinking */
    if (new_cap < ino->shm_cap)
        tmpfs_mem_release(ino->mount, ino->shm_cap - new_cap);

    /* Re-map */
    void *ptr = mmap(NULL, new_cap, PROT_READ | PROT_WRITE,
                     MAP_SHARED, ino->shm_fd, 0);
    if (ptr == MAP_FAILED) {
        rc = errno;
        /* Attempt to restore old capacity - best effort */
        ftruncate(ino->shm_fd, (off_t)ino->shm_cap);
        if (new_cap > ino->shm_cap)
            tmpfs_mem_release(ino->mount, new_cap - ino->shm_cap);
        return rc;
    }

    ino->shm_ptr = ptr;
    ino->shm_cap = new_cap;
    return 0;
}

/* -------------------------------------------------------------------------
 * tmpfs_file_ensure_capacity
 *
 * Make sure the backing store is large enough for `needed` bytes.
 * Grows using the doubling/linear strategy.  Never shrinks.
 * ---------------------------------------------------------------------- */
int tmpfs_file_ensure_capacity(tmpfs_inode_t *ino, size_t needed)
{
    if (needed <= ino->shm_cap)
        return 0;
    size_t new_cap = desired_capacity(needed);
    return shm_resize(ino, new_cap);
}

/* -------------------------------------------------------------------------
 * tmpfs_file_read
 *
 * Read `nbytes` from file `ino` at `offset` into caller-provided `buf`.
 * Returns number of bytes actually read, or -1 with errno set.
 * Caller must hold ino->attr.lock.
 * ---------------------------------------------------------------------- */
ssize_t tmpfs_file_read(tmpfs_inode_t *ino, void *buf, size_t nbytes,
                         off_t offset)
{
    off_t file_size = ino->attr.nbytes;

    if (offset >= file_size)
        return 0;

    size_t avail = (size_t)(file_size - offset);
    if (nbytes > avail)
        nbytes = avail;
    if (nbytes == 0)
        return 0;

    memcpy(buf, (char *)ino->shm_ptr + offset, nbytes);
    return (ssize_t)nbytes;
}

/* -------------------------------------------------------------------------
 * tmpfs_file_write
 *
 * Write `nbytes` from `buf` into file `ino` at `offset`.
 * Grows the file and backing store as needed.
 * Returns number of bytes written, or -1 with errno set.
 * Caller must hold ino->attr.lock.
 * ---------------------------------------------------------------------- */
ssize_t tmpfs_file_write(tmpfs_inode_t *ino, const void *buf, size_t nbytes,
                          off_t offset)
{
    if (nbytes == 0)
        return 0;

    size_t end = (size_t)offset + nbytes;

    int rc = tmpfs_file_ensure_capacity(ino, end);
    if (rc != 0) {
        errno = rc;
        return -1;
    }

    memcpy((char *)ino->shm_ptr + offset, buf, nbytes);

    if ((off_t)end > ino->attr.nbytes)
        ino->attr.nbytes = (off_t)end;

    iofunc_time_update(&ino->attr);
    return (ssize_t)nbytes;
}

/* -------------------------------------------------------------------------
 * tmpfs_file_truncate
 *
 * Set the file size to `new_size`.
 * Grows or shrinks the backing store following the hysteresis rule:
 *   shrink if new_size < current_capacity / TMPFS_SHM_SHRINK_RATIO
 * Returns 0 on success, errno on failure.
 * Caller must hold ino->attr.lock.
 * ---------------------------------------------------------------------- */
int tmpfs_file_truncate(tmpfs_inode_t *ino, off_t new_size)
{
    if (new_size < 0)
        return EINVAL;

    size_t ns = (size_t)new_size;
    int rc = 0;

    if (ns > ino->shm_cap) {
        /* Grow backing store */
        rc = tmpfs_file_ensure_capacity(ino, ns);
        if (rc != 0)
            return rc;
    } else if (ino->shm_cap > 0 && ns < ino->shm_cap / TMPFS_SHM_SHRINK_RATIO) {
        /* Shrink backing store (hysteresis) */
        size_t new_cap = desired_capacity(ns);
        rc = shm_resize(ino, new_cap);
        if (rc != 0)
            return rc;
    }

    /* Zero out the region from new_size to old_size if shrinking */
    if ((off_t)ns < ino->attr.nbytes && ino->shm_ptr != MAP_FAILED) {
        size_t to_zero = (size_t)(ino->attr.nbytes - (off_t)ns);
        if (ns + to_zero <= ino->shm_cap)
            memset((char *)ino->shm_ptr + ns, 0, to_zero);
    }

    ino->attr.nbytes = (off_t)ns;
    iofunc_time_update(&ino->attr);
    return 0;
}
