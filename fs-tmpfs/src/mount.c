/*
 * mount.c  --  Mount lifecycle: option parsing, attach, detach, teardown.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/resmgr.h>
#include <sys/stat.h>

#include "../include/tmpfs_internal.h"
#include "../include/tmpfs_ipc.h"
#include "memory.h"
#include "inode.h"
#include "dir.h"
#include "mount.h"

/* Declared in resmgr.c */
extern resmgr_connect_funcs_t g_connect_funcs;
extern resmgr_io_funcs_t      g_io_funcs;
extern iofunc_funcs_t        *g_tmpfs_iofunc_funcs;

/* -------------------------------------------------------------------------
 * tmpfs_parse_size
 *
 * Parse a size string into bytes.
 * Formats: N, NB, NK, NM, NG, N%
 * Percent is relative to total physical RAM.
 * Returns 0 on success, errno on failure.
 * ---------------------------------------------------------------------- */
int tmpfs_parse_size(const char *str, uint64_t total_ram, uint64_t *out)
{
    if (str == NULL || *str == '\0')
        return EINVAL;

    char *end;
    unsigned long long val = strtoull(str, &end, 10);
    if (end == str)
        return EINVAL;

    if (*end == '\0' || *end == 'B' || *end == 'b') {
        *out = (uint64_t)val;
    } else if (*end == 'K' || *end == 'k') {
        *out = (uint64_t)val * 1024ULL;
    } else if (*end == 'M' || *end == 'm') {
        *out = (uint64_t)val * 1024ULL * 1024ULL;
    } else if (*end == 'G' || *end == 'g') {
        *out = (uint64_t)val * 1024ULL * 1024ULL * 1024ULL;
    } else if (*end == '%') {
        if (val > TMPFS_MAX_MOUNT_PERCENT)
            return EINVAL;
        *out = (uint64_t)(total_ram * val / 100ULL);
    } else {
        return EINVAL;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Internal: recursively destroy all inodes in a subtree (post-order).
 * ---------------------------------------------------------------------- */
static void destroy_tree(tmpfs_inode_t *ino)
{
    if (S_ISDIR(ino->attr.mode)) {
        for (unsigned b = 0; b < TMPFS_DIR_HASH_BUCKETS; b++) {
            tmpfs_dirent_t *de = ino->hash[b];
            while (de != NULL) {
                tmpfs_dirent_t *next = de->next;
                destroy_tree(de->inode);
                free(de->name);
                free(de);
                de = next;
            }
            ino->hash[b] = NULL;
        }
    }
    tmpfs_inode_free(ino);
}

/* -------------------------------------------------------------------------
 * tmpfs_mount_add
 *
 * Attach a new mount point.  Called both from main() for the first mount
 * and from the control devctl handler for subsequent mounts.
 * ---------------------------------------------------------------------- */
int tmpfs_mount_add(const tmpfs_mount_req_t *req)
{
    /* --- Resolve the mount size --- */
    uint64_t cap;
    if (req->size_opt.bytes == 0) {
        /* Default: 25% of total RAM */
        cap = g_tmpfs.total_ram / TMPFS_DEFAULT_MOUNT_FRACTION;
    } else {
        cap = req->size_opt.bytes;
    }

    if (cap < TMPFS_MIN_MOUNT_SIZE)
        return EINVAL;

    /* Cap must not exceed global budget */
    if (cap > g_tmpfs.global_cap)
        cap = g_tmpfs.global_cap;

    /* Ensure the requested cap would fit in the remaining global budget */
    size_t currently_used = tmpfs_mem_used_global();
    if (cap > g_tmpfs.global_cap - currently_used)
        return ENOSPC;

    /* --- Allocate mount structure --- */
    tmpfs_mount_t *mnt = calloc(1, sizeof(tmpfs_mount_t));
    if (mnt == NULL)
        return ENOMEM;

    mnt->mount_cap = (size_t)cap;
    atomic_init(&mnt->mount_used, 0);
    atomic_init(&mnt->file_count, 0);
    atomic_init(&mnt->dir_count, 0);
    atomic_init(&mnt->symlink_count, 0);
    atomic_init(&mnt->inode_count, 0);
    strncpy(mnt->path, req->path, PATH_MAX - 1);

    /* --- Initialise iofunc_mount_t --- */
    iofunc_mount_init(&mnt->iofunc_mount, sizeof(mnt->iofunc_mount));
    mnt->iofunc_mount.conf      = IOFUNC_PC_CHOWN_RESTRICTED |
                                   IOFUNC_PC_NO_TRUNC         |
                                   IOFUNC_PC_SYNC_IO          |
                                   IOFUNC_PC_SYMLINK;
    mnt->iofunc_mount.blocksize = 4096;
    mnt->iofunc_mount.dev       = (dev_t)((uintptr_t)mnt & 0xffffffff);
    mnt->iofunc_mount.funcs     = g_tmpfs_iofunc_funcs;

    /* --- Create root directory inode --- */
    mode_t root_mode = req->mode ? req->mode : 0755;
    mnt->root = tmpfs_inode_alloc_root(mnt, req->uid, req->gid, root_mode);
    if (mnt->root == NULL) {
        int e = errno;
        free(mnt);
        return e;
    }

    /* --- Attach resmgr path --- */
    resmgr_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.nparts_max   = 8;
    attr.msg_max_size = 65536;

    mnt->resmgr_id = resmgr_attach(
        g_tmpfs.dpp,
        &attr,
        req->path,
        _FTYPE_MOUNT,
        _RESMGR_FLAG_DIR | _RESMGR_FLAG_SELF | _RESMGR_FLAG_BEFORE,
        &g_connect_funcs,
        &g_io_funcs,
        &mnt->root->attr);  /* handle = root inode's attr */

    if (mnt->resmgr_id == -1) {
        int e = errno;
        tmpfs_inode_free(mnt->root);
        free(mnt);
        return e;
    }

    /* --- Add to global mount list --- */
    pthread_rwlock_wrlock(&g_tmpfs.mounts_lock);
    mnt->next          = g_tmpfs.mounts;
    g_tmpfs.mounts     = mnt;
    g_tmpfs.mount_count++;
    pthread_rwlock_unlock(&g_tmpfs.mounts_lock);

    return EOK;
}

/* -------------------------------------------------------------------------
 * tmpfs_mount_remove
 *
 * Detach a mount by path and free all its resources.
 * Decrements the global mount count; if it reaches 0 the coordinator will
 * exit after a short grace period (handled in main.c).
 * ---------------------------------------------------------------------- */
int tmpfs_mount_remove(const char *path)
{
    pthread_rwlock_wrlock(&g_tmpfs.mounts_lock);

    tmpfs_mount_t **pp  = &g_tmpfs.mounts;
    tmpfs_mount_t  *mnt = NULL;

    while (*pp != NULL) {
        if (strcmp((*pp)->path, path) == 0) {
            mnt  = *pp;
            *pp  = mnt->next;
            g_tmpfs.mount_count--;
            break;
        }
        pp = &(*pp)->next;
    }

    pthread_rwlock_unlock(&g_tmpfs.mounts_lock);

    if (mnt == NULL)
        return ENOENT;

    /* Detach from path manager (stops new opens) */
    resmgr_detach(g_tmpfs.dpp, mnt->resmgr_id, _RESMGR_DETACH_ALL);

    /* Recursively free the entire filesystem tree */
    destroy_tree(mnt->root);

    free(mnt);
    return EOK;
}

/* -------------------------------------------------------------------------
 * tmpfs_mount_find
 *
 * Find a mount by its resmgr id.  Used by connect handlers to identify
 * which mount a message arrived on.
 * Caller must hold g_tmpfs.mounts_lock (at least read).
 * ---------------------------------------------------------------------- */
tmpfs_mount_t *tmpfs_mount_find_by_id(int resmgr_id)
{
    tmpfs_mount_t *mnt = g_tmpfs.mounts;
    while (mnt != NULL) {
        if (mnt->resmgr_id == resmgr_id)
            return mnt;
        mnt = mnt->next;
    }
    return NULL;
}
