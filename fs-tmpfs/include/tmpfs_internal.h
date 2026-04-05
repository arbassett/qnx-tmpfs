/*
 * tmpfs_internal.h  --  Internal data structures for fs-tmpfs.
 *
 * NOT for inclusion by external CLI tools -- use tmpfs_ipc.h instead.
 */

#ifndef TMPFS_INTERNAL_H
#define TMPFS_INTERNAL_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/iofunc.h>
#include <sys/resmgr.h>

/* Must be defined before dispatch.h so thread_pool_attr_t uses correct types */
#ifndef THREAD_POOL_PARAM_T
# define THREAD_POOL_PARAM_T    resmgr_context_t
#endif
#ifndef THREAD_POOL_HANDLE_T
# define THREAD_POOL_HANDLE_T   dispatch_t
#endif
#include <sys/dispatch.h>
#include <sys/types.h>
#include <time.h>

#include "fs-tmpfs.h"
#include "tmpfs_ipc.h"

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */
typedef struct tmpfs_global    tmpfs_global_t;
typedef struct tmpfs_mount     tmpfs_mount_t;
typedef struct tmpfs_inode     tmpfs_inode_t;
typedef struct tmpfs_ocb       tmpfs_ocb_t;
typedef struct tmpfs_dirent    tmpfs_dirent_t;

/* -------------------------------------------------------------------------
 * Directory hash entry
 * ---------------------------------------------------------------------- */
struct tmpfs_dirent {
    char            *name;          /* heap-allocated filename          */
    tmpfs_inode_t   *inode;         /* target inode (not owned here)    */
    tmpfs_dirent_t  *next;          /* next in hash bucket chain        */
};

/* -------------------------------------------------------------------------
 * Inode  (iofunc_attr_t MUST be first for QNX framework compatibility)
 * ---------------------------------------------------------------------- */
struct tmpfs_inode {
    /* --- QNX framework fields (must be first) --- */
    iofunc_attr_t       attr;           /* uid, gid, mode, times, size, nlink */

    /* --- tmpfs private fields --- */
    tmpfs_mount_t      *mount;          /* back-pointer to owning mount        */
    uint32_t            ref_count;      /* open OCB count (protected by attr.lock) */

    /* --- FILE specific --- */
    int                 shm_fd;         /* SHM_ANON fd (-1 if not a file)      */
    void               *shm_ptr;        /* server-side mmap of the shm object  */
    size_t              shm_cap;        /* current allocated shm capacity      */

    /* --- DIR specific --- */
    tmpfs_dirent_t     *hash[TMPFS_DIR_HASH_BUCKETS]; /* children hash table   */
    tmpfs_inode_t      *parent;         /* parent directory (NULL for root)    */
    uint32_t            child_count;    /* number of directory entries         */

    /* --- SYMLINK specific --- */
    char               *symlink_target; /* heap-allocated target path          */
};

/* -------------------------------------------------------------------------
 * Open Control Block  (iofunc_ocb_t MUST be first)
 * ---------------------------------------------------------------------- */
struct tmpfs_ocb {
    iofunc_ocb_t        ocb;            /* must be first — QNX framework uses this */
    tmpfs_inode_t      *inode;          /* the inode this OCB refers to        */
    uint32_t            dir_pos;        /* readdir position (0='.', 1='..', 2+=children) */
};

/* -------------------------------------------------------------------------
 * Per-mount state
 * ---------------------------------------------------------------------- */
struct tmpfs_mount {
    /* Accounting */
    size_t              mount_cap;      /* maximum bytes this mount may use    */
    atomic_size_t       mount_used;     /* bytes currently charged to this mount */

    /* Inode limit */
    uint64_t            inode_cap;      /* maximum inodes this mount may hold  */

    /* Statistics counters */
    atomic_uint_fast64_t file_count;
    atomic_uint_fast64_t dir_count;
    atomic_uint_fast64_t symlink_count;
    atomic_uint_fast64_t inode_count;   /* all live inodes (files+dirs+symlinks) */

    /* resmgr attachment */
    int                 resmgr_id;      /* id returned by resmgr_attach()      */
    iofunc_mount_t      iofunc_mount;   /* QNX mount info (blocksize, dev, ...) */

    /* Filesystem tree */
    tmpfs_inode_t      *root;           /* root directory inode                */

    /* Coordinator linked list */
    tmpfs_mount_t      *next;

    /* Mount path (for stats) */
    char                path[PATH_MAX];
};

/* -------------------------------------------------------------------------
 * Global coordinator state
 * ---------------------------------------------------------------------- */
struct tmpfs_global {
    /* Memory budget */
    uint64_t            total_ram;      /* total physical RAM                  */
    uint64_t            global_cap;     /* total_ram / 2                       */
    atomic_size_t       global_used;    /* bytes in use across ALL mounts      */

    /* Mount list */
    tmpfs_mount_t      *mounts;         /* linked list head                    */
    uint32_t            mount_count;
    pthread_rwlock_t    mounts_lock;    /* protects mounts list + mount_count  */

    /* Control device */
    int                 ctrl_resmgr_id; /* /dev/fs-tmpfs resmgr id             */

    /* Dispatch / thread pool */
    dispatch_t         *dpp;
    thread_pool_t      *pool;

    /* IO + connect function tables (shared across all mounts + ctrl) */
    resmgr_connect_funcs_t  connect_funcs;
    resmgr_io_funcs_t       io_funcs;
    resmgr_connect_funcs_t  ctrl_connect_funcs;
    resmgr_io_funcs_t       ctrl_io_funcs;

    /* iofunc attr for the control device */
    iofunc_attr_t       ctrl_attr;
    iofunc_mount_t      ctrl_mount;

    /* Start time (for uptime_ms in stats) */
    struct timespec     start_time;
};

/* -------------------------------------------------------------------------
 * Global instance (defined in main.c, used across all modules)
 * ---------------------------------------------------------------------- */
extern tmpfs_global_t g_tmpfs;

/* -------------------------------------------------------------------------
 * Convenience cast macros
 * ---------------------------------------------------------------------- */

/* Get tmpfs_inode_t* from an iofunc_attr_t* (attr is first member) */
#define INODE_FROM_ATTR(attr_ptr) \
    ((tmpfs_inode_t *)(attr_ptr))

/* Get tmpfs_ocb_t* from an iofunc_ocb_t* (ocb is first member) */
#define TMPFS_OCB(ocb_ptr) \
    ((tmpfs_ocb_t *)(ocb_ptr))

/* Get tmpfs_inode_t* from an iofunc_ocb_t* */
#define INODE_FROM_OCB(ocb_ptr) \
    (TMPFS_OCB(ocb_ptr)->inode)

#endif /* TMPFS_INTERNAL_H */
