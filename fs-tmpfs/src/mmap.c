/*
 * mmap.c  --  io_mmap handler.
 *
 * Redirects the client's mmap() call to the file's anonymous SHM backing
 * store so that the kernel can map those pages directly (zero copy).
 * Also handles msync to keep mtime accurate for MAP_SHARED writers.
 */

#include <errno.h>
#include <sys/iofunc.h>
#include <sys/resmgr.h>
#include <sys/mman.h>
#include <sys/iomsg.h>
#include <sys/stat.h>

#include "../include/tmpfs_internal.h"
#include "file.h"
#include "mmap.h"

/* -------------------------------------------------------------------------
 * tmpfs_io_mmap
 *
 * Handler for _IO_MMAP messages.
 *
 * The strategy:
 *   1. Verify the inode is a regular file with a valid shm_fd.
 *   2. If the file is empty and the client wants to map it, ensure at
 *      least one page of backing store exists.
 *   3. Reply with _IO_MMAP_REPLY_FLAGS_SERVER_SHMEM_OBJECT set and the
 *      shm_fd in the reply so the kernel maps the SHM directly.
 * ---------------------------------------------------------------------- */
int tmpfs_io_mmap(resmgr_context_t *ctp, io_mmap_t *msg, iofunc_ocb_t *ocb)
{
    tmpfs_inode_t *ino = INODE_FROM_OCB(ocb);

    /* Only regular files can be mmap'd */
    if (!S_ISREG(ino->attr.mode))
        return ENODEV;

    /* Must have a backing shm object */
    if (ino->shm_fd == -1)
        return ENODEV;

    uint64_t requested_len = msg->i.requested_len;
    uint64_t offset        = msg->i.offset;

    /* Ensure we have enough backing capacity for the requested range */
    if (requested_len > 0) {
        size_t needed = (size_t)(offset + requested_len);
        if (needed > ino->shm_cap) {
            int rc = tmpfs_file_ensure_capacity(ino, needed);
            if (rc != 0)
                return rc;
        }
    }

    /* Build the reply */
    struct _io_mmap_reply *reply = &msg->o;
    reply->zero         = 0;
    reply->allowed_prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    reply->offset       = offset;
    reply->fd           = ino->shm_fd;
    reply->coid         = 0;

    /*
     * Tell the memory manager to map our SHM object directly.
     * The kernel will duplicate shm_fd into the client's process.
     */
    MsgReply(ctp->rcvid, _IO_MMAP_REPLY_FLAGS_SERVER_SHMEM_OBJECT,
             reply, sizeof(*reply));
    return _RESMGR_NOREPLY;
}

/* -------------------------------------------------------------------------
 * tmpfs_io_sync
 *
 * Handler for _IO_SYNC (fsync/fdatasync/msync).
 * For a memory-backed FS there is nothing to flush to disk, but we do
 * update mtime so MAP_SHARED writers that never call write() still see
 * an accurate modification timestamp.
 * ---------------------------------------------------------------------- */
int tmpfs_io_sync(resmgr_context_t *ctp, io_sync_t *msg, iofunc_ocb_t *ocb)
{
    tmpfs_inode_t *ino = INODE_FROM_OCB(ocb);

    iofunc_attr_lock(&ino->attr);
    iofunc_time_update(&ino->attr);
    iofunc_attr_unlock(&ino->attr);

    return EOK;
}
