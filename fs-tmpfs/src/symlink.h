/*
 * symlink.h  --  symlink interface
 */
#ifndef TMPFS_SYMLINK_H
#define TMPFS_SYMLINK_H

#include <sys/types.h>
#include "../include/tmpfs_internal.h"

int     tmpfs_symlink_create(tmpfs_inode_t *dir, const char *name,
                              const char *target, struct _client_info *cinfo);
ssize_t tmpfs_symlink_read(const tmpfs_inode_t *ino, char *buf, size_t bufsz);

#endif /* TMPFS_SYMLINK_H */
