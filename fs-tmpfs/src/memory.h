/*
 * memory.h  --  memory accounting interface
 */
#ifndef TMPFS_MEMORY_H
#define TMPFS_MEMORY_H

#include <stddef.h>
#include "../include/tmpfs_internal.h"

int    tmpfs_mem_reserve(tmpfs_mount_t *mnt, size_t delta);
void   tmpfs_mem_release(tmpfs_mount_t *mnt, size_t delta);
size_t tmpfs_mem_used_global(void);
size_t tmpfs_mem_used_mount(const tmpfs_mount_t *mnt);

#endif /* TMPFS_MEMORY_H */
