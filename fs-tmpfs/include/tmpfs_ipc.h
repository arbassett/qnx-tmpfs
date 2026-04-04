/*
 * tmpfs_ipc.h  --  Public IPC interface between fs-tmpfs and CLI tools.
 *
 * This is the ONLY header CLI tools need to include.
 * It contains the devctl command codes and statistics structures.
 * No internal implementation details live here.
 */

#ifndef TMPFS_IPC_H
#define TMPFS_IPC_H

#include <stdint.h>
#include <limits.h>
#include <devctl.h>

/* -------------------------------------------------------------------------
 * devctl class  (use MISC range to avoid clashes with system FS commands)
 * ---------------------------------------------------------------------- */
#define _DCMD_TMPFS     0x54        /* 'T' */

/* -------------------------------------------------------------------------
 * Commands
 *
 * DCMD_TMPFS_ADD_MOUNT  -- ask coordinator to attach a new mount point
 * DCMD_TMPFS_DEL_MOUNT  -- ask coordinator to detach a mount point
 * DCMD_TMPFS_GET_STATS  -- retrieve global statistics snapshot
 * DCMD_TMPFS_GET_MOUNT  -- retrieve per-mount statistics (path in input)
 * ---------------------------------------------------------------------- */
#define DCMD_TMPFS_ADD_MOUNT    __DIOT(_DCMD_TMPFS, 1, tmpfs_mount_req_t)
#define DCMD_TMPFS_DEL_MOUNT    __DIOT(_DCMD_TMPFS, 2, tmpfs_del_req_t)
#define DCMD_TMPFS_GET_STATS    __DIOF(_DCMD_TMPFS, 3, tmpfs_global_stats_t)
#define DCMD_TMPFS_GET_MOUNT    __DIOTF(_DCMD_TMPFS, 4, tmpfs_mount_stats_t)

/* -------------------------------------------------------------------------
 * Request structures (sent TO the coordinator)
 * ---------------------------------------------------------------------- */

/* Options parsed from -o size= */
typedef struct tmpfs_size_opt {
    uint64_t    bytes;          /* resolved size in bytes */
} tmpfs_size_opt_t;

typedef struct tmpfs_mount_req {
    char                path[PATH_MAX];     /* mount point path          */
    tmpfs_size_opt_t    size_opt;           /* 0 = use default           */
    uid_t               uid;               /* mounting user             */
    gid_t               gid;               /* mounting group            */
    mode_t              mode;              /* root dir permissions      */
} tmpfs_mount_req_t;

typedef struct tmpfs_del_req {
    char    path[PATH_MAX];     /* mount point path to detach */
} tmpfs_del_req_t;

/* -------------------------------------------------------------------------
 * Statistics structures (received FROM the coordinator)
 * ---------------------------------------------------------------------- */

typedef struct tmpfs_global_stats {
    uint32_t    version_major;          /* driver version               */
    uint32_t    version_minor;
    uint32_t    version_patch;
    uint32_t    _pad;
    uint64_t    total_ram;              /* total system RAM, bytes      */
    uint64_t    global_cap;             /* 50% of total_ram             */
    uint64_t    global_used;            /* bytes in use across all mounts */
    uint32_t    mount_count;            /* number of active mounts      */
    uint32_t    _pad2;
    uint64_t    uptime_ms;              /* ms since coordinator started */
} tmpfs_global_stats_t;

typedef struct tmpfs_mount_stats {
    /* Input: fill path before calling DCMD_TMPFS_GET_MOUNT */
    char        path[PATH_MAX];         /* mount point path             */

    /* Output */
    uint64_t    mount_cap;              /* per-mount size limit, bytes  */
    uint64_t    mount_used;             /* bytes currently in use       */
    uint64_t    file_count;             /* number of regular files      */
    uint64_t    dir_count;              /* number of directories        */
    uint64_t    symlink_count;          /* number of symlinks           */
    uint64_t    inode_count;            /* total inodes (all types)     */
} tmpfs_mount_stats_t;

#endif /* TMPFS_IPC_H */
