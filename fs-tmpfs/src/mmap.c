/*
 * mmap.c  --  io_mmap handler.
 *
 * NOTE: mmap support (io_mmap -> SERVER_SHMEM_OBJECT) does not function on
 * QNX 8.0 for user-space resmgr file descriptors.  The _IO_MMAP message
 * is dispatched by memmgr to our handler, but the SERVER_SHMEM_OBJECT reply
 * mechanism requires a kernel-level fd reference that user-space servers
 * cannot produce.  memmgr returns EBADF to the mmap(2) caller regardless
 * of what fd/coid values we place in the reply.
 *
 * The handler is kept registered so the slot is not ENOSYS, and returns
 * ENODEV ("operation not supported on this device") to communicate clearly
 * that mmap is unavailable rather than producing a confusing ENOSYS.
 *
 * See TODO.md for the tracking item.
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
    (void)ctp; (void)msg; (void)ocb;
    /*
     * mmap via SERVER_SHMEM_OBJECT does not work on QNX 8.0 for user-space
     * resmgr servers -- see file header comment and TODO.md.
     */
    return ENODEV;
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
