# Writing a QNX 8 Filesystem Driver (resmgr) — Field Notes

This document captures everything learned from building `fs-tmpfs`, a
user-space filesystem driver for QNX 8.0.  It is written as a reference for
future work on the same or similar drivers.  It does not repeat the code —
it explains the *why* behind every non-obvious decision and records every
trap that was discovered the hard way.

---

## 1. The Mental Model

A QNX user-space filesystem is a **resource manager** (resmgr).  A resmgr is
a normal process that registers one or more paths with the QNX path manager
(pathmgr, part of `procnto`).  When any process opens a path that matches a
registered prefix, the kernel routes all subsequent IPC messages for that fd
to your resmgr process.

The resmgr receive-reply loop is the filesystem.  Every syscall the client
makes (`open`, `read`, `write`, `stat`, `mkdir`, `rename`, …) becomes a
synchronous IPC message your process receives, processes, and replies to.  The
client is **blocked** for the entire duration of your reply.

This is both the strength (simple programming model) and the constraint
(single-threaded handlers block other clients; use a thread pool).

---

## 2. The Required Infrastructure Stack

The minimum set of QNX headers and libraries needed:

```c
#include <sys/iofunc.h>    // iofunc_attr_t, iofunc_ocb_t, iofunc_* helpers
#include <sys/resmgr.h>    // resmgr_attach, connect/io funcs, THREAD_POOL_PARAM_T
#include <sys/dispatch.h>  // dispatch_create, thread_pool_*
```

**Header order matters.**  `<sys/resmgr.h>` must come before `<sys/dispatch.h>`
because resmgr.h defines `THREAD_POOL_PARAM_T` as `resmgr_context_t`.  If
dispatch.h is included first, `THREAD_POOL_PARAM_T` defaults to `void` and all
the thread pool function pointer types are wrong — you get mysterious type
mismatch compile errors.

**No extra link flags.**  On QNX 8 self-hosted (no `qcc`), compile with:

```makefile
CC     = clang
CFLAGS = -Wall -Wextra -O2 -g -D_QNX_SOURCE
LIBS   = -lc
```

Everything — resmgr, iofunc, thread pool — lives in `libc.so`.  There is no
`-lresmgr`.

---

## 3. Process Lifecycle

### Startup sequence

```
1.  procmgr_ability()     ← request PATHSPACE + PUBLIC_CHANNEL abilities
                            (BEFORE procmgr_daemon, needs parent credentials)
2.  procmgr_daemon()      ← fork; parent exits, child continues
3.  dispatch_create()     ← create the dispatch context (channel)
4.  iofunc_func_init()    ← fill connect_funcs / io_funcs with safe defaults
5.  resmgr_attach()       ← register path(s) with pathmgr
6.  thread_pool_create()  ← create the thread pool
7.  thread_pool_start()   ← enter the dispatch loop (blocks forever)
```

Everything from step 3 onward must happen **after** `procmgr_daemon()`.
`procmgr_daemon` forks; the parent exits immediately.  Any dispatch context,
channel, or resmgr registration created before the fork lives only in the
parent and is gone.

### Non-root operation

`resmgr_attach()` fails with EPERM for non-root processes unless you first
claim the abilities:

```c
procmgr_ability(0,
    PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PATHSPACE,
    PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PUBLIC_CHANNEL,
    PROCMGR_AID_EOL);
```

Call this before `procmgr_daemon()`, while still running as the original
(possibly non-root) process.

### Mount point does not need to pre-exist

`resmgr_attach` registers the path with the pathmgr directly.  The path does
not need to exist as a real directory on any underlying filesystem beforehand.
It appears in the namespace when attached and disappears when detached.  Do
not `mkdir` the mountpoint before mounting.

---

## 4. `resmgr_attach` — Getting It Right

```c
mnt->resmgr_id = resmgr_attach(
    dpp,
    &attr,
    path,
    _FTYPE_MOUNT,                                           // NOT _FTYPE_ANY
    _RESMGR_FLAG_DIR | _RESMGR_FLAG_SELF | _RESMGR_FLAG_BEFORE,
    &connect_funcs,
    &io_funcs,
    &root_inode->attr);   // the "handle" passed to connect handlers
```

**`_FTYPE_MOUNT`, not `_FTYPE_ANY`.**  For a directory-style filesystem mount,
you must use `_FTYPE_MOUNT`.  With `_FTYPE_ANY`, the pathmgr sends directory
open messages with `ioflag=0x8880` (neither `_IO_FLAG_RD` nor `_IO_FLAG_WR`
set), which causes `iofunc_open` to return `EISDIR` for every directory open —
making `ls`, `opendir`, and any directory traversal fail immediately.  With
`_FTYPE_MOUNT`, directory opens correctly get `ioflag=0x8001` (`_IO_FLAG_RD`
set).

**`_RESMGR_FLAG_BEFORE`.**  If the mountpoint path already exists as a real
directory on the underlying filesystem, the pathmgr finds it first and returns
`EISDIR` before routing to your resmgr.  `_RESMGR_FLAG_BEFORE` makes your
registration take priority.  Always include it for filesystem mounts.

**The handle.**  The fourth-to-last argument to `resmgr_attach` is the
"handle" — an opaque pointer the framework passes to your connect handlers as
the `handle` argument.  For a filesystem, this is typically your root
directory's `iofunc_attr_t *`.  Connect handlers (open, unlink, rename, …) use
it to find the root of the filesystem tree.

---

## 5. The Two Handler Tables

QNX splits handlers into two categories:

### Connect handlers (`resmgr_connect_funcs_t`)

Called for **path-resolution** operations before an fd is created.  No OCB
exists yet.  The `handle` argument is what you passed to `resmgr_attach`.

| Handler     | Triggered by                          |
|-------------|---------------------------------------|
| `open`      | `open()`, `creat()`                   |
| `unlink`    | `unlink()`, `rmdir()`                 |
| `rename`    | `rename()`                            |
| `mknod`     | `mkdir()`, `mknod()`                  |
| `readlink`  | `readlink()`                          |
| `link`      | `link()`, `symlink()`                 |
| `mount`     | `umount()` (unmount notification)     |

**Connect handlers run without any locks held.**  You must acquire your own
locks around any shared state.

### IO handlers (`resmgr_io_funcs_t`)

Called for **fd operations** after an OCB exists.  The `ocb` argument is your
open control block.

| Handler      | Triggered by                         |
|--------------|--------------------------------------|
| `read`       | `read()`, `readdir()`                |
| `write`      | `write()`                            |
| `close_ocb`  | `close()` (final close of the fd)    |
| `stat`       | `fstat()`, `stat()` on open fd       |
| `lseek`      | `lseek()`                            |
| `chmod`      | `fchmod()`                           |
| `chown`      | `fchown()`                           |
| `utime`      | `futimes()`                          |
| `mmap`       | `mmap()` on the fd                   |
| `sync`       | `fsync()`, `fdatasync()`             |
| `space`      | `ftruncate()`, `posix_fallocate()`   |
| `devctl`     | `devctl()` (including `statvfs`)     |
| `lock_ocb`   | Called by framework before IO handler|
| `unlock_ocb` | Called by framework after IO handler |

**IO handlers run with `attr->lock` already held at depth 1** by
`iofunc_lock_ocb_default`.  Do not call `iofunc_attr_lock()` again inside IO
handlers — see the locking section below.

Initialise both tables with safe defaults first:

```c
iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                 _RESMGR_IO_NFUNCS,      &io_funcs);
```

Then override only the handlers you need.

---

## 6. The OCB and Custom Types

An OCB (Open Control Block) represents one open fd.  Multiple OCBs can exist
for the same inode (multiple opens of the same file).

The framework requires:
- `iofunc_ocb_t` must be the **first member** of your custom OCB struct.
- `iofunc_attr_t` must be the **first member** of your custom inode struct.

These are hard constraints — the framework casts between the iofunc types and
your types with raw pointer casts.  Any padding before the first member breaks
everything silently.

```c
typedef struct my_inode {
    iofunc_attr_t attr;     // MUST BE FIRST — no exceptions
    /* your fields */
} my_inode_t;

typedef struct my_ocb {
    iofunc_ocb_t ocb;       // MUST BE FIRST — no exceptions
    my_inode_t  *inode;
    uint32_t     dir_pos;
    /* your fields */
} my_ocb_t;
```

To use a custom OCB size, provide a custom `iofunc_funcs_t` wired into your
`iofunc_mount_t`:

```c
static iofunc_funcs_t my_funcs = {
    .nfuncs     = _IOFUNC_NFUNCS,
    .ocb_calloc = my_ocb_calloc,  // allocate your extended OCB
    .ocb_free   = my_ocb_free,    // free it — called WITH attr->lock HELD
};

iofunc_mount_init(&mnt->iofunc_mount, sizeof(mnt->iofunc_mount));
mnt->iofunc_mount.funcs = &my_funcs;
```

Without this, `iofunc_close_ocb_default` calls the default `free()` which
corrupts the heap if your OCB is larger than `iofunc_ocb_t`.

### `ocb_free` runs with `attr->lock` held

`iofunc_close_ocb_default` acquires `attr->lock`, calls `ocb_free`, then
releases it.  Therefore:
- Only free the OCB struct itself inside `ocb_free`.  Do not touch the inode.
- Do not call `iofunc_attr_lock()` inside `ocb_free` — recursive deadlock (or
  on QNX with `PTHREAD_MUTEX_RECURSIVE`, depth increase that breaks things).

The correct close pattern:

```c
int my_io_close_ocb(resmgr_context_t *ctp, void *reserved, iofunc_ocb_t *ocb)
{
    my_inode_t *ino = MY_INODE(ocb);   // save pointer BEFORE OCB is freed

    iofunc_close_ocb_default(ctp, reserved, ocb);
    // OCB is now freed.  attr->lock is released.

    // Now safe to work with the inode:
    iofunc_attr_lock(&ino->attr);
    int freed = inode_unref(ino);      // drops ref count, maybe frees inode
    if (!freed)
        iofunc_attr_unlock(&ino->attr);
    // If freed: inode memory is gone — do NOT unlock
    return EOK;
}
```

---

## 7. The Locking Model

`iofunc_attr_t` embeds a `PTHREAD_MUTEX_RECURSIVE` mutex called `attr.lock`.
Understanding when it is held is the most important operational constraint for
IO handlers.

### Framework lock_ocb / unlock_ocb

Before calling any IO handler, the resmgr framework calls
`iofunc_lock_ocb_default`, which acquires `attr->lock` (depth 1).  After the
handler returns, `iofunc_unlock_ocb_default` releases it (back to depth 0).

**This means `attr->lock` is held at depth 1 for the entire duration of every
IO handler.**

### Do NOT double-lock in IO handlers

Because `attr->lock` is already held at depth 1, calling `iofunc_attr_lock()`
inside an IO handler raises the recursion depth to 2.  On QNX 8, calling
`MsgRead()` or `MsgReply()` while holding a recursive mutex at depth > 1
causes **ESRVRFAULT (errno 312)** — the kernel faults the server process with
SIGSEGV even when the data pointer is valid heap memory.

```c
// WRONG — raises depth to 2, then MsgRead crashes:
int my_io_write(...) {
    iofunc_attr_lock(&ino->attr);   // already at depth 1!
    MsgRead(ctp->rcvid, buf, ...);  // ESRVRFAULT → SIGSEGV
}

// CORRECT — rely on the depth-1 lock from lock_ocb_default:
int my_io_write(...) {
    // attr->lock already held at depth 1 — just use it
    ensure_capacity(...);
    MsgRead(ctp->rcvid, buf, ...);  // safe at depth 1
}
```

### IO handlers vs connect handlers

Connect handlers (open, unlink, rename, …) run **without** the framework
holding any lock on `attr->lock`.  If a connect handler accesses shared
mutable inode state, it must acquire the lock itself.  This is why the
explicit `iofunc_attr_lock` / `unlock` calls appear in connect handlers but
NOT in IO handlers.

### Other shared state

Counters like `inode_count`, `mount_used`, `global_used` are C11 `_Atomic`
types — use `atomic_fetch_add`, `atomic_load`, CAS loops.  No mutex needed for
these counters.

The global mount list is protected by a `pthread_rwlock_t`: read-lock for
traversal, write-lock for add/remove.

---

## 8. `iofunc_check_access` is Broken — Always Use Manual Checks

`iofunc_check_access()` always returns `EPERM` for non-root users on QNX 8,
regardless of mode bits.  This is a confirmed QNX bug.

Replace every call to it with a manual POSIX mode-bit check:

```c
static int check_access(const iofunc_attr_t *attr, mode_t need,
                         const struct _client_info *ci)
{
    if (ci->cred.euid == 0) return EOK;       // root always wins

    mode_t eff;
    if      (ci->cred.euid == attr->uid) eff = (attr->mode >> 6) & 7;
    else if (ci->cred.egid == attr->gid) eff = (attr->mode >> 3) & 7;
    else                                 eff = (attr->mode >> 0) & 7;

    return ((eff & need) == need) ? EOK : EPERM;
}
```

Apply this check in every connect handler that modifies the filesystem
(unlink, rename, mknod, link, and the O_CREAT branch of open).

`iofunc_open()` does its own permission checking correctly for the open
operation itself — you only need the manual check for the parent-directory
write-permission guards.

---

## 9. The `O_WRONLY` Trap

On QNX, the POSIX open flag values are:

```c
O_RDONLY = 0
O_WRONLY = 1    // <-- same bit as _IO_FLAG_RD !
O_RDWR   = 2
```

The internal resmgr direction flags are:

```c
_IO_FLAG_RD = 0x1   // set for read-only opens
_IO_FLAG_WR = 0x2
```

`O_WRONLY == _IO_FLAG_RD`.  Checking `(ioflag & O_WRONLY)` in a connect
handler will fire for **every read-only open**, not just write opens.  Never
check `O_WRONLY` directly.  Delegate direction checking to `iofunc_open()`.

---

## 10. `statvfs` Goes Through `devctl`, Not `stat`

`statvfs(2)` on QNX sends a `devctl` message with command `DCMD_FSYS_STATVFS`
(`<sys/dcmd_blk.h>`), not a stat message.  If your `io_devctl` handler returns
`ENOSYS`, `statvfs` falls back to querying the underlying block device or
returns garbage values (e.g., 200 GB when you mounted a 32 MB filesystem).

Implement the handler:

```c
#include <sys/statvfs.h>
#include <sys/dcmd_blk.h>

if (msg->i.dcmd == DCMD_FSYS_STATVFS) {
    struct __msg_statvfs *sv = (struct __msg_statvfs *)_DEVCTL_DATA(msg->o);
    memset(sv, 0, sizeof(*sv));
    sv->f_bsize   = blocksize;
    sv->f_frsize  = blocksize;
    sv->f_blocks  = cap / blocksize;
    sv->f_bfree   = (cap - used) / blocksize;
    sv->f_bavail  = sv->f_bfree;
    sv->f_files   = inode_cap;        // the limit
    sv->f_ffree   = inode_cap - inode_count;
    sv->f_favail  = sv->f_ffree;
    sv->f_fsid    = dev;
    sv->f_namemax = 255;
    strncpy(sv->f_basetype, "myfs", sizeof(sv->f_basetype) - 1);
    msg->o.ret_val = EOK;
    msg->o.nbytes  = sizeof(*sv);
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*sv));
}
```

---

## 11. `unlink` vs `rmdir` — Distinguish by `msg->connect.mode`

Both `unlink()` and `rmdir()` route to the `connect_funcs.unlink` handler.
To tell them apart, inspect the mode bits in the connect message:

- `rmdir()` sets `S_IFDIR` type bits in `msg->connect.mode` (value `0x4000`).
- `unlink()` sets `S_IFSOCK` type bits (`0xa000`) or other non-directory bits.

```c
int is_rmdir = S_ISDIR(msg->connect.mode);

if (S_ISDIR(ino->attr.mode) && !is_rmdir) return EISDIR;   // unlink(dir) → EISDIR
if (!S_ISDIR(ino->attr.mode) && is_rmdir) return ENOTDIR;  // rmdir(file) → ENOTDIR
```

`_IO_CONNECT_EFLAG_DIR` is **not** set by `rmdir` — do not rely on it.

---

## 12. Connect Subtype Reference

| Value | Constant                    | Triggered by                       |
|-------|-----------------------------|------------------------------------|
| 0     | `_IO_CONNECT_COMBINE`       | combined open+IO (fd kept open)    |
| 1     | `_IO_CONNECT_COMBINE_CLOSE` | `stat`, `lstat`, `access`, etc.    |
| 2     | `_IO_CONNECT_OPEN`          | `open()` — creates a persistent OCB|
| 3     | `_IO_CONNECT_UNLINK`        | `unlink()`, `rmdir()`              |
| 4     | `_IO_CONNECT_RENAME`        | `rename()`                         |
| 5     | `_IO_CONNECT_MKNOD`         | `mkdir()`, `mknod()`               |
| 6     | `_IO_CONNECT_READLINK`      | `readlink()`                       |
| 7     | `_IO_CONNECT_LINK`          | `link()`, `symlink()`              |

Key consequences:
- `mkdir()` arrives as subtype 5 (`MKNOD`) — register `connect_funcs.mknod`.
- `COMBINE_CLOSE` (1) is used by `stat`/`lstat`/`access`.  It creates a
  temporary OCB that the framework closes automatically.
- Symlink following should only happen for subtype 2 (`OPEN`).

### The `COMBINE_CLOSE` / `O_CREAT|O_EXCL` interaction

The QNX pathmgr uses `COMBINE_CLOSE` as a pre-check before `O_CREAT|O_EXCL`:
it asks "does this path exist?" before attempting the create.  If your open
handler returns `EOK` with an OCB for the `COMBINE_CLOSE` pre-check, the
pathmgr concludes the path pre-existed and returns `EEXIST` to the caller.

Fix: clear `O_EXCL` from `msg->connect.ioflag` immediately after successfully
creating a new file via `O_CREAT`.  This tells the pathmgr "we just created
this — it's new":

```c
// Inside the O_CREAT branch, after inserting the inode:
msg->connect.ioflag &= ~(uint32_t)O_EXCL;
```

---

## 13. Symlink Protocol

Three distinct cases, three different behaviours:

### Following during `open()`

When your open handler encounters a symlink and the open should follow it
(subtype == `_IO_CONNECT_OPEN`, `O_NOFOLLOW` not set), send
`_IO_CONNECT_RET_LINK`:

```c
struct _io_connect_link_reply *rep = (struct _io_connect_link_reply *)msg;
memset(rep, 0, sizeof(*rep));
rep->path_len = (uint16_t)(strlen(target) + 1);
memcpy((char *)rep + sizeof(*rep), target, strlen(target) + 1);
SETIOV(&ctp->iov[0], rep, sizeof(*rep) + strlen(target) + 1);
_IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_LINK);
return _RESMGR_NPARTS(1);
```

The pathmgr re-resolves the target path and sends a new message.

### `readlink()` syscall

Use `EOK` (NOT `_IO_CONNECT_RET_LINK`) and reply with the target string.
`path_len` must be the string length **without** the null terminator:

```c
rep->path_len = (uint16_t)strlen(target);   // no +1
memcpy((char *)rep + sizeof(*rep), target, strlen(target) + 1);
MsgReplyv(ctp->rcvid, EOK, &rl_iov, 1);
return _RESMGR_NOREPLY;
```

If you use `_IO_CONNECT_RET_LINK` here, the pathmgr follows the link and
calls readlink again — infinite loop.

### Relative targets in subdirectories

The pathmgr resolves `_IO_CONNECT_RET_LINK` targets relative to the **mount
root**, not the symlink's parent.  For a symlink at `sub/link → file.txt`, you
must send `sub/file.txt`:

```c
const char *lastslash = strrchr(msg->connect.path, '/');
if (lastslash && target[0] != '/') {
    snprintf(fullpath, PATH_MAX, "%.*s/%s",
             (int)(lastslash - msg->connect.path), msg->connect.path, target);
    target = fullpath;
}
```

---

## 14. `rename` and `link` — QNX Packs Destination First

This is the single most counterintuitive thing in the protocol.

For `rename(old, new)`:
- `msg->connect.path` = **new** (destination)
- `extra->path` = **old** (source)

For `link(src, dst)`:
- `msg->connect.path` = **dst** (destination / new link name)
- `extra->path` = **src** (source / existing file)

For `symlink(target, linkname)`:
- `msg->connect.path` = **linkname**
- `extra->path` = **target**
- `msg->connect.extra_type` = `_IO_CONNECT_EXTRA_SYMLINK`

---

## 15. `devctl` Reply Pattern

For devctl commands that return data (`__DIOF` or `__DIOTF`):

```c
MyStruct *out = (MyStruct *)_DEVCTL_DATA(msg->o);
/* fill *out */
msg->o.ret_val = EOK;
msg->o.nbytes  = sizeof(*out);
return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*out));
```

The response **must** go into `msg->o` (backed by the resmgr receive buffer),
not a stack-allocated struct.  The stack frame is gone before the reply is sent.

---

## 16. `readdir` — Synthesise `.` and `..`

The `io_read` handler handles both file reads and directory reads.  For
directory reads (`_IO_XTYPE_READDIR`), synthesise `.` at position 0 and `..`
at position 1 before yielding real children:

```c
// pos 0 → "."   (current dir's inode)
// pos 1 → ".."  (parent dir's inode, or self if root)
// pos 2+ → real children from hash table
```

QNX `struct dirent` record sizes must be 8-byte aligned:

```c
size_t reclen = (offsetof(struct dirent, d_name) + namelen + 1 + 7) & ~7;
```

---

## 17. `ftruncate` and `posix_fallocate` Both Go to `io_space`

Both `ftruncate()` and `posix_fallocate()` route to `io_funcs.space`.
Distinguish by `msg->i.subtype & 0xff`:

| Subtype        | Operation                           |
|----------------|-------------------------------------|
| `F_FREESP`     | `ftruncate()` — set new size        |
| `F_FREESP64`   | same, 64-bit                        |
| `F_GROWSP`     | grow-only truncate                  |
| `F_GROWSP64`   | same, 64-bit                        |
| `F_ALLOCSP`    | `posix_fallocate()` — reserve space |

`posix_fallocate()` should grow `attr->nbytes` to cover the requested range,
not just ensure backing capacity without changing the visible size.  Many
applications depend on this:

```c
rc = ensure_capacity(needed);
if (rc == EOK && (off_t)needed > ino->attr.nbytes)
    ino->attr.nbytes = (off_t)needed;
```

---

## 18. Getting Total RAM

`sysconf(_SC_PHYS_PAGES)` returns -1 on QNX 8.  Message the memory manager:

```c
#include <sys/mman.h>     // NOFD
#include <sys/memmsg.h>   // mem_info_t, _MEM_INFO, MEMMGR_COID

mem_info_t msg;
memset(&msg, 0, sizeof(msg));
msg.i.type  = _MEM_INFO;
msg.i.fd    = NOFD;
MsgSend(MEMMGR_COID, &msg.i, sizeof(msg.i), &msg.o, sizeof(msg.o));
uint64_t total_ram = (uint64_t)msg.o.info.__posix_tmi_total;
```

`MEMMGR_COID == SYSMGR_COID`.  Works on all QNX 8 configurations including QEMU.

---

## 19. The Thread Pool — Correct Setup

```c
thread_pool_attr_t tpattr;
memset(&tpattr, 0, sizeof(tpattr));
tpattr.handle         = dpp;
tpattr.context_alloc  = resmgr_context_alloc;   // NOT dispatch_context_alloc
tpattr.block_func     = resmgr_block;            // NOT dispatch_block
tpattr.handler_func   = resmgr_handler;          // NOT dispatch_handler
tpattr.context_free   = resmgr_context_free;     // NOT dispatch_context_free
tpattr.lo_water      = 2;
tpattr.hi_water      = ncpus * 2;
tpattr.increment     = 1;
tpattr.maximum       = 32;

pool = thread_pool_create(&tpattr, POOL_FLAG_EXIT_SELF);
thread_pool_start(pool);   // blocks forever
```

Use the `resmgr_*` variants, never the `dispatch_*` variants.  The type of
`THREAD_POOL_PARAM_T` is `resmgr_context_t` (set by resmgr.h); mixing with
dispatch context functions causes type-mismatch compile errors with `-Wall`.

---

## 20. `mount -t TYPE` Integration

QNX `mount(8)` does not use `dlopen`.  It uses `spawnp` to find and run a
binary named `mount_TYPE` from `PATH`.  For `mount -t tmpfs`, it runs
`mount_tmpfs`.

The binary receives:
```
mount_tmpfs  -o uid=1000  -o gid=1000  -o mode=700  none  /mountpoint
```

Three critical points:

**Each `-o` is a separate flag.**  `mount(8)` splits `-o a,b,c` into
`-o a -o b -o c`.  If your `getopt` loop does `opt_string = optarg`, only the
last option survives.  Accumulate all `-o` args:

```c
char opt_buf[512] = "";
while ((opt = getopt(argc, argv, "o:")) != -1) {
    if (opt == 'o') {
        if (opt_buf[0]) strncat(opt_buf, ",", ...);
        strncat(opt_buf, optarg, ...);
    }
}
```

**`mount(8)` returns a non-zero exit code even when the mount succeeds.**  QNX
`mount(8)` does its own post-mount validation that fails for virtual
filesystems (errno 29 or 30).  Use `check=False` in test automation and verify
the coordinator started instead.

**The mount point need not exist.**  See Section 3.

---

## 21. `umount` Integration

`umount /path` sends `_IO_CONNECT_MOUNT` with `_MOUNT_UNMOUNT` in
`extra->flags` to the resmgr serving that path.  Register a mount handler:

```c
connect_funcs.mount = my_connect_mount;

int my_connect_mount(resmgr_context_t *ctp, io_mount_t *msg,
                     iofunc_attr_t *handle, io_mount_extra_t *extra)
{
    if (extra && (extra->flags & _MOUNT_UNMOUNT)) {
        // find mount by ctp->id (resmgr_id), tear down
        tmpfs_mount_remove(path);
    }
    return EOK;
}
```

Without this handler, `umount` returns "Function not implemented".

---

## 22. `MsgRead` and `MsgReply` Safety Rules

These are the most operationally critical rules for IO handlers:

**Rule 1: Never call `MsgRead`/`MsgReply` at `attr->lock` depth > 1.**
The framework holds `attr->lock` at depth 1.  If you add a second
`iofunc_attr_lock()`, depth becomes 2.  Calling `MsgRead` or `MsgReply` at
depth 2 causes ESRVRFAULT (errno 312) on QNX 8 — the kernel sends SIGSEGV to
your process.  This crashes the whole server.

**Rule 2: Never call `MsgRead` directly into `MAP_SHARED` shm memory.**
Under concurrent multi-threaded load, `MsgRead` into a `MAP_SHARED SHM_ANON`
mapping causes ESRVRFAULT even when the address is valid.  This is an
undocumented QNX 8 kernel restriction.  Always `MsgRead` into heap memory:

```c
void *buf = malloc(nbytes);
MsgRead(ctp->rcvid, buf, nbytes, sizeof(msg->i));
memcpy(shm_ptr + offset, buf, nbytes);
free(buf);
```

**Rule 3: `MsgRead` and `MsgReply` are non-blocking.**
The client is synchronously blocked waiting for your reply.  `MsgRead` is a
kernel memcpy from the client's address space.  `MsgReply` is a kernel memcpy
to it.  Neither waits for any resource.  Both complete immediately.

**Rule 4: `RESMGR_NOREPLY` when you handle reply yourself.**
If you call `MsgReply()` or `MsgReplyv()` directly, return `_RESMGR_NOREPLY`
from the handler to prevent the framework from sending a second reply.

---

## 23. `MAP_FIXED` is Unsafe for Growing `SHM_ANON` Mappings

`MAP_FIXED` silently unmaps any existing mapping at the target address.  When
you use `MAP_FIXED` to extend an inode's shm backing store in-place, it may
destroy another inode's shm mapping that happens to start at `your_ptr + old_cap`.
Any server thread accessing that other inode gets ESRVRFAULT → SIGSEGV.

`MAP_FIXED_NOREPLACE` (Linux 4.17+) would solve this atomically, but QNX 8
does not have it.  An `msync` probe before `MAP_FIXED` has a TOCTOU race: the
range can be claimed between the probe and the `MAP_FIXED`.

**The only safe approach: always use `munmap` + `mmap(NULL)`.**  Let the OS
pick a fresh, guaranteed non-overlapping address:

```c
munmap(ino->shm_ptr, old_cap);
void *ptr = mmap(NULL, new_cap, PROT_READ|PROT_WRITE, MAP_SHARED, shm_fd, 0);
ino->shm_ptr = ptr;
```

This is slower (~12× more overhead per resize event) but correct.

---

## 24. Permission Bits — Use `07777` Not `0777`

When storing or passing permission bits through `iofunc_attr_init`, always use
`07777` as the mask — not `0777`:

```c
// WRONG — strips sticky (01000), setgid (02000), setuid (04000):
iofunc_attr_init(&ino->attr, S_IFDIR | (mode & 0777), NULL, NULL);

// CORRECT — preserves all permission bits:
iofunc_attr_init(&ino->attr, S_IFDIR | (mode & 07777), NULL, NULL);
```

The sticky bit (`01000`) is routinely set on shared directories (`mode=1777`).
Masking with `0777` silently discards it.

---

## 25. `mount(8)` Does Not Find `mount_TYPE` in the Build Directory

`mount(8)` uses `spawnp` which searches `PATH`.  For tests and development,
rather than installing to `/usr/bin`, prepend the build directory to `PATH`
before spawning `mount`:

```python
env = os.environ.copy()
env["PATH"] = BUILD_DIR + ":" + env.get("PATH", "")
subprocess.run(["mount", "-t", "tmpfs", ...], env=env)
```

Create a `mount_tmpfs` symlink in the build directory pointing at the built
binary:

```sh
ln -sf /path/to/build/fs-tmpfs /path/to/build/mount_tmpfs
```

This avoids polluting `/usr/bin` during development.

---

## 26. Detecting Process Identity in a Coordinator Pattern

To support a "first invocation becomes coordinator, subsequent invocations
delegate" pattern:

1. Register a well-known control device (e.g., `/dev/fs-tmpfs`) via a second
   `resmgr_attach`.
2. On startup, try `open(CTRL_PATH, O_RDWR)`:
   - ENOENT → no coordinator, become one.
   - Success → coordinator exists, send request via `devctl`, exit.
3. The coordinator process detaches the control device when the last mount is
   removed and exits after a grace period.

The control device handler uses `devctl` for structured IPC.  Define commands
with `__DIOT`, `__DIOF`, `__DIOTF` macros from `<devctl.h>`.

---

## 27. Testing Considerations

### Mount point lifecycle

The mount point does not need to exist before mounting and does not persist
after unmounting.  Do not `mkdir` the mountpoint in test setup.  After
`umount`, the path is gone.

### Detecting coordinator readiness

After starting the coordinator, poll for the control device rather than
sleeping a fixed amount:

```python
deadline = time.monotonic() + 2.0
while not os.path.exists(CTRL_PATH):
    if time.monotonic() > deadline: raise RuntimeError("coordinator timeout")
    time.sleep(0.01)
```

### `mount(8)` exit code

`mount -t tmpfs` returns non-zero even when the mount succeeds.  Check
coordinator readiness, not the exit code.

### Concurrent write testing

The easiest way to trigger the `MAP_FIXED` corruption bug and the `MsgRead`
depth bug is 4+ threads each writing 512 KiB in 4 KiB records to separate
files.  The 4 KiB record size generates many resize events; the separate files
mean many concurrent shm allocations whose addresses may be adjacent.

### `iofunc_attr_t` mutex type

The mutex embedded in `iofunc_attr_t` is `PTHREAD_MUTEX_RECURSIVE`.  You can
verify this with:

```c
pthread_mutex_trylock(&attr.lock);   // succeeds even if already locked by this thread
```

This means double-locking does not deadlock but does raise the depth counter,
which triggers ESRVRFAULT from `MsgRead`/`MsgReply`.

---

## 28. PROT_READ / PROT_WRITE Values on QNX

On QNX, `PROT_READ` and `PROT_WRITE` have different values than Linux:

```c
PROT_READ  = 0x100    // NOT 0x1
PROT_WRITE = 0x200    // NOT 0x2
PROT_EXEC  = 0x400    // NOT 0x4
```

Python's `mmap` module uses the QNX values.  When inspecting `_IO_MMAP`
message `prot` fields or constructing `mmap()` arguments from ctypes, use
the values from `<sys/mman.h>`, not Linux assumptions.

---

## 29. mmap Is Not Supported for User-Space Resmgr Files

The `_IO_MMAP_REPLY_FLAGS_SERVER_SHMEM_OBJECT` mechanism intended to let a
resmgr return its shm fd for zero-copy mmap does not work on QNX 8 for
user-space resmgr file fds.  The memmgr (pid=1) calls `MsgRead`/`MsgReply`
with `SERVER_SHMEM_OBJECT` set, but the memmgr rejects the fd from the reply
with EBADF regardless of what fd or coid values are provided.

This is an undocumented QNX 8 limitation.  mmap on `/dev/shmem` files works
because those are served by memmgr itself (no cross-process fd reference
required).

For files in a user-space resmgr: return `ENODEV` from the `io_mmap` handler.
Files can still be read and written efficiently via `read(2)`/`write(2)`.

---

## 30. Quick Survival Reference

| "Why does X not work?" | Answer |
|------------------------|--------|
| `resmgr_attach` → EPERM | Call `procmgr_ability(PATHSPACE + PUBLIC_CHANNEL)` before daemon |
| `ls` → EISDIR | Use `_FTYPE_MOUNT` not `_FTYPE_ANY` |
| Open handler never called | Add `_RESMGR_FLAG_BEFORE` to flags |
| `mkdir` → EPERM for non-root | Replace `iofunc_check_access` with manual mode-bit check |
| `mkdir` doesn't reach mknod handler | Register `connect_funcs.mknod` (subtype 5, not open) |
| Close crashes / double-free | In `ocb_free`, only free the OCB; save inode ptr before calling `iofunc_close_ocb_default` |
| Write → EIO under concurrent load | `MsgRead` into heap buffer, not shm directly |
| Write → SIGSEGV crashes server | Remove `MAP_FIXED` from shm growth; use `mmap(NULL)` |
| Concurrent writes → ESRVRFAULT | `MsgRead`/`MsgReply` called at mutex depth 2; remove explicit `iofunc_attr_lock` from IO handlers |
| `statvfs` returns garbage | Implement `io_devctl` handling `DCMD_FSYS_STATVFS` |
| `ftruncate` does nothing | Handle `F_FREESP` in `io_space`, not a missing handler |
| `umount` → "Function not implemented" | Register `connect_funcs.mount`; check `_MOUNT_UNMOUNT` flag |
| `mount -t X -o a,b,c` ignores options | `mount(8)` splits options into separate `-o` flags; accumulate with comma join |
| `mode=1777` → stored as `0777` | Mask with `07777` not `0777` in `iofunc_attr_init` |
| `O_CREAT` ignores parent permissions | Add explicit `check_access(parent, W_OK)` before inode alloc |
| `unlink(dir)` succeeds silently | Check `S_ISDIR(msg->connect.mode)` to distinguish `unlink` from `rmdir` |
| `readlink` returns extra `\0` | Set `path_len = strlen(target)` not `strlen(target)+1` |
| `rename`/`link` src and dst swapped | `connect.path` = destination, `extra->path` = source (always) |
| `sysconf(_SC_PHYS_PAGES)` → -1 | Use `MsgSend(MEMMGR_COID, _MEM_INFO)` |
| mmap → ENODEV / EBADF | Known QNX 8 limitation for user-space resmgr; not fixable |
