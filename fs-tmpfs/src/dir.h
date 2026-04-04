/*
 * dir.h  --  directory operations interface
 */
#ifndef TMPFS_DIR_H
#define TMPFS_DIR_H

#include "../include/tmpfs_internal.h"

tmpfs_inode_t  *tmpfs_dir_lookup(tmpfs_inode_t *dir, const char *name);
int             tmpfs_dir_insert(tmpfs_inode_t *dir, const char *name,
                                 tmpfs_inode_t *child);
tmpfs_inode_t  *tmpfs_dir_remove(tmpfs_inode_t *dir, const char *name);
tmpfs_dirent_t *tmpfs_dir_get_nth(tmpfs_inode_t *dir, uint32_t index);
void            tmpfs_dir_free_all(tmpfs_inode_t *dir);

tmpfs_inode_t  *tmpfs_dir_walk(tmpfs_inode_t *start, const char *path,
                                int follow,
                                tmpfs_inode_t **parent_out,
                                const char **name_out);

#endif /* TMPFS_DIR_H */
