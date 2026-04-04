/*
 * file.h  --  regular file operations interface
 */
#ifndef TMPFS_FILE_H
#define TMPFS_FILE_H

#include <sys/types.h>
#include "../include/tmpfs_internal.h"

int     tmpfs_file_open_shm(tmpfs_inode_t *ino);
int     tmpfs_file_ensure_capacity(tmpfs_inode_t *ino, size_t needed);
ssize_t tmpfs_file_read(tmpfs_inode_t *ino, void *buf, size_t nbytes, off_t offset);
ssize_t tmpfs_file_write(tmpfs_inode_t *ino, const void *buf, size_t nbytes, off_t offset);
int     tmpfs_file_truncate(tmpfs_inode_t *ino, off_t new_size);

#endif /* TMPFS_FILE_H */
