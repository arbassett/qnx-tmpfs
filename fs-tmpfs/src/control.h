/*
 * control.h  --  control device interface
 */
#ifndef TMPFS_CONTROL_H
#define TMPFS_CONTROL_H

#include "memory.h"

int  tmpfs_control_init(void);
void tmpfs_control_fini(void);

/* devctl + open handlers (also called from resmgr.c for the ctrl path) */
int ctrl_io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb);
int ctrl_io_open(resmgr_context_t *ctp, io_open_t *msg,
                 iofunc_attr_t *attr, void *extra);

#endif /* TMPFS_CONTROL_H */
