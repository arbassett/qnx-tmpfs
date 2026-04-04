/*
 * inode.c  --  inode lifecycle: alloc, init, ref-counting, destruction.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/iofunc.h>
#include <stdatomic.h>
#include <time.h>

#include "../include/tmpfs_internal.h"
#include "memory.h"
#include "inode.h"

/* Monotonically increasing inode number generator */
static atomic_uint_fast64_t s_next_ino = TMPFS_ROOT_INO + 1;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static void inode_stat_inc(tmpfs_inode_t *ino)
{
    tmpfs_mount_t *mnt = ino->mount;
    mode_t mode = ino->attr.mode;

    atomic_fetch_add(&mnt->inode_count, 1);
    if (S_ISREG(mode))       atomic_fetch_add(&mnt->file_count,    1);
    else if (S_ISDIR(mode))  atomic_fetch_add(&mnt->dir_count,     1);
    else if (S_ISLNK(mode))  atomic_fetch_add(&mnt->symlink_count, 1);
}

static void inode_stat_dec(tmpfs_inode_t *ino)
{
    tmpfs_mount_t *mnt = ino->mount;
    mode_t mode = ino->attr.mode;

    atomic_fetch_sub(&mnt->inode_count, 1);
    if (S_ISREG(mode))       atomic_fetch_sub(&mnt->file_count,    1);
    else if (S_ISDIR(mode))  atomic_fetch_sub(&mnt->dir_count,     1);
    else if (S_ISLNK(mode))  atomic_fetch_sub(&mnt->symlink_count, 1);
}

/* -------------------------------------------------------------------------
 * tmpfs_inode_alloc
 *
 * Allocate and initialise a new inode of the given mode.
 * Charges TMPFS_INODE_OVERHEAD bytes against the mount quota.
 * Returns NULL with errno set on failure.
 * ---------------------------------------------------------------------- */
tmpfs_inode_t *tmpfs_inode_alloc(tmpfs_mount_t *mnt, mode_t mode,
                                  struct _client_info *cinfo)
{
    int rc;

    /* Reserve quota for the inode metadata */
    rc = tmpfs_mem_reserve(mnt, TMPFS_INODE_OVERHEAD);
    if (rc != 0) {
        errno = rc;
        return NULL;
    }

    tmpfs_inode_t *ino = calloc(1, sizeof(tmpfs_inode_t));
    if (ino == NULL) {
        tmpfs_mem_release(mnt, TMPFS_INODE_OVERHEAD);
        errno = ENOMEM;
        return NULL;
    }

    /* Initialise the embedded iofunc_attr_t */
    iofunc_attr_init(&ino->attr, mode, NULL, cinfo);
    ino->attr.inode  = (ino_t)atomic_fetch_add(&s_next_ino, 1);
    ino->attr.mount  = &mnt->iofunc_mount;
    ino->attr.nlink  = S_ISDIR(mode) ? 2 : 1;

    /* Private fields */
    ino->mount       = mnt;
    ino->ref_count   = 0;
    ino->shm_fd      = -1;
    ino->shm_ptr     = MAP_FAILED;
    ino->shm_cap     = 0;
    ino->parent      = NULL;
    ino->child_count = 0;
    ino->symlink_target = NULL;

    inode_stat_inc(ino);
    return ino;
}

/*
 * tmpfs_inode_alloc_root
 *
 * Allocate the root directory inode with a fixed inode number.
 * Called once per mount during mount setup.
 */
tmpfs_inode_t *tmpfs_inode_alloc_root(tmpfs_mount_t *mnt, uid_t uid,
                                       gid_t gid, mode_t mode)
{
    int rc = tmpfs_mem_reserve(mnt, TMPFS_INODE_OVERHEAD);
    if (rc != 0) {
        errno = rc;
        return NULL;
    }

    tmpfs_inode_t *ino = calloc(1, sizeof(tmpfs_inode_t));
    if (ino == NULL) {
        tmpfs_mem_release(mnt, TMPFS_INODE_OVERHEAD);
        errno = ENOMEM;
        return NULL;
    }

    iofunc_attr_init(&ino->attr, S_IFDIR | (mode & 0777), NULL, NULL);
    ino->attr.inode  = TMPFS_ROOT_INO;
    ino->attr.mount  = &mnt->iofunc_mount;
    ino->attr.nlink  = 2;
    ino->attr.uid    = uid;
    ino->attr.gid    = gid;

    ino->mount       = mnt;
    ino->ref_count   = 0;
    ino->shm_fd      = -1;
    ino->shm_ptr     = MAP_FAILED;
    ino->shm_cap     = 0;
    ino->parent      = NULL;   /* root has no parent */
    ino->child_count = 0;
    ino->symlink_target = NULL;

    inode_stat_inc(ino);
    return ino;
}

/* -------------------------------------------------------------------------
 * tmpfs_inode_ref / tmpfs_inode_unref
 *
 * ref_count tracks how many OCBs hold a reference.
 * Must be called with the inode's attr.lock held (lock_ocb / unlock_ocb
 * already does this via iofunc helpers, but direct callers must acquire).
 * ---------------------------------------------------------------------- */
void tmpfs_inode_ref(tmpfs_inode_t *ino)
{
    ino->ref_count++;
}

/*
 * Drop a reference.  If ref_count and nlink both hit 0, the inode is freed.
 * Returns 1 if the inode was freed, 0 otherwise.
 */
int tmpfs_inode_unref(tmpfs_inode_t *ino)
{
    if (ino->ref_count > 0)
        ino->ref_count--;

    if (ino->ref_count == 0 && ino->attr.nlink == 0) {
        tmpfs_inode_free(ino);
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * tmpfs_inode_free
 *
 * Unconditionally destroy an inode and release all its resources.
 * The caller is responsible for ensuring no OCBs or directory entries
 * still reference this inode.
 * ---------------------------------------------------------------------- */
void tmpfs_inode_free(tmpfs_inode_t *ino)
{
    tmpfs_mount_t *mnt = ino->mount;
    size_t total_charge = TMPFS_INODE_OVERHEAD;

    /* Release file backing store */
    if (ino->shm_fd != -1) {
        if (ino->shm_ptr != MAP_FAILED && ino->shm_cap > 0)
            munmap(ino->shm_ptr, ino->shm_cap);
        close(ino->shm_fd);
        total_charge += ino->shm_cap;
    }

    /* Free symlink target */
    if (ino->symlink_target != NULL) {
        total_charge += strlen(ino->symlink_target) + 1;
        free(ino->symlink_target);
    }

    inode_stat_dec(ino);
    tmpfs_mem_release(mnt, total_charge);
    free(ino);
}
