/*
 * mmap.h  --  mmap handler interface
 */
#ifndef TMPFS_MMAP_H
#define TMPFS_MMAP_H

#include <sys/iofunc.h>
#include <sys/resmgr.h>
#include <sys/iomsg.h>

int tmpfs_io_mmap(resmgr_context_t *ctp, io_mmap_t *msg, iofunc_ocb_t *ocb);
int tmpfs_io_sync(resmgr_context_t *ctp, io_sync_t *msg, iofunc_ocb_t *ocb);

#endif /* TMPFS_MMAP_H */
