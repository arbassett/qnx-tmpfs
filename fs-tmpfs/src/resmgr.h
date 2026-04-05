/*
 * resmgr.h  --  resmgr / dispatch interface
 */
#ifndef TMPFS_RESMGR_H
#define TMPFS_RESMGR_H

#include <sys/iofunc.h>
#include <sys/resmgr.h>
#include <sys/iomsg.h>

int tmpfs_resmgr_init(void);
int tmpfs_resmgr_start(void);

/* Connect handler exposed so it can be referenced before resmgr_init */
int tmpfs_connect_mount(resmgr_context_t *ctp, io_mount_t *msg,
                         iofunc_attr_t *handle, io_mount_extra_t *extra);

#endif /* TMPFS_RESMGR_H */
