/*
 * resmgr.c  --  Dispatch loop, thread pool, and all IO/connect handlers.
 *
 * Handler responsibility summary:
 *
 *   connect_funcs.open      : path walk, create if O_CREAT, allocate OCB
 *   connect_funcs.unlink    : remove file or empty dir
 *   connect_funcs.rename    : atomic rename within same mount
 *   connect_funcs.mknod     : create regular file, directory, or FIFO
 *   connect_funcs.readlink  : return symlink target
 *   connect_funcs.link      : create hard link (_IO_CONNECT_EXTRA_SYMLINK
 *                             sub-case also handled here to create symlinks)
 *
 *   io_funcs.read           : file data read, or readdir for directories
 *   io_funcs.write          : file data write
 *   io_funcs.stat           : iofunc_stat_default  (uses iofunc_attr_t)
 *   io_funcs.lseek          : iofunc_lseek_default
 *   io_funcs.chmod          : iofunc_chmod_default
 *   io_funcs.chown          : iofunc_chown_default
 *   io_funcs.utime          : iofunc_utime_default
 *   io_funcs.devctl         : ENOSYS (no device-specific controls on files)
 *   io_funcs.close_ocb      : decref inode, free OCB
 *   io_funcs.lock_ocb       : iofunc_lock_ocb_default
 *   io_funcs.unlock_ocb     : iofunc_unlock_ocb_default
 *   io_funcs.mmap           : redirect to shm_fd  (mmap.c)
 *   io_funcs.sync           : update mtime         (mmap.c)
 *   io_funcs.space          : allocate disk space (fallocate-style)
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/iofunc.h>
#include <sys/resmgr.h>

#include <sys/dispatch.h>
#include <sys/stat.h>
#include <sys/iomsg.h>

#include "../include/tmpfs_internal.h"
#include "memory.h"
#include "inode.h"
#include "dir.h"
#include "file.h"
#include "symlink.h"
#include "mmap.h"
#include "mount.h"
#include "resmgr.h"

/* -------------------------------------------------------------------------
 * Exported function tables (referenced by mount.c)
 * ---------------------------------------------------------------------- */
resmgr_connect_funcs_t g_connect_funcs;
resmgr_io_funcs_t      g_io_funcs;

/* -------------------------------------------------------------------------
 * Custom iofunc_funcs_t: lets iofunc_close_ocb_default call our ocb_free
 * so we avoid a double-free and properly unref the inode on close.
 * ---------------------------------------------------------------------- */
static iofunc_ocb_t *tmpfs_ocb_calloc(resmgr_context_t *ctp, iofunc_attr_t *attr)
{
    (void)ctp; (void)attr;
    tmpfs_ocb_t *tocb = calloc(1, sizeof(tmpfs_ocb_t));
    return tocb ? &tocb->ocb : NULL;
}

static void tmpfs_ocb_free(iofunc_ocb_t *ocb)
{
    /*
     * Called by iofunc_close_ocb_default WITH attr.lock held.
     * Only free the OCB struct here. Inode unref/free is handled
     * in tmpfs_io_close_ocb AFTER iofunc_close_ocb_default returns
     * and the lock is released.
     */
    free(TMPFS_OCB(ocb));
}

static iofunc_funcs_t g_iofunc_funcs = {
    .nfuncs     = _IOFUNC_NFUNCS,
    .ocb_calloc = tmpfs_ocb_calloc,
    .ocb_free   = tmpfs_ocb_free,
};
iofunc_funcs_t *g_tmpfs_iofunc_funcs = &g_iofunc_funcs;

/*
 * tmpfs_check_access  --  POSIX permission check, bypassing QNX's
 * iofunc_check_access which incorrectly denies non-root users even on
 * world-writable directories. Checks mode bits directly.
 *
 * need: bitmask of W_OK(2), R_OK(4), X_OK(1) (from <unistd.h>)
 */
static int tmpfs_check_access(const iofunc_attr_t *attr, mode_t need,
                               const struct _client_info *ci)
{
    if (ci->cred.euid == 0)
        return EOK;  /* root has all access */

    mode_t  mode = attr->mode;
    mode_t  eff;

    if (ci->cred.euid == attr->uid)
        eff = (mode >> 6) & 7;   /* owner bits */
    else if (ci->cred.egid == attr->gid)
        eff = (mode >> 3) & 7;   /* group bits */
    else
        eff = (mode >> 0) & 7;   /* other bits */

    return ((eff & need) == need) ? EOK : EPERM;
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * Allocate a new OCB for the given inode and bind it to the resmgr layer.
 */
static int bind_ocb(resmgr_context_t *ctp, io_open_t *msg,
                    tmpfs_inode_t *ino)
{
    tmpfs_ocb_t *tocb = calloc(1, sizeof(tmpfs_ocb_t));
    if (tocb == NULL)
        return ENOMEM;

    tocb->inode   = ino;
    tocb->dir_pos = 0;

    /*
     * iofunc_ocb_attach:
     *   - sets ocb->attr  = attr
     *   - sets ocb->ioflag from msg
     *   - calls resmgr_open_bind() to register the OCB
     * It does NOT call ocb_calloc (we already allocated).
     * It does NOT call ocb_free on failure paths beyond resmgr_open_bind.
     */
    int rc = iofunc_ocb_attach(ctp, msg, &tocb->ocb, &ino->attr, &g_io_funcs);
    if (rc != EOK) {
        free(tocb);
        return rc;
    }

    iofunc_attr_lock(&ino->attr);
    tmpfs_inode_ref(ino);
    iofunc_attr_unlock(&ino->attr);
    return EOK;
}

/* =========================================================================
 * CONNECT HANDLERS
 * ====================================================================== */

/*
 * tmpfs_connect_open
 *
 * Called for _IO_CONNECT_OPEN (and O_CREAT creates).
 * Performs path resolution and, if needed, creates inodes.
 */
int tmpfs_connect_open(resmgr_context_t *ctp, io_open_t *msg,
                        iofunc_attr_t *handle, void *extra)
{
    tmpfs_inode_t *root = INODE_FROM_ATTR(handle);
    tmpfs_mount_t *mnt  = root->mount;

    const char *path = msg->connect.path;
    uint32_t    oflag = msg->connect.ioflag;
    mode_t      mode  = msg->connect.mode;

    /* Walk to the target.
     * Always use follow=0 for the final component -- the pathmgr handles
     * symlink following at the VFS layer via _IO_CONNECT_READLINK.
     * We only follow intermediate components (handled inside dir_walk).
     */
    tmpfs_inode_t *parent  = NULL;
    const char    *basename = NULL;
    int            follow   = 0;   /* never follow the final component */

    tmpfs_inode_t *ino = tmpfs_dir_walk(root, path, follow,
                                         &parent, &basename);

    if (ino == NULL) {
        if (errno != ENOENT)
            return errno;

        /* Not found -- create if O_CREAT */
        if (!(oflag & O_CREAT) || parent == NULL || basename == NULL)
            return ENOENT;

        /* Prevent creating inside a non-directory */
        if (!S_ISDIR(parent->attr.mode))
            return ENOTDIR;

        /* Reject trailing slash on a non-directory create */
        if (oflag & O_DIRECTORY)
            return ENOENT;

        struct _client_info cinfo;
        iofunc_client_info(ctp, 0, &cinfo);

        ino = tmpfs_inode_alloc(mnt, S_IFREG | (mode & ~0111 & 0777), &cinfo);
        if (ino == NULL)
            return errno;

        int rc = tmpfs_file_open_shm(ino);
        if (rc != EOK) {
            tmpfs_inode_free(ino);
            return rc;
        }

        iofunc_attr_lock(&parent->attr);
        rc = tmpfs_dir_insert(parent, basename, ino);
        iofunc_attr_unlock(&parent->attr);

        if (rc != EOK) {
            tmpfs_file_truncate(ino, 0);
            tmpfs_inode_free(ino);
            return rc;
        }
    } else {
        /* Found */
        if ((oflag & O_CREAT) && (oflag & O_EXCL))
            return EEXIST;

        /*
         * Symlink: if we should follow it, redirect the pathmgr via
         * _IO_CONNECT_RET_LINK. The pathmgr will re-resolve the target
         * path from the mount root and send us a new open message.
         * If O_NOFOLLOW is set, fall through and open the symlink directly.
         */
        if (S_ISLNK(ino->attr.mode) && !(oflag & O_NOFOLLOW)
            && msg->connect.subtype == _IO_CONNECT_OPEN) {
            const char *target = ino->symlink_target ? ino->symlink_target : "";
            char fullpath[PATH_MAX];

            /*
             * For relative symlink targets, prepend the parent directory path
             * so the pathmgr resolves from the mount root correctly.
             * E.g. target='file.txt' in parent 'sub/' -> 'sub/file.txt'
             */
            if (target[0] != '/' && parent != NULL && parent != root) {
                /* Build parent path from root */
                char parpath[PATH_MAX] = "";
                /* Walk up from parent to build its path */
                /* Simple approach: the path we walked is in msg->connect.path.
                 * Strip the last component to get the parent path. */
                const char *p = msg->connect.path;
                const char *lastslash = strrchr(p, '/');
                if (lastslash != NULL) {
                    size_t parlen = (size_t)(lastslash - p);
                    if (parlen < PATH_MAX - 1) {
                        memcpy(parpath, p, parlen);
                        parpath[parlen] = '\0';
                    }
                }
                if (parpath[0] != '\0')
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", parpath, target);
                else
                    snprintf(fullpath, sizeof(fullpath), "%s", target);
                target = fullpath;
            }

            size_t tlen = strlen(target);
            struct _io_connect_link_reply *rep =
                (struct _io_connect_link_reply *)msg;
            memset(rep, 0, sizeof(*rep));
            rep->nentries = 0;
            rep->path_len = (uint16_t)(tlen + 1);
            memcpy((char *)rep + sizeof(*rep), target, tlen + 1);
            SETIOV(&ctp->iov[0], rep, sizeof(*rep) + tlen + 1);
            _IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_LINK);
            return _RESMGR_NPARTS(1);
        }

        /*
         * Do NOT check O_WRONLY/O_RDWR here -- on QNX, O_WRONLY=1 which
         * collides with _IO_FLAG_RD. Let iofunc_open() handle EISDIR for
         * write opens on directories.
         */

        /* Truncate on open */
        if (S_ISREG(ino->attr.mode) && (oflag & O_TRUNC)) {
            iofunc_attr_lock(&ino->attr);
            tmpfs_file_truncate(ino, 0);
            iofunc_attr_unlock(&ino->attr);
        }
    }

    /* Check permissions */
    struct _client_info cinfo;
    iofunc_client_info(ctp, 0, &cinfo);

    /*
     * When opening a directory, the pathmgr may send ioflag with neither
     * _IO_FLAG_RD nor _IO_FLAG_WR set (e.g. ioflag=0x8880 from opendir).
     * iofunc_open returns EISDIR for such opens on a directory.
     * Detect this and force _IO_FLAG_RD so iofunc_open accepts it.
     */
    if (S_ISDIR(ino->attr.mode) &&
        !(msg->connect.ioflag & (_IO_FLAG_RD | _IO_FLAG_WR))) {
        msg->connect.ioflag |= _IO_FLAG_RD;
    }

    int rc = iofunc_open(ctp, msg, &ino->attr, NULL, &cinfo);
    if (rc != EOK)
        return rc;

    return bind_ocb(ctp, msg, ino);
}

/*
 * tmpfs_connect_unlink
 *
 * Remove a file or empty directory.
 */
int tmpfs_connect_unlink(resmgr_context_t *ctp, io_unlink_t *msg,
                          iofunc_attr_t *handle, void *reserved)
{
    tmpfs_inode_t *root = INODE_FROM_ATTR(handle);

    tmpfs_inode_t *parent  = NULL;
    const char    *basename = NULL;
    tmpfs_inode_t *ino = tmpfs_dir_walk(root, msg->connect.path, 0,
                                         &parent, &basename);
    if (ino == NULL)
        return ENOENT;
    if (parent == NULL || basename == NULL)
        return EBUSY; /* can't unlink mount root */

    /* Check: directories must be empty */
    if (S_ISDIR(ino->attr.mode)) {
        iofunc_attr_lock(&ino->attr);
        int empty = (ino->child_count == 0);
        iofunc_attr_unlock(&ino->attr);
        if (!empty)
            return ENOTEMPTY;
    }

    /* Permission check on parent */
    struct _client_info cinfo;
    iofunc_client_info(ctp, 0, &cinfo);
    int rc = tmpfs_check_access(&parent->attr, W_OK, &cinfo);
    if (rc != EOK)
        return rc;

    iofunc_attr_lock(&parent->attr);
    tmpfs_dir_remove(parent, basename);
    iofunc_attr_unlock(&parent->attr);

    iofunc_attr_lock(&ino->attr);
    ino->attr.nlink--;
    /* Directories: also drop the ".." reference */
    if (S_ISDIR(ino->attr.mode) && parent->attr.nlink > 0)
        parent->attr.nlink--;
    int do_free = (ino->attr.nlink == 0 && ino->ref_count == 0);
    iofunc_attr_unlock(&ino->attr);

    if (do_free)
        tmpfs_inode_free(ino);

    return EOK;
}

/*
 * tmpfs_connect_rename
 */
int tmpfs_connect_rename(resmgr_context_t *ctp, io_rename_t *msg,
                          iofunc_attr_t *handle, io_rename_extra_t *extra)
{
    tmpfs_inode_t *root = INODE_FROM_ATTR(handle);

    if (!extra) return EINVAL;

    /*
     * QNX rename protocol:
     *   connect.path = new (destination) path relative to mount root
     *   extra->path  = old (source)      path relative to mount root
     */
    const char *new_path = msg->connect.path;
    const char *old_path = extra->path;

    tmpfs_inode_t *old_parent = NULL, *new_parent = NULL;
    const char    *old_base   = NULL,  *new_base   = NULL;

    tmpfs_inode_t *src = tmpfs_dir_walk(root, old_path, 0,
                                         &old_parent, &old_base);
    if (src == NULL)
        return ENOENT;

    tmpfs_inode_t *dst = tmpfs_dir_walk(root, new_path, 0,
                                         &new_parent, &new_base);

    if (old_parent == NULL || old_base == NULL)
        return EBUSY;
    if (new_parent == NULL || new_base == NULL)
        return ENOENT;

    struct _client_info cinfo;
    iofunc_client_info(ctp, 0, &cinfo);

    /* Permission checks */
    int rc = tmpfs_check_access(&old_parent->attr, W_OK, &cinfo);
    if (rc != EOK) return rc;
    rc     = tmpfs_check_access(&new_parent->attr, W_OK, &cinfo);
    if (rc != EOK) return rc;

    /* If destination exists, remove it first (must be empty if dir) */
    if (dst != NULL) {
        if (S_ISDIR(dst->attr.mode)) {
            if (!S_ISDIR(src->attr.mode)) return EISDIR;
            iofunc_attr_lock(&dst->attr);
            int empty = (dst->child_count == 0);
            iofunc_attr_unlock(&dst->attr);
            if (!empty) return ENOTEMPTY;
        } else {
            if (S_ISDIR(src->attr.mode)) return ENOTDIR;
        }
        iofunc_attr_lock(&new_parent->attr);
        tmpfs_dir_remove(new_parent, new_base);
        iofunc_attr_unlock(&new_parent->attr);

        iofunc_attr_lock(&dst->attr);
        dst->attr.nlink--;
        int do_free = (dst->attr.nlink == 0 && dst->ref_count == 0);
        iofunc_attr_unlock(&dst->attr);
        if (do_free) tmpfs_inode_free(dst);
    }

    /* Move: remove from old parent, insert into new parent */
    iofunc_attr_lock(&old_parent->attr);
    tmpfs_dir_remove(old_parent, old_base);
    iofunc_attr_unlock(&old_parent->attr);

    iofunc_attr_lock(&new_parent->attr);
    rc = tmpfs_dir_insert(new_parent, new_base, src);
    iofunc_attr_unlock(&new_parent->attr);

    if (rc != EOK) {
        /* Failed to insert into new parent -- re-insert into old parent */
        iofunc_attr_lock(&old_parent->attr);
        tmpfs_dir_insert(old_parent, old_base, src);
        iofunc_attr_unlock(&old_parent->attr);
        return rc;
    }

    /* Update parent pointer for directories */
    if (S_ISDIR(src->attr.mode))
        src->parent = new_parent;

    iofunc_time_update(&src->attr);
    return EOK;
}

/*
 * tmpfs_connect_mknod
 *
 * Create a regular file, directory, or named pipe.
 * Symlinks are handled via tmpfs_connect_link.
 */
int tmpfs_connect_mknod(resmgr_context_t *ctp, io_mknod_t *msg,
                         iofunc_attr_t *handle, void *reserved)
{
    tmpfs_inode_t *root = INODE_FROM_ATTR(handle);
    tmpfs_mount_t *mnt  = root->mount;
    mode_t         mode = msg->connect.mode;

    tmpfs_inode_t *parent  = NULL;
    const char    *basename = NULL;
    tmpfs_inode_t *existing = tmpfs_dir_walk(root, msg->connect.path, 0,
                                              &parent, &basename);
    if (existing != NULL)
        return EEXIST;
    if (parent == NULL || basename == NULL)
        return ENOENT;

    struct _client_info cinfo;
    iofunc_client_info(ctp, 0, &cinfo);

    int rc = tmpfs_check_access(&parent->attr, W_OK, &cinfo);
    if (rc != EOK) return rc;

    tmpfs_inode_t *ino = tmpfs_inode_alloc(mnt, mode, &cinfo);
    if (ino == NULL) return errno;

    if (S_ISREG(mode)) {
        rc = tmpfs_file_open_shm(ino);
        if (rc != EOK) { tmpfs_inode_free(ino); return rc; }
    } else if (S_ISDIR(mode)) {
        ino->attr.nlink = 2;
        ino->parent     = parent;
        /* Increment parent link count for ".." */
        iofunc_attr_lock(&parent->attr);
        parent->attr.nlink++;
        iofunc_attr_unlock(&parent->attr);
    }

    iofunc_attr_lock(&parent->attr);
    rc = tmpfs_dir_insert(parent, basename, ino);
    iofunc_attr_unlock(&parent->attr);

    if (rc != EOK) {
        if (S_ISDIR(mode)) {
            iofunc_attr_lock(&parent->attr);
            if (parent->attr.nlink > 0) parent->attr.nlink--;
            iofunc_attr_unlock(&parent->attr);
        }
        tmpfs_inode_free(ino);
        return rc;
    }

    return EOK;
}

/*
 * tmpfs_connect_readlink
 */
int tmpfs_connect_readlink(resmgr_context_t *ctp, io_readlink_t *msg,
                            iofunc_attr_t *handle, void *reserved)
{
    tmpfs_inode_t *root = INODE_FROM_ATTR(handle);

    tmpfs_inode_t *ino = tmpfs_dir_walk(root, msg->connect.path, 0,
                                         NULL, NULL);
    if (ino == NULL)
        return ENOENT;
    if (!S_ISLNK(ino->attr.mode))
        return EINVAL;

    const char *target = ino->symlink_target ? ino->symlink_target : "";
    size_t      tlen   = strlen(target);

    /*
     * Reply with the link_reply header + target string.
     * Status must be EOK (not _IO_CONNECT_RET_LINK) so the pathmgr
     * returns the path string to the readlink() caller rather than
     * following the link.
     */
    struct _io_connect_link_reply *rep =
        (struct _io_connect_link_reply *)msg;
    memset(rep, 0, sizeof(*rep));
    rep->nentries = 0;
    rep->path_len = (uint16_t)(tlen + 1);
    memcpy((char *)rep + sizeof(*rep), target, tlen + 1);

    iov_t rl_iov;
    SETIOV(&rl_iov, rep, sizeof(*rep) + tlen + 1);
    MsgReplyv(ctp->rcvid, EOK, &rl_iov, 1);
    return _RESMGR_NOREPLY;
}

/*
 * tmpfs_connect_link
 *
 * Handles both hard links (_IO_CONNECT_EXTRA_LINK) and
 * symlink creation (_IO_CONNECT_EXTRA_SYMLINK).
 */
int tmpfs_connect_link(resmgr_context_t *ctp, io_link_t *msg,
                        iofunc_attr_t *handle, io_link_extra_t *extra)
{
    tmpfs_inode_t *root = INODE_FROM_ATTR(handle);

    struct _client_info cinfo;
    iofunc_client_info(ctp, 0, &cinfo);

    if (msg->connect.extra_type == _IO_CONNECT_EXTRA_SYMLINK) {
        /* Symlink: path = link name, extra->path = target */
        tmpfs_inode_t *parent  = NULL;
        const char    *basename = NULL;
        tmpfs_inode_t *existing = tmpfs_dir_walk(root, msg->connect.path, 0,
                                                  &parent, &basename);
        if (existing != NULL) return EEXIST;
        if (parent == NULL || basename == NULL) return ENOENT;

        int rc = tmpfs_check_access(&parent->attr, W_OK, &cinfo);
        if (rc != EOK) return rc;

        iofunc_attr_lock(&parent->attr);
        rc = tmpfs_symlink_create(parent, basename, extra->path, &cinfo);
        iofunc_attr_unlock(&parent->attr);
        return rc;

    } else {
        /*
         * Hard link:
         *   connect.path = new link name (destination, relative to mount root)
         *   extra->path  = source file path (relative to mount root)
         */
        tmpfs_inode_t *dst_parent = NULL;
        const char    *dst_base   = NULL;
        tmpfs_inode_t *existing   = tmpfs_dir_walk(root, msg->connect.path, 0,
                                                    &dst_parent, &dst_base);
        if (existing != NULL) return EEXIST;
        if (dst_parent == NULL || dst_base == NULL) return ENOENT;
        if (!extra) return EINVAL;

        tmpfs_inode_t *src = tmpfs_dir_walk(root, extra->path, 1,
                                             NULL, NULL);
        if (src == NULL) return ENOENT;
        if (S_ISDIR(src->attr.mode)) return EPERM;

        int rc = tmpfs_check_access(&dst_parent->attr, W_OK, &cinfo);
        if (rc != EOK) return rc;

        iofunc_attr_lock(&src->attr);
        src->attr.nlink++;
        iofunc_attr_unlock(&src->attr);

        iofunc_attr_lock(&dst_parent->attr);
        rc = tmpfs_dir_insert(dst_parent, dst_base, src);
        iofunc_attr_unlock(&dst_parent->attr);

        if (rc != EOK) {
            iofunc_attr_lock(&src->attr);
            src->attr.nlink--;
            iofunc_attr_unlock(&src->attr);
            return rc;
        }
        return EOK;
    }
}

/* =========================================================================
 * IO HANDLERS
 * ====================================================================== */

/*
 * tmpfs_io_read
 *
 * Handles both regular file reads and directory reads (readdir).
 */
int tmpfs_io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb)
{
    tmpfs_ocb_t   *tocb = TMPFS_OCB(ocb);
    tmpfs_inode_t *ino  = tocb->inode;

    int rc = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (rc != EOK) return rc;

    /* ---- DIRECTORY READ (readdir) ---- */
    if (S_ISDIR(ino->attr.mode)) {
        /* We need a buffer to pack dirent records into.
         * The QNX readdir xtype tells us the client wants dirents. */
        if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_READDIR
            && (msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
            return EBADF;

        uint32_t pos     = tocb->dir_pos;
        uint32_t nbytes  = msg->i.nbytes;
        char    *outbuf  = alloca(nbytes);
        uint32_t written = 0;

        iofunc_attr_lock(&ino->attr);

        while (written < nbytes) {
            struct dirent *de = (struct dirent *)(outbuf + written);
            size_t remaining  = nbytes - written;

            if (pos == 0) {
                /* Entry: "." */
                size_t namelen = 1;
                size_t reclen  = ((offsetof(struct dirent, d_name) +
                                   namelen + 1 + 7) & ~(size_t)7);
                if (remaining < reclen) break;
                memset(de, 0, reclen);
                de->d_ino    = ino->attr.inode;
                de->d_offset = pos;
                de->d_reclen = (int16_t)reclen;
                de->d_namelen= (int16_t)namelen;
                memcpy(de->d_name, ".", 2);
                written += reclen;
                pos++;

            } else if (pos == 1) {
                /* Entry: ".." */
                tmpfs_inode_t *p = ino->parent ? ino->parent : ino;
                size_t namelen   = 2;
                size_t reclen    = ((offsetof(struct dirent, d_name) +
                                     namelen + 1 + 7) & ~(size_t)7);
                if (remaining < reclen) break;
                memset(de, 0, reclen);
                de->d_ino     = p->attr.inode;
                de->d_offset  = pos;
                de->d_reclen  = (int16_t)reclen;
                de->d_namelen = (int16_t)namelen;
                memcpy(de->d_name, "..", 3);
                written += reclen;
                pos++;

            } else {
                /* Real children */
                uint32_t child_idx = pos - 2;
                tmpfs_dirent_t *child_de = tmpfs_dir_get_nth(ino, child_idx);
                if (child_de == NULL)
                    break; /* end of directory */

                size_t namelen = strlen(child_de->name);
                size_t reclen  = ((offsetof(struct dirent, d_name) +
                                   namelen + 1 + 7) & ~(size_t)7);
                if (remaining < reclen) break;
                memset(de, 0, reclen);
                de->d_ino     = child_de->inode->attr.inode;
                de->d_offset  = pos;
                de->d_reclen  = (int16_t)reclen;
                de->d_namelen = (int16_t)namelen;
                memcpy(de->d_name, child_de->name, namelen + 1);
                written += reclen;
                pos++;
            }
        }

        iofunc_attr_unlock(&ino->attr);
        tocb->dir_pos = pos;

        if (written == 0) {
            /* Signal EOF */
            MsgReply(ctp->rcvid, 0, NULL, 0);
            return _RESMGR_NOREPLY;
        }

        MsgReply(ctp->rcvid, written, outbuf, written);
        return _RESMGR_NOREPLY;
    }

    /* ---- REGULAR FILE READ ---- */
    if (!S_ISREG(ino->attr.mode))
        return EBADF;

    uint32_t nbytes = msg->i.nbytes;
    off_t    offset = ocb->offset;

    iofunc_attr_lock(&ino->attr);
    off_t file_size = ino->attr.nbytes;

    if (offset >= file_size || nbytes == 0) {
        iofunc_attr_unlock(&ino->attr);
        MsgReply(ctp->rcvid, 0, NULL, 0);
        return _RESMGR_NOREPLY;
    }

    if ((off_t)nbytes > file_size - offset)
        nbytes = (uint32_t)(file_size - offset);

    void *buf = malloc(nbytes);
    if (buf == NULL) {
        iofunc_attr_unlock(&ino->attr);
        return ENOMEM;
    }

    ssize_t nread = tmpfs_file_read(ino, buf, nbytes, offset);
    iofunc_attr_unlock(&ino->attr);

    if (nread < 0) {
        free(buf);
        return EIO;
    }

    ocb->offset += nread;
    iofunc_attr_lock(&ino->attr);
    iofunc_time_update(&ino->attr);
    iofunc_attr_unlock(&ino->attr);

    MsgReply(ctp->rcvid, nread, buf, (size_t)nread);
    free(buf);
    return _RESMGR_NOREPLY;
}

/*
 * tmpfs_io_write
 */
int tmpfs_io_write(resmgr_context_t *ctp, io_write_t *msg, iofunc_ocb_t *ocb)
{
    tmpfs_inode_t *ino = INODE_FROM_OCB(ocb);

    int rc = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (rc != EOK) return rc;

    if (!S_ISREG(ino->attr.mode))
        return EINVAL;

    uint32_t nbytes = msg->i.nbytes;
    if (nbytes == 0) {
        _IO_SET_WRITE_NBYTES(ctp, 0);
        return EOK;
    }

    /* The write data follows the message header */
    void *data = malloc(nbytes);
    if (data == NULL)
        return ENOMEM;

    rc = MsgRead(ctp->rcvid, data, nbytes, sizeof(msg->i));
    if (rc < 0) {
        free(data);
        return EIO;
    }

    off_t offset = (ocb->ioflag & O_APPEND) ? ino->attr.nbytes : ocb->offset;

    iofunc_attr_lock(&ino->attr);
    ssize_t nwritten = tmpfs_file_write(ino, data, nbytes, offset);
    iofunc_attr_unlock(&ino->attr);
    free(data);

    if (nwritten < 0)
        return errno;

    ocb->offset += nwritten;
    _IO_SET_WRITE_NBYTES(ctp, nwritten);
    return EOK;
}

/*
 * tmpfs_io_close_ocb
 *
 * Save the inode pointer BEFORE calling iofunc_close_ocb_default
 * (which calls ocb_free and frees the OCB). Then unref the inode
 * after the lock is released.
 */
int tmpfs_io_close_ocb(resmgr_context_t *ctp, void *reserved, iofunc_ocb_t *ocb)
{
    tmpfs_inode_t *ino = INODE_FROM_OCB(ocb);  /* save before OCB is freed */

    /* This calls ocb_free (which frees the OCB) then releases attr.lock */
    int rc = iofunc_close_ocb_default(ctp, reserved, ocb);

    /* OCB is now freed. Unref the inode (attr.lock is released by now). */
    iofunc_attr_lock(&ino->attr);
    int freed = tmpfs_inode_unref(ino);
    if (!freed)
        iofunc_attr_unlock(&ino->attr);
    /* If freed==1, tmpfs_inode_free already ran (which also unlocks is not needed
     * since the inode struct is gone). The iofunc_attr_t mutex was destroyed
     * as part of the inode free -- do NOT unlock after free. */
    return rc;
}

/*
 * tmpfs_io_space  --  handles _IO_SPACE: truncate (F_FREESP/F_GROWSP) and
 *                     fallocate-style reservation (F_ALLOCSP)
 */
int tmpfs_io_space(resmgr_context_t *ctp, io_space_t *msg, iofunc_ocb_t *ocb)
{
    tmpfs_inode_t *ino = INODE_FROM_OCB(ocb);
    if (!S_ISREG(ino->attr.mode))
        return EINVAL;

    int subtype = msg->i.subtype & 0xff;
    int rc;

    iofunc_attr_lock(&ino->attr);
    if (subtype == F_FREESP || subtype == F_FREESP64) {
        /* Truncate: new size = start + len (len==0 means truncate to start) */
        off_t new_size = (off_t)msg->i.start;
        if (msg->i.len > 0)
            new_size += (off_t)msg->i.len;
        rc = tmpfs_file_truncate(ino, new_size);
    } else if (subtype == F_GROWSP || subtype == F_GROWSP64) {
        /* Grow to start+len */
        off_t new_size = (off_t)(msg->i.start + msg->i.len);
        if (new_size > ino->attr.nbytes)
            rc = tmpfs_file_truncate(ino, new_size);
        else
            rc = EOK;
    } else {
        /* F_ALLOCSP: just ensure capacity without changing visible size */
        size_t needed = (size_t)(msg->i.start + msg->i.len);
        rc = tmpfs_file_ensure_capacity(ino, needed);
    }
    iofunc_attr_unlock(&ino->attr);

    if (rc != EOK) return rc;
    msg->o = (uint64_t)ino->attr.nbytes;
    MsgReply(ctp->rcvid, EOK, &msg->o, sizeof(msg->o));
    return _RESMGR_NOREPLY;
}

/* =========================================================================
 * DISPATCH / THREAD POOL SETUP
 * ====================================================================== */

/*
 * tmpfs_resmgr_init
 *
 * Create the dispatch handle, register function tables, and start the
 * thread pool.  Called once from main().
 */
int tmpfs_resmgr_init(void)
{
    /* Create dispatch context */
    g_tmpfs.dpp = dispatch_create();
    if (g_tmpfs.dpp == NULL)
        return errno;

    /* Initialise function tables with safe defaults */
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect_funcs,
                     _RESMGR_IO_NFUNCS,      &g_io_funcs);

    /* Override connect handlers */
    g_connect_funcs.open     = tmpfs_connect_open;
    g_connect_funcs.unlink   = tmpfs_connect_unlink;
    g_connect_funcs.rename   = tmpfs_connect_rename;
    g_connect_funcs.mknod    = tmpfs_connect_mknod;
    g_connect_funcs.readlink = tmpfs_connect_readlink;
    g_connect_funcs.link     = tmpfs_connect_link;

    /* Override IO handlers */
    g_io_funcs.read      = tmpfs_io_read;
    g_io_funcs.write     = tmpfs_io_write;
    g_io_funcs.close_ocb = tmpfs_io_close_ocb;
    g_io_funcs.stat      = iofunc_stat_default;
    g_io_funcs.lseek     = iofunc_lseek_default;
    g_io_funcs.chmod     = iofunc_chmod_default;
    g_io_funcs.chown     = iofunc_chown_default;
    g_io_funcs.utime     = iofunc_utime_default;
    g_io_funcs.mmap      = tmpfs_io_mmap;
    g_io_funcs.sync      = tmpfs_io_sync;
    g_io_funcs.space     = tmpfs_io_space;
    g_io_funcs.lock_ocb  = iofunc_lock_ocb_default;
    g_io_funcs.unlock_ocb= iofunc_unlock_ocb_default;

    /* Thread pool */
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus <= 0) ncpus = 1;

    thread_pool_attr_t tpattr;
    memset(&tpattr, 0, sizeof(tpattr));
    tpattr.handle         = g_tmpfs.dpp;
    tpattr.context_alloc  = resmgr_context_alloc;
    tpattr.block_func     = resmgr_block;
    tpattr.handler_func   = resmgr_handler;
    tpattr.context_free   = resmgr_context_free;
    tpattr.lo_water      = TMPFS_POOL_LO_WATER;
    tpattr.hi_water      = (int)(ncpus * TMPFS_POOL_HI_WATER_PER_CPU);
    tpattr.increment     = TMPFS_POOL_INCREMENT;
    tpattr.maximum       = TMPFS_POOL_MAX;

    g_tmpfs.pool = thread_pool_create(&tpattr, POOL_FLAG_EXIT_SELF);
    if (g_tmpfs.pool == NULL)
        return errno;

    return EOK;
}

/*
 * tmpfs_resmgr_start
 *
 * Start the thread pool (blocks until the pool is destroyed).
 */
int tmpfs_resmgr_start(void)
{
    return thread_pool_start(g_tmpfs.pool);
}
