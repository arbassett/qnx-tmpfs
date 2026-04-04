/*
 * mount.h  --  mount lifecycle interface
 */
#ifndef TMPFS_MOUNT_H
#define TMPFS_MOUNT_H

#include "../include/tmpfs_internal.h"
#include "../include/tmpfs_ipc.h"

int            tmpfs_parse_size(const char *str, uint64_t total_ram,
                                uint64_t *out);
int            tmpfs_mount_add(const tmpfs_mount_req_t *req);
int            tmpfs_mount_remove(const char *path);
tmpfs_mount_t *tmpfs_mount_find_by_id(int resmgr_id);

#endif /* TMPFS_MOUNT_H */
