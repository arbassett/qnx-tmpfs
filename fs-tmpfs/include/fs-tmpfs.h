/*
 * fs-tmpfs.h  --  compile-time constants and shared configuration
 *
 * This header is included by every source file in the driver.
 * It must not pull in internal struct definitions (see tmpfs_internal.h).
 */

#ifndef FS_TMPFS_H
#define FS_TMPFS_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>

/* -------------------------------------------------------------------------
 * Version
 * ---------------------------------------------------------------------- */
#define TMPFS_VERSION_MAJOR     1
#define TMPFS_VERSION_MINOR     0
#define TMPFS_VERSION_PATCH     0

/* -------------------------------------------------------------------------
 * Memory limits
 * ---------------------------------------------------------------------- */

/* Fraction of total RAM used as the global cap (50%) */
#define TMPFS_GLOBAL_RAM_FRACTION       2

/* Default per-mount cap when -o size= is not specified (25% of total RAM) */
#define TMPFS_DEFAULT_MOUNT_FRACTION    4

/* Minimum mount size we'll accept (1 MB) */
#define TMPFS_MIN_MOUNT_SIZE            (1ULL * 1024 * 1024)

/* Maximum mount size percentage allowed (50 - must fit inside global cap) */
#define TMPFS_MAX_MOUNT_PERCENT         50

/* -------------------------------------------------------------------------
 * Inode limits
 * ---------------------------------------------------------------------- */

/*
 * Default per-mount inode cap when -o nr_inodes= is not specified.
 * Mirrors Linux tmpfs: half the number of physical RAM pages.
 * total_ram_pages = total_ram / page_size; default = total_ram_pages / 2.
 * Expressed as a fraction of total_ram: default = total_ram / (page_size * 2).
 * Page size is fixed at 4096 on QNX 8 aarch64.
 */
#define TMPFS_PAGE_SIZE                 4096ULL
#define TMPFS_DEFAULT_INODES_DENOM      (TMPFS_PAGE_SIZE * 2)  /* total_ram / this */

/* Hard floor: always allow at least this many inodes per mount */
#define TMPFS_MIN_INODES                16ULL

/* -------------------------------------------------------------------------
 * SHM backing store growth policy
 * ---------------------------------------------------------------------- */

/* Initial backing capacity for a new file (one page) */
#define TMPFS_SHM_INITIAL_SIZE          4096ULL

/* Double capacity up to this point, then grow linearly */
#define TMPFS_SHM_DOUBLE_THRESHOLD      (1ULL * 1024 * 1024)   /* 1 MB */

/* Linear growth increment above the threshold */
#define TMPFS_SHM_LINEAR_INCREMENT      (1ULL * 1024 * 1024)   /* 1 MB */

/* Shrink backing store when file size drops below capacity / this factor */
#define TMPFS_SHM_SHRINK_RATIO          4

/* -------------------------------------------------------------------------
 * Thread pool sizing
 * ---------------------------------------------------------------------- */
#define TMPFS_POOL_LO_WATER     2
#define TMPFS_POOL_HI_WATER_PER_CPU 2   /* hi_water = ncpus * this */
#define TMPFS_POOL_INCREMENT    1
#define TMPFS_POOL_MAX          32

/* -------------------------------------------------------------------------
 * Directory hash table
 * ---------------------------------------------------------------------- */
#define TMPFS_DIR_HASH_BUCKETS  16      /* power-of-two for fast modulo */

/* -------------------------------------------------------------------------
 * Coordinator control device
 * ---------------------------------------------------------------------- */
#define TMPFS_CTRL_PATH         "/dev/fs-tmpfs"

/* Grace period (ms) after last unmount before the daemon self-exits */
#define TMPFS_DRAIN_GRACE_MS    100

/* -------------------------------------------------------------------------
 * Symlink recursion limit
 * ---------------------------------------------------------------------- */
#define TMPFS_SYMLINK_MAX_DEPTH 8

/* -------------------------------------------------------------------------
 * Inode number base (root gets 1, others are atomically incremented) */
#define TMPFS_ROOT_INO          1

/* -------------------------------------------------------------------------
 * Misc
 * ---------------------------------------------------------------------- */
#define TMPFS_CTRL_DEVNO_MAJOR  200     /* Arbitrary major for control dev */
#define TMPFS_CTRL_DEVNO_MINOR  0

/* Metadata overhead charged per inode (approximation for accounting) */
#define TMPFS_INODE_OVERHEAD    256

/* Metadata overhead charged per directory entry (name len accounted separately) */
#define TMPFS_DIRENT_OVERHEAD   64

#endif /* FS_TMPFS_H */
