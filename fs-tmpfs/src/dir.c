/*
 * dir.c  --  Directory operations: lookup, insert, remove, walk.
 *
 * All directory operations require the caller to hold the inode's
 * attr.lock (via iofunc_lock_ocb / iofunc_unlock_ocb or explicit
 * iofunc_attr_lock / iofunc_attr_unlock) unless noted otherwise.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "../include/tmpfs_internal.h"
#include "memory.h"
#include "inode.h"
#include "dir.h"

/* -------------------------------------------------------------------------
 * Internal hash function
 * ---------------------------------------------------------------------- */
static unsigned dir_hash(const char *name)
{
    unsigned h = 5381;
    unsigned char c;
    while ((c = (unsigned char)*name++) != '\0')
        h = ((h << 5) + h) ^ c;
    return h & (TMPFS_DIR_HASH_BUCKETS - 1);
}

/* -------------------------------------------------------------------------
 * tmpfs_dir_lookup
 *
 * Find a directory entry by name inside directory `dir`.
 * Returns the child inode on success, NULL if not found.
 * Caller must hold dir->attr.lock (at least read).
 * ---------------------------------------------------------------------- */
tmpfs_inode_t *tmpfs_dir_lookup(tmpfs_inode_t *dir, const char *name)
{
    if (!S_ISDIR(dir->attr.mode))
        return NULL;

    unsigned bucket = dir_hash(name);
    tmpfs_dirent_t *de = dir->hash[bucket];
    while (de != NULL) {
        if (strcmp(de->name, name) == 0)
            return de->inode;
        de = de->next;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * tmpfs_dir_insert
 *
 * Add a new (name -> inode) entry to directory `dir`.
 * Charges accounting for the dirent overhead + name length.
 * Returns 0 on success, errno on failure.
 * Caller must hold dir->attr.lock (write).
 * ---------------------------------------------------------------------- */
int tmpfs_dir_insert(tmpfs_inode_t *dir, const char *name, tmpfs_inode_t *child)
{
    if (!S_ISDIR(dir->attr.mode))
        return ENOTDIR;

    /* Fail if name already exists */
    if (tmpfs_dir_lookup(dir, name) != NULL)
        return EEXIST;

    size_t namelen  = strlen(name);
    size_t charge   = TMPFS_DIRENT_OVERHEAD + namelen + 1;
    int rc = tmpfs_mem_reserve(dir->mount, charge);
    if (rc != 0)
        return rc;

    tmpfs_dirent_t *de = malloc(sizeof(tmpfs_dirent_t));
    if (de == NULL) {
        tmpfs_mem_release(dir->mount, charge);
        return ENOMEM;
    }

    de->name = malloc(namelen + 1);
    if (de->name == NULL) {
        free(de);
        tmpfs_mem_release(dir->mount, charge);
        return ENOMEM;
    }
    memcpy(de->name, name, namelen + 1);
    de->inode = child;

    unsigned bucket = dir_hash(name);
    de->next        = dir->hash[bucket];
    dir->hash[bucket] = de;
    dir->child_count++;

    /* Update dir size and ctime */
    dir->attr.nbytes += (off_t)(namelen + 1);
    iofunc_time_update(&dir->attr);

    return 0;
}

/* -------------------------------------------------------------------------
 * tmpfs_dir_remove
 *
 * Remove the entry `name` from directory `dir`.
 * Does NOT free or unref the child inode -- caller is responsible.
 * Releases dirent quota.
 * Returns the removed inode on success, NULL if not found.
 * Caller must hold dir->attr.lock (write).
 * ---------------------------------------------------------------------- */
tmpfs_inode_t *tmpfs_dir_remove(tmpfs_inode_t *dir, const char *name)
{
    if (!S_ISDIR(dir->attr.mode))
        return NULL;

    unsigned bucket = dir_hash(name);
    tmpfs_dirent_t **pp = &dir->hash[bucket];
    while (*pp != NULL) {
        tmpfs_dirent_t *de = *pp;
        if (strcmp(de->name, name) == 0) {
            *pp = de->next;
            tmpfs_inode_t *child = de->inode;

            size_t namelen = strlen(de->name);
            size_t charge  = TMPFS_DIRENT_OVERHEAD + namelen + 1;
            tmpfs_mem_release(dir->mount, charge);

            dir->attr.nbytes -= (off_t)(namelen + 1);
            if (dir->attr.nbytes < 0) dir->attr.nbytes = 0;
            iofunc_time_update(&dir->attr);
            dir->child_count--;

            free(de->name);
            free(de);
            return child;
        }
        pp = &de->next;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * tmpfs_dir_get_nth
 *
 * Return the Nth real entry (0-based, not counting . and ..).
 * Used by the readdir handler to enumerate entries.
 * Returns NULL if index is out of range.
 * Caller must hold dir->attr.lock (at least read).
 * ---------------------------------------------------------------------- */
tmpfs_dirent_t *tmpfs_dir_get_nth(tmpfs_inode_t *dir, uint32_t index)
{
    uint32_t count = 0;
    for (unsigned b = 0; b < TMPFS_DIR_HASH_BUCKETS; b++) {
        tmpfs_dirent_t *de = dir->hash[b];
        while (de != NULL) {
            if (count == index)
                return de;
            count++;
            de = de->next;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * tmpfs_dir_free_all
 *
 * Remove and unref all directory entries.  Used during mount teardown
 * when we are destroying the entire filesystem tree recursively.
 * The caller owns the traversal order (children before parents).
 * Caller must hold dir->attr.lock (write) or be in teardown context.
 * ---------------------------------------------------------------------- */
void tmpfs_dir_free_all(tmpfs_inode_t *dir)
{
    for (unsigned b = 0; b < TMPFS_DIR_HASH_BUCKETS; b++) {
        tmpfs_dirent_t *de = dir->hash[b];
        while (de != NULL) {
            tmpfs_dirent_t *next = de->next;

            size_t namelen = strlen(de->name);
            size_t charge  = TMPFS_DIRENT_OVERHEAD + namelen + 1;
            tmpfs_mem_release(dir->mount, charge);

            free(de->name);
            free(de);
            de = next;
        }
        dir->hash[b] = NULL;
    }
    dir->child_count = 0;
    dir->attr.nbytes = 0;
}

/* -------------------------------------------------------------------------
 * tmpfs_dir_walk
 *
 * Resolve a path relative to `start` and return the target inode.
 * Handles multi-component paths, ".", "..", and symlink following.
 *
 * `path`       -- relative path (may be empty or start with '/')
 * `follow`     -- if non-zero, follow symlinks on the final component
 * `parent_out` -- if non-NULL, *parent_out is set to the parent dir
 * `name_out`   -- if non-NULL, points to the final component inside `path`
 *
 * Returns the inode on success, NULL with errno set on failure.
 * ---------------------------------------------------------------------- */
tmpfs_inode_t *tmpfs_dir_walk(tmpfs_inode_t *start, const char *path,
                               int follow,
                               tmpfs_inode_t **parent_out,
                               const char **name_out)
{
    /* Work on a copy of the path so we can tokenise it */
    char   buf[PATH_MAX];
    size_t plen = strlen(path);
    if (plen >= PATH_MAX) { errno = ENAMETOOLONG; return NULL; }
    memcpy(buf, path, plen + 1);

    tmpfs_inode_t *cur    = start;
    tmpfs_inode_t *parent = NULL;
    char          *tok    = buf;
    int            depth  = 0;   /* symlink recursion depth */

    /* Strip leading slashes -- paths here are relative to mount root */
    while (*tok == '/') tok++;

    /* Empty path or "." means the start itself */
    if (*tok == '\0') {
        if (parent_out) *parent_out = start->parent ? start->parent : start;
        if (name_out)   *name_out   = path;
        return start;
    }

    while (*tok != '\0') {
        /* Split off the next component */
        char *sep = strchr(tok, '/');
        char *next_tok;
        if (sep != NULL) {
            *sep     = '\0';
            next_tok = sep + 1;
            while (*next_tok == '/') next_tok++;
        } else {
            next_tok = tok + strlen(tok); /* points to '\0' */
        }

        int is_last = (*next_tok == '\0');

        if (strcmp(tok, ".") == 0) {
            /* stay */
        } else if (strcmp(tok, "..") == 0) {
            if (cur->parent != NULL)
                cur = cur->parent;
            /* at root, ".." stays at root */
        } else {
            if (!S_ISDIR(cur->attr.mode)) { errno = ENOTDIR; return NULL; }

            parent = cur;
            tmpfs_inode_t *child = tmpfs_dir_lookup(cur, tok);
            if (child == NULL) {
                if (is_last) {
                    /* Not found on last component -- normal for O_CREAT */
                    if (parent_out) *parent_out = parent;
                    if (name_out) {
                        /* point into original path string at this component */
                        *name_out = path + (tok - buf);
                    }
                    errno = ENOENT;
                    return NULL;
                }
                errno = ENOENT;
                return NULL;
            }

            /* Follow symlinks (always on intermediate, optional on final) */
            if (S_ISLNK(child->attr.mode) && (!is_last || follow)) {
                if (++depth > TMPFS_SYMLINK_MAX_DEPTH) {
                    errno = ELOOP;
                    return NULL;
                }
                /* Symlink target: restart walk from root if absolute,
                 * else from parent directory */
                const char *target = child->symlink_target;
                tmpfs_inode_t *link_start;
                if (target[0] == '/') {
                    /* Find mount root by walking to the top */
                    link_start = cur;
                    while (link_start->parent != NULL)
                        link_start = link_start->parent;
                    target++; /* skip leading '/' */
                } else {
                    link_start = parent;
                }
                /* Recurse for the symlink target */
                tmpfs_inode_t *resolved =
                    tmpfs_dir_walk(link_start, target,
                                   !is_last ? 1 : follow,
                                   parent_out, name_out);
                if (resolved == NULL)
                    return NULL;
                child = resolved;
            }

            cur = child;
        }

        if (is_last) {
            if (parent_out) *parent_out = parent ? parent : start;
            if (name_out)   *name_out   = path + (tok - buf);
            return cur;
        }

        tok = next_tok;
    }

    /* Fell through -- path was all slashes or similar */
    if (parent_out) *parent_out = start;
    if (name_out)   *name_out   = path;
    return cur;
}
