/*
 * inode.h  --  inode lifecycle interface
 */
#ifndef TMPFS_INODE_H
#define TMPFS_INODE_H

#include <sys/types.h>
#include "../include/tmpfs_internal.h"

tmpfs_inode_t *tmpfs_inode_alloc(tmpfs_mount_t *mnt, mode_t mode,
                                  struct _client_info *cinfo);
tmpfs_inode_t *tmpfs_inode_alloc_root(tmpfs_mount_t *mnt, uid_t uid,
                                       gid_t gid, mode_t mode);
void           tmpfs_inode_ref(tmpfs_inode_t *ino);
int            tmpfs_inode_unref(tmpfs_inode_t *ino); /* returns 1 if freed */
void           tmpfs_inode_free(tmpfs_inode_t *ino);

#endif /* TMPFS_INODE_H */
