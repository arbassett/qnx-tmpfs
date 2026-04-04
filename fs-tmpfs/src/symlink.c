/*
 * symlink.c  --  Symlink creation and target retrieval.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "../include/tmpfs_internal.h"
#include "memory.h"
#include "inode.h"
#include "dir.h"
#include "symlink.h"

/* -------------------------------------------------------------------------
 * tmpfs_symlink_create
 *
 * Create a symlink named `name` in directory `dir` pointing to `target`.
 * Charges quota for inode overhead + target string length.
 * Returns 0 on success, errno on failure.
 * Caller must hold dir->attr.lock (write).
 * ---------------------------------------------------------------------- */
int tmpfs_symlink_create(tmpfs_inode_t *dir, const char *name,
                          const char *target, struct _client_info *cinfo)
{
    size_t target_len = strlen(target);
    if (target_len == 0 || target_len >= PATH_MAX)
        return EINVAL;

    /* Allocate the symlink inode */
    tmpfs_inode_t *ino = tmpfs_inode_alloc(dir->mount,
                                            S_IFLNK | 0777, cinfo);
    if (ino == NULL)
        return errno;

    /* Reserve quota for the target string */
    int rc = tmpfs_mem_reserve(dir->mount, target_len + 1);
    if (rc != 0) {
        tmpfs_inode_free(ino);
        return rc;
    }

    ino->symlink_target = malloc(target_len + 1);
    if (ino->symlink_target == NULL) {
        tmpfs_mem_release(dir->mount, target_len + 1);
        tmpfs_inode_free(ino);
        return ENOMEM;
    }
    memcpy(ino->symlink_target, target, target_len + 1);
    ino->attr.nbytes = (off_t)target_len;

    /* Insert into parent directory */
    rc = tmpfs_dir_insert(dir, name, ino);
    if (rc != 0) {
        tmpfs_inode_free(ino);
        return rc;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * tmpfs_symlink_read
 *
 * Copy the symlink target into `buf` (up to `bufsz` bytes, no NUL).
 * Returns the number of bytes written (POSIX readlink semantics).
 * ---------------------------------------------------------------------- */
ssize_t tmpfs_symlink_read(const tmpfs_inode_t *ino, char *buf, size_t bufsz)
{
    if (!S_ISLNK(ino->attr.mode))
        return -1;
    if (ino->symlink_target == NULL)
        return 0;

    size_t len = strlen(ino->symlink_target);
    if (len > bufsz) len = bufsz;
    memcpy(buf, ino->symlink_target, len);
    return (ssize_t)len;
}
