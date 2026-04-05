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
 * Growth path (hot path for sequential writes):
 *   1. Reserve quota for the additional bytes.
 *   2. ftruncate() the shm fd to the new size.
 *   3. Extend the server-side mmap in-place with MAP_FIXED.
 *      This avoids munmap + full remap, which is the dominant cost
 *      in the naive approach (~1.7 ms per resize vs ~0.15 ms with MAP_FIXED).
 *
 * Shrink path (hysteresis-controlled):
 *   Full munmap + ftruncate + mmap because shrinking the VA range
 *   requires establishing a new, smaller mapping.
 *
 * Returns 0 on success, errno on failure.
 * ---------------------------------------------------------------------- */
static int shm_resize(tmpfs_inode_t *ino, size_t new_cap)
{
    if (new_cap == ino->shm_cap)
        return 0;

    int rc;

    /* ---- GROW ---- */
    if (new_cap > ino->shm_cap) {
        size_t delta = new_cap - ino->shm_cap;
        rc = tmpfs_mem_reserve(ino->mount, delta);
        if (rc != 0)
            return rc;

        /* Extend the backing store */
        if (ftruncate(ino->shm_fd, (off_t)new_cap) == -1) {
            rc = errno;
            tmpfs_mem_release(ino->mount, delta);
            return rc;
        }

        if (ino->shm_ptr == MAP_FAILED) {
            /* First mapping: mmap the full new range */
            void *ptr = mmap(NULL, new_cap,
                             PROT_READ | PROT_WRITE, MAP_SHARED,
                             ino->shm_fd, 0);
            if (ptr == MAP_FAILED) {
                rc = errno;
                ftruncate(ino->shm_fd, 0);
                tmpfs_mem_release(ino->mount, delta);
                return rc;
            }
            ino->shm_ptr = ptr;
        } else {
            /*
             * Grow: full remap to a new non-overlapping address.
             *
             * We deliberately avoid MAP_FIXED for the extension even though
             * it would be faster (~12x less overhead per resize).  MAP_FIXED
             * silently unmaps any existing mapping at the target address,
             * which can corrupt a concurrent inode whose shm region happens
             * to start immediately after ours -- leading to ESRVRFAULT when
             * MsgRead/MsgReply tries to access the unmapped pages.
             *
             * The correct solution would be MAP_FIXED_NOREPLACE (Linux 4.17+),
             * but QNX 8 does not support it.  The msync probe we tried is also
             * insufficient because another inode can be mmap'd into the range
             * in the window between the probe and the MAP_FIXED call.
             *
             * Full remap (munmap + mmap(NULL)) is always safe because the OS
             * picks a fresh non-overlapping VA range.
             */
            size_t old_cap = ino->shm_cap;
            munmap(ino->shm_ptr, old_cap);
            void *ptr = mmap(NULL, new_cap,
                             PROT_READ | PROT_WRITE, MAP_SHARED,
                             ino->shm_fd, 0);
            if (ptr == MAP_FAILED) {
                rc = errno;
                ino->shm_ptr = MAP_FAILED;
                tmpfs_mem_release(ino->mount, delta);
                return rc;
            }
            ino->shm_ptr = ptr;
        }

        ino->shm_cap = new_cap;
        return 0;
    }

    /* ---- SHRINK ---- */
    /* Release excess backing store and remap to the smaller size */
    if (ino->shm_ptr != MAP_FAILED) {
        munmap(ino->shm_ptr, ino->shm_cap);
        ino->shm_ptr = MAP_FAILED;
    }

    if (new_cap == 0) {
        ftruncate(ino->shm_fd, 0);
        tmpfs_mem_release(ino->mount, ino->shm_cap);
        ino->shm_cap = 0;
        return 0;
    }

    if (ftruncate(ino->shm_fd, (off_t)new_cap) == -1) {
        rc = errno;
        tmpfs_mem_release(ino->mount, ino->shm_cap - new_cap);
        ino->shm_cap = 0;
        return rc;
    }

    void *ptr = mmap(NULL, new_cap, PROT_READ | PROT_WRITE,
                     MAP_SHARED, ino->shm_fd, 0);
    if (ptr == MAP_FAILED) {
        rc = errno;
        tmpfs_mem_release(ino->mount, ino->shm_cap - new_cap);
        ino->shm_cap = 0;
        return rc;
    }

    tmpfs_mem_release(ino->mount, ino->shm_cap - new_cap);
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
 * Set the file's logical size to `new_size`.
 *
 * Growth: grows the backing store as needed.
 *
 * Shrink: does NOT release the shm backing or VA mapping. The physical
 * backing (shm_cap) stays at its current value so subsequent writes
 * can reuse it without an expensive munmap+ftruncate+mmap cycle.
 * This is the correct tmpfs semantics: pages committed to a file stay
 * committed until the file is deleted. Quota is charged against shm_cap
 * (the high-water mark), not the current logical file size.
 *
 * The backing store is only released in tmpfs_inode_free() when the
 * inode is destroyed (nlink==0 and ref_count==0).
 *
 * Returns 0 on success, errno on failure.
 * Caller must hold ino->attr.lock.
 * ---------------------------------------------------------------------- */
int tmpfs_file_truncate(tmpfs_inode_t *ino, off_t new_size)
{
    if (new_size < 0)
        return EINVAL;

    size_t ns = (size_t)new_size;

    if (ns > ino->shm_cap) {
        /* Grow backing store */
        int rc = tmpfs_file_ensure_capacity(ino, ns);
        if (rc != 0)
            return rc;
    }
    /* No shrink path: shm_cap stays at current value so subsequent
     * writes avoid the expensive munmap+ftruncate+mmap resize cycle. */

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
