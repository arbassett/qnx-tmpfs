/*
 * control.c  --  /dev/fs-tmpfs pseudo-device.
 *
 * Provides the coordinator control channel used by:
 *   - Secondary mount invocations (DCMD_TMPFS_ADD_MOUNT)
 *   - Programmatic unmount    (DCMD_TMPFS_DEL_MOUNT)
 *   - CLI stats tools         (DCMD_TMPFS_GET_STATS, DCMD_TMPFS_GET_MOUNT)
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/iofunc.h>
#include <sys/resmgr.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>

#include "../include/tmpfs_internal.h"
#include "../include/tmpfs_ipc.h"
#include "mount.h"
#include "control.h"

/* -------------------------------------------------------------------------
 * Helper: milliseconds since coordinator start
 * ---------------------------------------------------------------------- */
static uint64_t uptime_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ms  = (uint64_t)now.tv_sec  * 1000 + now.tv_nsec  / 1000000;
    uint64_t base_ms = (uint64_t)g_tmpfs.start_time.tv_sec  * 1000
                     + g_tmpfs.start_time.tv_nsec / 1000000;
    return now_ms - base_ms;
}

/* -------------------------------------------------------------------------
 * ctrl_io_devctl  --  handle devctl commands on /dev/fs-tmpfs
 * ---------------------------------------------------------------------- */
int ctrl_io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb)
{
    int  cmd    = msg->i.dcmd;
    int  status = EOK;

    switch (cmd) {

    /* ------------------------------------------------------------------ */
    case DCMD_TMPFS_ADD_MOUNT: {
        tmpfs_mount_req_t *req = (tmpfs_mount_req_t *)_DEVCTL_DATA(msg->i);
        if (msg->i.nbytes < sizeof(*req))
            return EINVAL;
        req->path[PATH_MAX - 1] = '\0';

        status = tmpfs_mount_add(req);
        msg->o.ret_val = status;
        msg->o.nbytes  = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    /* ------------------------------------------------------------------ */
    case DCMD_TMPFS_DEL_MOUNT: {
        tmpfs_del_req_t *req = (tmpfs_del_req_t *)_DEVCTL_DATA(msg->i);
        if (msg->i.nbytes < sizeof(*req))
            return EINVAL;
        req->path[PATH_MAX - 1] = '\0';

        status = tmpfs_mount_remove(req->path);
        msg->o.ret_val = status;
        msg->o.nbytes  = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    /* ------------------------------------------------------------------ */
    case DCMD_TMPFS_GET_STATS: {
        tmpfs_global_stats_t *stats = (tmpfs_global_stats_t *)_DEVCTL_DATA(msg->o);
        memset(stats, 0, sizeof(*stats));

        stats->version_major = TMPFS_VERSION_MAJOR;
        stats->version_minor = TMPFS_VERSION_MINOR;
        stats->version_patch = TMPFS_VERSION_PATCH;
        stats->total_ram     = g_tmpfs.total_ram;
        stats->global_cap    = g_tmpfs.global_cap;
        stats->global_used   = (uint64_t)tmpfs_mem_used_global();
        stats->uptime_ms     = uptime_ms();

        pthread_rwlock_rdlock(&g_tmpfs.mounts_lock);
        stats->mount_count   = g_tmpfs.mount_count;
        pthread_rwlock_unlock(&g_tmpfs.mounts_lock);

        msg->o.ret_val  = EOK;
        msg->o.nbytes   = sizeof(*stats);
        return _RESMGR_PTR(ctp, &msg->o,
                           sizeof(msg->o) + sizeof(*stats));
    }

    /* ------------------------------------------------------------------ */
    case DCMD_TMPFS_GET_MOUNT: {
        tmpfs_mount_stats_t *req = (tmpfs_mount_stats_t *)_DEVCTL_DATA(msg->i);
        char path[PATH_MAX];
        strncpy(path, req->path, PATH_MAX - 1);
        path[PATH_MAX - 1] = '\0';

        pthread_rwlock_rdlock(&g_tmpfs.mounts_lock);
        tmpfs_mount_t *mnt = g_tmpfs.mounts;
        while (mnt != NULL) {
            if (strcmp(mnt->path, path) == 0)
                break;
            mnt = mnt->next;
        }
        if (mnt == NULL) {
            pthread_rwlock_unlock(&g_tmpfs.mounts_lock);
            return ENOENT;
        }

        tmpfs_mount_stats_t *stats = (tmpfs_mount_stats_t *)_DEVCTL_DATA(msg->o);
        memset(stats, 0, sizeof(*stats));
        strncpy(stats->path, mnt->path, PATH_MAX - 1);
        stats->mount_cap      = mnt->mount_cap;
        stats->mount_used     = (uint64_t)atomic_load(&mnt->mount_used);
        stats->file_count     = (uint64_t)atomic_load(&mnt->file_count);
        stats->dir_count      = (uint64_t)atomic_load(&mnt->dir_count);
        stats->symlink_count  = (uint64_t)atomic_load(&mnt->symlink_count);
        stats->inode_count    = (uint64_t)atomic_load(&mnt->inode_count);
        pthread_rwlock_unlock(&g_tmpfs.mounts_lock);

        msg->o.ret_val  = EOK;
        msg->o.nbytes   = sizeof(*stats);
        return _RESMGR_PTR(ctp, &msg->o,
                           sizeof(msg->o) + sizeof(*stats));
    }

    /* ------------------------------------------------------------------ */
    default:
        return iofunc_devctl_default(ctp, msg, ocb);
    }

    /* unreachable - all cases return directly */
    return EOK;
}

/* -------------------------------------------------------------------------
 * ctrl_io_open  --  allow anyone to open /dev/fs-tmpfs
 * ---------------------------------------------------------------------- */
int ctrl_io_open(resmgr_context_t *ctp, io_open_t *msg,
                 iofunc_attr_t *attr, void *extra)
{
    return iofunc_open_default(ctp, msg, attr, extra);
}

/* -------------------------------------------------------------------------
 * tmpfs_control_init
 *
 * Register /dev/fs-tmpfs with the path manager.
 * Sets up a minimal resmgr that only handles devctl (and open/close).
 * ---------------------------------------------------------------------- */
int tmpfs_control_init(void)
{
    resmgr_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.nparts_max   = 3;
    attr.msg_max_size = sizeof(io_devctl_t) + sizeof(tmpfs_global_stats_t) + 64;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_tmpfs.ctrl_connect_funcs,
                     _RESMGR_IO_NFUNCS,      &g_tmpfs.ctrl_io_funcs);

    g_tmpfs.ctrl_connect_funcs.open   = ctrl_io_open;
    g_tmpfs.ctrl_io_funcs.devctl      = ctrl_io_devctl;

    /* Initialise the control device attribute */
    iofunc_mount_init(&g_tmpfs.ctrl_mount, sizeof(g_tmpfs.ctrl_mount));
    iofunc_attr_init(&g_tmpfs.ctrl_attr, S_IFCHR | 0666, NULL, NULL);
    g_tmpfs.ctrl_attr.mount = &g_tmpfs.ctrl_mount;

    g_tmpfs.ctrl_resmgr_id =
        resmgr_attach(g_tmpfs.dpp,
                      &attr,
                      TMPFS_CTRL_PATH,
                      _FTYPE_ANY,
                      0,
                      &g_tmpfs.ctrl_connect_funcs,
                      &g_tmpfs.ctrl_io_funcs,
                      &g_tmpfs.ctrl_attr);

    if (g_tmpfs.ctrl_resmgr_id == -1)
        return errno;

    return EOK;
}

/* -------------------------------------------------------------------------
 * tmpfs_control_fini  --  deregister /dev/fs-tmpfs
 * ---------------------------------------------------------------------- */
void tmpfs_control_fini(void)
{
    if (g_tmpfs.ctrl_resmgr_id != -1) {
        resmgr_detach(g_tmpfs.dpp, g_tmpfs.ctrl_resmgr_id, _RESMGR_DETACH_ALL);
        g_tmpfs.ctrl_resmgr_id = -1;
    }
}
