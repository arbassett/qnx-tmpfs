/*
 * memory.c  --  Two-level memory accounting (global cap + per-mount cap).
 *
 * Every allocation in the driver must go through tmpfs_mem_reserve() and
 * every free through tmpfs_mem_release().  Nothing else touches the counters.
 */

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>

#include "../include/tmpfs_internal.h"

/*
 * tmpfs_mem_reserve  --  attempt to reserve `delta` bytes.
 *
 * Checks both the per-mount cap and the global cap atomically.
 * Uses a compare-exchange loop to ensure we never overshoot either limit.
 *
 * Returns 0 on success, ENOSPC if either cap would be exceeded.
 */
int tmpfs_mem_reserve(tmpfs_mount_t *mnt, size_t delta)
{
    size_t old_global, new_global;
    size_t old_mount,  new_mount;

    if (delta == 0)
        return 0;

    /*
     * We need to atomically reserve from BOTH counters.
     * Strategy: reserve global first (optimistic CAS loop), then mount.
     * If mount fails, release global immediately.
     */

    /* --- 1. Global cap CAS loop --- */
    old_global = atomic_load_explicit(&g_tmpfs.global_used, memory_order_relaxed);
    do {
        if (old_global > g_tmpfs.global_cap ||
            delta > g_tmpfs.global_cap - old_global) {
            return ENOSPC;
        }
        new_global = old_global + delta;
    } while (!atomic_compare_exchange_weak_explicit(
                &g_tmpfs.global_used,
                &old_global, new_global,
                memory_order_acquire,
                memory_order_relaxed));

    /* --- 2. Per-mount cap CAS loop --- */
    old_mount = atomic_load_explicit(&mnt->mount_used, memory_order_relaxed);
    do {
        if (old_mount > mnt->mount_cap ||
            delta > mnt->mount_cap - old_mount) {
            /* Mount cap exceeded -- roll back global reservation */
            atomic_fetch_sub_explicit(&g_tmpfs.global_used, delta,
                                      memory_order_release);
            return ENOSPC;
        }
        new_mount = old_mount + delta;
    } while (!atomic_compare_exchange_weak_explicit(
                &mnt->mount_used,
                &old_mount, new_mount,
                memory_order_acquire,
                memory_order_relaxed));

    return 0;
}

/*
 * tmpfs_mem_release  --  release `delta` bytes back to both counters.
 *
 * Safe to call with delta == 0.
 */
void tmpfs_mem_release(tmpfs_mount_t *mnt, size_t delta)
{
    if (delta == 0)
        return;

    atomic_fetch_sub_explicit(&mnt->mount_used,    delta, memory_order_release);
    atomic_fetch_sub_explicit(&g_tmpfs.global_used, delta, memory_order_release);
}

/*
 * tmpfs_mem_used_global  --  snapshot of current global usage (advisory).
 */
size_t tmpfs_mem_used_global(void)
{
    return atomic_load_explicit(&g_tmpfs.global_used, memory_order_relaxed);
}

/*
 * tmpfs_mem_used_mount  --  snapshot of current per-mount usage (advisory).
 */
size_t tmpfs_mem_used_mount(const tmpfs_mount_t *mnt)
{
    return atomic_load_explicit(&mnt->mount_used, memory_order_relaxed);
}

/*
 * tmpfs_inode_reserve  --  claim one inode slot against inode_cap.
 *
 * Uses a CAS loop so no inode is ever created that would push inode_count
 * past inode_cap, even under concurrent allocation.
 *
 * Returns 0 on success, ENOSPC when the cap is reached.
 */
int tmpfs_inode_reserve(tmpfs_mount_t *mnt)
{
    uint_fast64_t old, new_val;

    old = atomic_load_explicit(&mnt->inode_count, memory_order_relaxed);
    do {
        if (old >= mnt->inode_cap)
            return ENOSPC;
        new_val = old + 1;
    } while (!atomic_compare_exchange_weak_explicit(
                &mnt->inode_count,
                &old, new_val,
                memory_order_acquire,
                memory_order_relaxed));

    return 0;
}

/*
 * tmpfs_inode_release  --  return one inode slot to the pool.
 */
void tmpfs_inode_release(tmpfs_mount_t *mnt)
{
    atomic_fetch_sub_explicit(&mnt->inode_count, 1, memory_order_release);
}
