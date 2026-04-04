# fs-tmpfs — QNX 8.0.3 Temporary Filesystem Driver Design

## Overview

`fs-tmpfs` is a user-space resource manager (resmgr) filesystem driver for QNX 8.0.3 implementing a tmpfs-style in-memory filesystem. It supports multiple simultaneous mount points, per-mount size limits, and a global cap of 50% of total system RAM across all mounts combined.

---

## Goals & Requirements

- Mount at multiple paths simultaneously
- Each mount has its own configurable maximum size
- All mounts combined must never exceed **50% of total physical RAM**
- Behave like a normal POSIX filesystem (`ENOSPC` on cap hit, etc.)
- Support `mmap`
- Support symlinks and hard links
- Support standard POSIX permissions
- Unmounting a mount immediately releases its memory back to the global pool
- Expose runtime statistics via a pseudo-device for CLI tooling
- Coordinator daemon exits cleanly when the last mount is removed

---

## Process Model — Coordinator Daemon

A single `fs-tmpfs` process acts as the coordinator for all mounts.

### First Mount
```
fs-tmpfs [options] /mount/point
  → no coordinator found
  → daemonize
  → calculate total RAM, set global_cap = total_ram / 2
  → register /dev/fs-tmpfs (control + stats channel)
  → resmgr_attach() for /mount/point
  → enter thread pool dispatch loop
```

### Subsequent Mounts
```
fs-tmpfs [options] /another/point
  → coordinator found via open(TMPFS_CTRL_PATH, O_RDWR)
  → send DCMD_TMPFS_ADD_MOUNT via devctl()
  → coordinator performs resmgr_attach() internally
  → invoking process exits cleanly
```

### Unmount
```
umount /mount/point
  → free all inodes for that mount
  → decrement global_used
  → resmgr_detach() for that mount point
  → decrement mount_count
  → if mount_count == 0:
      → enter 100ms DRAINING grace period (accept any racing ADD_MOUNT)
      → if no new mount arrives: deregister /dev/fs-tmpfs, exit(0)
```

---

## Memory Accounting

### Global Cap

Total physical RAM is read at startup by messaging the QNX memory manager:
```c
mem_info_t msg;
msg.i.type = _MEM_INFO;  msg.i.fd = NOFD;  msg.i.flags = 0;
MsgSend(MEMMGR_COID, &msg.i, sizeof(msg.i), &msg.o, sizeof(msg.o));
uint64_t total_ram = (uint64_t)msg.o.info.__posix_tmi_total;
```
`global_cap = total_ram / 2` — fixed at startup, never changes.

> **Note:** `sysconf(_SC_PHYS_PAGES)` returns -1 on QNX 8 and must not be used. See Implementation Note 1.

### Two-Level Check

Every allocation (file data, inode metadata, directory entries) performs:
```
1. Compute delta bytes needed
2. Check: mount_used + delta <= mount_cap       → else ENOSPC
3. Check: global_used + delta <= global_cap     → else ENOSPC
4. Atomic fetch-add on both counters
5. Perform actual allocation
```

Every free atomically decrements both counters.

> **Note:** Inode metadata and directory entries count against the quota, not just file data. This prevents exhausting memory with millions of empty files/directories.

### Atomic Counters
```c
// Global (process-wide)
size_t          global_cap;         // total_ram / 2, fixed at startup
atomic_size_t   global_used;        // sum across ALL mounts

// Per mount
size_t          mount_cap;          // from -o size=N
atomic_size_t   mount_used;
```

---

## Mount Size Options

Specified via `-o size=<value>` at mount time.

| Format   | Meaning                          |
|----------|----------------------------------|
| `size=N` | N bytes                          |
| `size=NB`| N bytes                          |
| `size=NK`| N * 1024                         |
| `size=NM`| N * 1024²                        |
| `size=NG`| N * 1024³                        |
| `size=N%`| N percent of total physical RAM  |

### Constraints
- `size=N%` requires `N <= 50` (global cap is 50%)
- A mount cannot request more than the remaining global cap allows — error at mount time
- **Default** if `-o size=` is omitted: **25% of total RAM**

---

## File Data Backing — Anonymous Shared Memory

To support `mmap`, file data cannot use plain `malloc` buffers. Each file's data is backed by a POSIX anonymous shared memory object:

```c
// On file creation:
shm_fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
ftruncate(shm_fd, initial_capacity);
shm_ptr = mmap(NULL, capacity, PROT_READ|PROT_WRITE, MAP_SHARED, shm_fd, 0);

// io_mmap handler redirects client directly to the shm fd:
msg->i.fd = inode->shm_fd;
// kernel maps those pages directly — zero copy
```

### SHM Growth Strategy — Exponential Doubling

```
Tiers:     4KB → 8KB → 16KB → ... (doubling)
Doubling cap: 1MB (after that, grow in 1MB increments)
Alignment: always align to page size (4KB on QNX 8)

Shrink condition: if current_size < capacity / 4
  → shrink to avoid wasting memory (hysteresis prevents thrash)
```

`ftruncate` on the shm fd is called infrequently as a result, which is desirable since it is a relatively expensive operation.

---

## Data Structures

### Mount (`tmpfs_mount_t`)
```
tmpfs_mount_t
  ├── mount_cap                    ← maximum bytes this mount may use
  ├── mount_used (atomic)          ← bytes currently charged to this mount
  ├── file_count (atomic_uint_fast64_t)
  ├── dir_count  (atomic_uint_fast64_t)
  ├── symlink_count (atomic_uint_fast64_t)
  ├── inode_count (atomic_uint_fast64_t)
  ├── resmgr_id                    ← id from resmgr_attach()
  ├── iofunc_mount_t iofunc_mount  ← QNX mount info (blocksize, dev, funcs)
  ├── root*                        ← root directory inode
  ├── next*                        ← coordinator linked list
  └── path[PATH_MAX]               ← mount point path (for stats)
```

### Inode (`tmpfs_inode_t`)
```
tmpfs_inode_t
  ├── iofunc_attr_t attr           ← MUST BE FIRST (uid, gid, mode, times, nlink, size)
  ├── tmpfs_mount_t* mount         ← back-pointer for accounting
  ├── uint32_t ref_count           ← open OCB count (protected by attr.lock)
  │
  ├── [FILE]    int shm_fd         ← SHM_ANON fd (-1 if not a file)
  │             void* shm_ptr      ← server-side mmap of the shm object
  │             size_t shm_cap     ← allocated shm capacity (may exceed file size)
  │
  ├── [DIR]     tmpfs_dirent_t* hash[16]  ← children hash table (16 buckets)
  │             tmpfs_inode_t* parent
  │             uint32_t child_count
  │
  └── [SYMLINK] char* symlink_target      ← heap-allocated target path
```

### Directory Entry (`tmpfs_dirent_t`)
```
tmpfs_dirent_t
  ├── char* name                   ← heap-allocated filename
  ├── tmpfs_inode_t* inode         ← target inode
  └── tmpfs_dirent_t* next         ← next in hash bucket chain
```

### Open Control Block (`tmpfs_ocb_t`)
```
tmpfs_ocb_t
  ├── iofunc_ocb_t ocb             ← MUST BE FIRST (QNX framework requirement)
  ├── tmpfs_inode_t* inode         ← the inode this OCB refers to
  └── uint32_t dir_pos             ← readdir position (0='.', 1='..', 2+=children)
```

### Global Coordinator State (`tmpfs_global_t`)
```
tmpfs_global_t  (g_tmpfs — process-wide singleton)
  ├── total_ram, global_cap        ← RAM budget (fixed at startup)
  ├── global_used (atomic)         ← bytes in use across ALL mounts
  ├── mounts*                      ← linked list of active mounts
  ├── mount_count
  ├── mounts_lock (pthread_rwlock_t) ← protects mounts list
  ├── ctrl_resmgr_id               ← /dev/fs-tmpfs attachment id
  ├── dpp*                         ← dispatch context
  ├── pool*                        ← thread pool
  ├── connect_funcs, io_funcs      ← shared handler tables for all mounts
  ├── ctrl_connect_funcs, ctrl_io_funcs  ← handler tables for /dev/fs-tmpfs
  ├── ctrl_attr, ctrl_mount        ← iofunc attrs for control device
  └── start_time                   ← for uptime_ms in stats
```

---

## Threading Model — Thread Pool

Uses `thread_pool_create()` / `thread_pool_start()` dispatch loop:

```
lo_water  = 2
hi_water  = ncpus * 2   (TMPFS_POOL_HI_WATER_PER_CPU = 2)
increment = 1
maximum   = 32
```

The thread pool uses `resmgr_context_alloc`, `resmgr_block`, `resmgr_handler`,
and `resmgr_context_free` — not the `dispatch_*` variants.

### Locking

The implementation uses two synchronisation primitives:

1. **`g_tmpfs.mounts_lock`** (`pthread_rwlock_t`) — protects the global mount
   linked list and `mount_count`. Held briefly for list traversal and
   modification.

2. **`inode->attr.lock`** (embedded `pthread_mutex_t` inside `iofunc_attr_t`)
   — the standard QNX iofunc attribute lock, acquired automatically by
   `iofunc_lock_ocb_default` / `iofunc_unlock_ocb_default` around every IO
   handler call. Protects per-inode data and metadata.

Memory counters (`global_used`, `mount_used`, stat counters) use C11
`_Atomic` / `atomic_fetch_add` — no mutex required for those.

> **Note:** The design originally called for per-mount `tree_rwlock` and
> per-inode `pthread_rwlock_t`. The implementation uses the simpler iofunc
> attr mutex for inode protection, which is sufficient for correct operation.

---

## Handler Coverage

The implementation splits handlers across connect functions (path-level
operations, no OCB) and IO functions (fd-level operations, OCB exists).

### Connect Handlers (`resmgr_connect_funcs_t`)

| Handler       | Implementation           | Notes                                              |
|---------------|--------------------------|----------------------------------------------------||
| `open`        | `tmpfs_connect_open`     | Path walk, O_CREAT, symlink follow, OCB alloc      |
| `unlink`      | `tmpfs_connect_unlink`   | File and empty-dir removal                         |
| `rename`      | `tmpfs_connect_rename`   | Atomic src→dst, handles existing dst               |
| `mknod`       | `tmpfs_connect_mknod`    | `mkdir()` and `mknod()` arrive here (subtype=5)    |
| `readlink`    | `tmpfs_connect_readlink` | Returns target via `MsgReplyv(EOK, link_reply)`    |
| `link`        | `tmpfs_connect_link`     | Hard links and symlink creation (extra_type check) |

### IO Handlers (`resmgr_io_funcs_t`)

| Handler      | Implementation          | Notes                                               |
|--------------|-------------------------|-----------------------------------------------------|
| `read`       | `tmpfs_io_read`         | File data read OR readdir (xtype check)             |
| `write`      | `tmpfs_io_write`        | Grows shm as needed, checks both caps               |
| `close_ocb`  | `tmpfs_io_close_ocb`    | Unrefs inode after `iofunc_close_ocb_default`       |
| `stat`       | `iofunc_stat_default`   | Standard iofunc default                             |
| `lseek`      | `iofunc_lseek_default`  | Standard iofunc default                             |
| `chmod`      | `iofunc_chmod_default`  | Standard iofunc default                             |
| `chown`      | `iofunc_chown_default`  | Standard iofunc default                             |
| `utime`      | `iofunc_utime_default`  | Standard iofunc default                             |
| `mmap`       | `tmpfs_io_mmap`         | Redirects to shm fd via `SERVER_SHMEM_OBJECT`       |
| `sync`       | `tmpfs_io_sync`         | Updates mtime (no actual flush needed)              |
| `space`      | `tmpfs_io_space`        | Handles `truncate`, `ftruncate`, `fallocate`        |
| `lock_ocb`   | `iofunc_lock_ocb_default`   | Standard iofunc default                         |
| `unlock_ocb` | `iofunc_unlock_ocb_default` | Standard iofunc default                         |

---

## Control & Stats Channel — `/dev/fs-tmpfs`

A pseudo-device registered by the coordinator. Used for both mount management and runtime statistics. CLI tools only need `tmpfs_ipc.h`.

### devctl Commands

| Command                  | Direction   | Description                          |
|--------------------------|-------------|--------------------------------------|
| `DCMD_TMPFS_ADD_MOUNT`   | write       | Coordinator adds a new mount point   |
| `DCMD_TMPFS_DEL_MOUNT`   | write       | Coordinator unmounts a path          |
| `DCMD_TMPFS_GET_STATS`   | read        | Returns global stats snapshot        |
| `DCMD_TMPFS_GET_MOUNT`   | read/write  | Returns stats for one mount by path  |

### Stats Structures

```c
typedef struct tmpfs_global_stats {
    uint32_t    version_major;      // driver version
    uint32_t    version_minor;
    uint32_t    version_patch;
    uint32_t    _pad;
    uint64_t    total_ram;          // total system RAM, bytes
    uint64_t    global_cap;         // 50% of total_ram
    uint64_t    global_used;        // bytes in use across all mounts
    uint32_t    mount_count;        // number of active mounts
    uint32_t    _pad2;
    uint64_t    uptime_ms;          // ms since coordinator started
} tmpfs_global_stats_t;

typedef struct tmpfs_mount_stats {
    char        path[PATH_MAX];     // mount point path (input for GET_MOUNT)
    uint64_t    mount_cap;          // per-mount size limit, bytes
    uint64_t    mount_used;         // bytes currently in use
    uint64_t    file_count;         // number of regular files
    uint64_t    dir_count;          // number of directories
    uint64_t    symlink_count;      // number of symlinks
    uint64_t    inode_count;        // total inodes (all types)
} tmpfs_mount_stats_t;
```

---

## Source Layout

```
fs-tmpfs/
├── Makefile
├── include/
│   ├── fs-tmpfs.h          ← compile-time constants (pool sizes, shm thresholds, etc.)
│   ├── tmpfs_ipc.h         ← public API: DCMD_* macros, stats structs, request structs
│   └── tmpfs_internal.h    ← internal structs: inode, mount, ocb, global state
├── src/
│   ├── main.c              ← entry point, -o parsing, coordinator bootstrap, RAM detection
│   ├── resmgr.c            ← all connect + IO handlers, thread pool, iofunc_funcs_t
│   ├── mount.c             ← mount attach/detach, size option parsing, tree teardown
│   ├── inode.c             ← inode alloc/free, ref counting, inode number generation
│   ├── dir.c               ← hash table ops: lookup, insert, remove, get_nth, walk
│   ├── file.c              ← read, write, truncate, shm growth/shrink strategy
│   ├── mmap.c              ← io_mmap handler, io_sync handler
│   ├── symlink.c           ← symlink_create, symlink_read
│   ├── memory.c            ← two-level atomic accounting: reserve, release
│   └── control.c           ← /dev/fs-tmpfs resmgr: devctl handlers, stats
└── tools/
    └── tmpfs-stat.c        ← CLI stats tool (only needs tmpfs_ipc.h)
```

### Build Order (minimises dependency issues)

```
1.  include/            ← all headers, no code
2.  memory.c            ← no deps, pure accounting
3.  inode.c             ← depends on memory.c
4.  dir.c               ← depends on inode.c
5.  file.c              ← depends on inode.c, memory.c
6.  symlink.c           ← depends on inode.c
7.  mmap.c              ← depends on file.c
8.  control.c           ← depends on memory.c, mount state
9.  mount.c             ← depends on all of the above
10. resmgr.c            ← wires all handlers together
11. main.c              ← entry point, coordinator logic
12. Makefile            ← ties it all together
13. tools/tmpfs-stat.c  ← uses only tmpfs_ipc.h
```

---

## New Session Bootstrap Instructions

Before writing any code or answering any questions:

1. Read **`DESIGN.md`** in full — this file.
2. Run `ls -R fs-tmpfs/` to confirm the current state of the source tree.
3. Run `cd fs-tmpfs && make` to confirm it still builds clean before touching anything.

This file is the single source of truth. It reflects the **as-built** implementation, not the original design. The Implementation Notes section documents every non-obvious QNX-specific decision made during development.

---

## QNX 8.0.3 Environment Notes

- **Compiler**: `clang` (QNX clang 21.1.3, target `aarch64-unknown-qnx`). No `qcc` on self-hosted systems.
- **Libraries**: All resmgr, iofunc, and thread pool symbols are in `libc.so`. No separate `-lresmgr` needed.
- **`SHM_ANON`**: Fully supported — used for all file data backing.
- **`iofunc_mmap()` default**: Does not handle anonymous shm redirection — a custom `io_mmap` handler is required.
- **`procmgr_ability()`**: Must be called before `procmgr_daemon()` to allow non-root `resmgr_attach()`.
- **`procmgr_daemon()`**: Forks the process. All initialisation (dispatch, resmgr, thread pool) happens in the child, after the call.
- **`sysconf(_SC_PHYS_PAGES)`**: Returns -1 — unusable. Use `MsgSend(MEMMGR_COID, _MEM_INFO)` instead.
- **`iofunc_check_access()`**: Broken for non-root users on QNX 8 — always returns EPERM. Use manual mode-bit check.
- **`_FTYPE_MOUNT`**: Required for filesystem-style mounts. `_FTYPE_ANY` breaks directory opens.
- **`_RESMGR_FLAG_BEFORE`**: Required when mounting over an existing real directory.
- **`O_WRONLY == 1 == _IO_FLAG_RD`**: Never check `O_WRONLY` directly in connect handlers.

---

## Implementation Notes — Hard-Won Lessons

These notes document every non-obvious problem encountered during implementation
and the exact fix applied. They are intended to prevent re-discovery of the same
issues in future sessions or related projects.

---

### 1. Getting Total System RAM — Use `_MEM_INFO`, not `sysconf`

`sysconf(_SC_PHYS_PAGES)` returns `-1` on QNX 8 QEMU systems (and potentially
on real hardware). Do not rely on it. The correct method is to message the
memory manager directly:

```c
#include <sys/mman.h>     // for NOFD
#include <sys/memmsg.h>   // for mem_info_t, _MEM_INFO, MEMMGR_COID

mem_info_t msg;
memset(&msg, 0, sizeof(msg));
msg.i.type  = _MEM_INFO;
msg.i.zero  = 0;
msg.i.flags = 0;
msg.i.fd    = NOFD;

if (MsgSend(MEMMGR_COID, &msg.i, sizeof(msg.i),
                         &msg.o, sizeof(msg.o)) != 0) {
    // handle error
}

uint64_t total_ram = (uint64_t)msg.o.info.__posix_tmi_total;
```

`MEMMGR_COID` is defined as `SYSMGR_COID` in `<sys/memmsg.h>`. This works
reliably on all QNX 8 configurations including QEMU.

---

### 2. `resmgr_attach` Requires Root or Explicit Abilities

Calling `resmgr_attach()` as a non-root process fails with `EPERM`. Before
daemonising, call `procmgr_ability()` to request the required abilities:

```c
#include <sys/procmgr.h>

procmgr_ability(0,
    PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PATHSPACE,
    PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PUBLIC_CHANNEL,
    PROCMGR_AID_EOL);
```

This must be called **before** `procmgr_daemon()` and before the first
`resmgr_attach()`. If the process is already running as root the call succeeds
but is a no-op.

---

### 3. Use `_FTYPE_MOUNT` for Filesystem Mounts, Not `_FTYPE_ANY`

When attaching a directory-style resource manager that should intercept all
path operations below a mount point, the `file_type` argument to `resmgr_attach`
must be `_FTYPE_MOUNT`.

Using `_FTYPE_ANY` causes the pathmgr to send `ioflag=0x8880` (no `_IO_FLAG_RD`
or `_IO_FLAG_WR` bits) for directory opens, which makes `iofunc_open` return
`EISDIR` for all directory opens — making `ls`, `opendir`, and all directory
operations fail.

With `_FTYPE_MOUNT` the pathmgr correctly sends `ioflag=0x8001` (`_IO_FLAG_RD`
set) for read-only directory opens, which `iofunc_open` accepts.

```c
resmgr_attach(dpp, &attr, path,
              _FTYPE_MOUNT,                                    // <-- not _FTYPE_ANY
              _RESMGR_FLAG_DIR | _RESMGR_FLAG_SELF | _RESMGR_FLAG_BEFORE,
              &connect_funcs, &io_funcs, &root_attr);
```

---

### 4. `_RESMGR_FLAG_BEFORE` Is Required to Shadow Real Directories

If the mount point path already exists as a real directory on the underlying
filesystem (e.g. `/mnt/tmpfs-test` exists on qnx6), `resmgr_attach` without
`_RESMGR_FLAG_BEFORE` will lose the path resolution race — the pathmgr finds
the real directory and returns `EISDIR` before ever forwarding to the resmgr.

Always include `_RESMGR_FLAG_BEFORE` so the resmgr takes priority:

```c
_RESMGR_FLAG_DIR | _RESMGR_FLAG_SELF | _RESMGR_FLAG_BEFORE
```

---

### 5. `O_WRONLY == 1 == _IO_FLAG_RD` on QNX — Never Check `O_WRONLY` Directly

This is a critical QNX gotcha. The POSIX flag values on QNX are:

```c
O_RDONLY = 0   // octal 000000
O_WRONLY = 1   // octal 000001
O_RDWR   = 2   // octal 000002
```

The internal resmgr flags are:

```c
_IO_FLAG_RD = 0x1   // == O_WRONLY !
_IO_FLAG_WR = 0x2
```

This means `(ioflag & O_WRONLY)` is **always true** when `_IO_FLAG_RD` is set,
which is the case for every read-only directory open. Any code that checks
`msg->connect.ioflag & O_WRONLY` to detect write intent will incorrectly fire
for all read opens.

**Fix**: Do not check `O_WRONLY` or `O_RDWR` manually in the open handler.
Pass `msg` directly to `iofunc_open()` and let it handle the EISDIR logic.

---

### 6. `iofunc_check_access` Always Returns EPERM for Non-Root Users

`iofunc_check_access(ctp, attr, W_OK, &cinfo)` returns `EPERM` (1) for all
non-root clients regardless of the directory mode bits. This was observed on
QNX 8.0.3 with `mode=040777`, `uid=root`, `gid=root` — a non-root user
should have write access via the `other` bits but `iofunc_check_access`
denies it.

This means `mkdir`, `symlink`, `link`, `rename`, and `unlink` operations by
non-root users will all fail with EPERM if you use `iofunc_check_access`.

**Fix**: Implement a manual POSIX permission check:

```c
static int tmpfs_check_access(const iofunc_attr_t *attr, mode_t need,
                               const struct _client_info *ci)
{
    if (ci->cred.euid == 0)
        return EOK;   /* root always wins */

    mode_t mode = attr->mode;
    mode_t eff;

    if (ci->cred.euid == attr->uid)
        eff = (mode >> 6) & 7;   /* owner bits */
    else if (ci->cred.egid == attr->gid)
        eff = (mode >> 3) & 7;   /* group bits */
    else
        eff = (mode >> 0) & 7;   /* other bits */

    /* need uses W_OK=2, R_OK=4, X_OK=1 from <unistd.h> */
    return ((eff & need) == need) ? EOK : EPERM;
}
```

Note: `iofunc_open()` (used for the `open` connect handler) does its own
permission checking correctly — only the **connect** handlers for mknod,
link, unlink, and rename need this manual check.

---

### 7. OCB Lifecycle — `iofunc_close_ocb_default` Calls `ocb_free` With Lock Held

The `iofunc_close_ocb_default()` function:
1. Calls `iofunc_ocb_detach()` (decrements `attr->count`)
2. Acquires `attr->lock`
3. Calls `mount->funcs->ocb_free(ocb)` **while holding `attr->lock`**
4. Releases `attr->lock`

Consequences:
- Do **not** call `iofunc_attr_lock()` inside `ocb_free` — deadlock.
- Do **not** call `free(ocb)` after `iofunc_close_ocb_default()` returns —
  double free, because `ocb_free` already freed it.
- Do **not** call `iofunc_attr_unlock()` after `tmpfs_inode_free()` if the
  inode was freed — use-after-free on the embedded mutex.

**Correct pattern**:

```c
// In iofunc_funcs_t:
static void tmpfs_ocb_free(iofunc_ocb_t *ocb) {
    free(TMPFS_OCB(ocb));   // ONLY free the OCB struct, nothing else
}

// In the close_ocb IO handler:
int tmpfs_io_close_ocb(resmgr_context_t *ctp, void *reserved,
                        iofunc_ocb_t *ocb)
{
    tmpfs_inode_t *ino = INODE_FROM_OCB(ocb);  // save BEFORE ocb is freed

    // This calls ocb_free (frees OCB) then releases attr->lock.
    iofunc_close_ocb_default(ctp, reserved, ocb);

    // OCB is gone. Unref the inode now that the lock is released.
    iofunc_attr_lock(&ino->attr);
    int freed = tmpfs_inode_unref(ino);
    if (!freed)
        iofunc_attr_unlock(&ino->attr);
    // If freed==1: inode memory is gone — do NOT call iofunc_attr_unlock.
    return EOK;
}
```

---

### 8. Thread Pool — Use `resmgr_*` Context Functions, Not `dispatch_*`

When setting up `thread_pool_attr_t` for a resmgr-based server, use the
`resmgr_*` variants of the context functions:

```c
// CORRECT:
tpattr.context_alloc = resmgr_context_alloc;  // declared in <sys/dispatch.h>
tpattr.block_func    = resmgr_block;
tpattr.handler_func  = resmgr_handler;
tpattr.context_free  = resmgr_context_free;

// WRONG — type mismatch, compile errors with -Wall:
tpattr.context_alloc = dispatch_context_alloc;
tpattr.block_func    = dispatch_block;
tpattr.handler_func  = dispatch_handler;
tpattr.context_free  = dispatch_context_free;
```

`resmgr.h` defines `THREAD_POOL_PARAM_T` as `resmgr_context_t`. The `dispatch_*`
variants use `dispatch_context_t`. Mixing them causes type-mismatch compiler
errors. All four `resmgr_*` functions are declared in `<sys/dispatch.h>`.

---

### 9. devctl Reply Format — Use `_DEVCTL_DATA` and `_RESMGR_PTR`

For devctl handlers that return data (`__DIOF` or `__DIOTF` commands), write
the response into the message buffer using `_DEVCTL_DATA(msg->o)`, then reply
via `_RESMGR_PTR`:

```c
case DCMD_TMPFS_GET_STATS: {
    tmpfs_global_stats_t *stats =
        (tmpfs_global_stats_t *)_DEVCTL_DATA(msg->o);
    memset(stats, 0, sizeof(*stats));
    // ... fill stats ...
    msg->o.ret_val = EOK;
    msg->o.nbytes  = sizeof(*stats);
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*stats));
}
```

Using `SETIOV` + `_RESMGR_NPARTS(2)` with a **stack-allocated** stats struct
does not work — the stack frame is gone by the time the framework sends the
reply. Always write into `msg->o`, which is backed by the persistent resmgr
receive buffer.

---

### 10. Symlink Handling — Three Distinct Protocol Cases

Symlinks interact with the resmgr protocol in three distinct ways:

#### A. Open handler — following a symlink during `open()`

When the open handler finds a `S_IFLNK` inode and the open should follow it
(`subtype == _IO_CONNECT_OPEN`, `O_NOFOLLOW` not set), reply with
`_IO_CONNECT_RET_LINK` using the **message buffer** as storage (the IOV must
point to memory that outlives the handler):

```c
if (S_ISLNK(ino->attr.mode) && !(oflag & O_NOFOLLOW)
    && msg->connect.subtype == _IO_CONNECT_OPEN) {

    const char *target = /* full path from mount root */;
    size_t tlen = strlen(target);

    struct _io_connect_link_reply *rep = (struct _io_connect_link_reply *)msg;
    memset(rep, 0, sizeof(*rep));
    rep->nentries = 0;
    rep->path_len = (uint16_t)(tlen + 1);
    memcpy((char *)rep + sizeof(*rep), target, tlen + 1);
    SETIOV(&ctp->iov[0], rep, sizeof(*rep) + tlen + 1);
    _IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_LINK);
    return _RESMGR_NPARTS(1);
}
```

For `subtype == _IO_CONNECT_COMBINE_CLOSE` (used by `stat`, `lstat`, `access`,
etc.) do **not** follow the symlink — open the symlink's own inode and let
`iofunc_open` return the `S_IFLNK` attributes. The pathmgr will not follow it.

#### B. `readlink` connect handler — for explicit `readlink()` syscall

Do **not** use `_IO_CONNECT_RET_LINK` here. Reply with status `EOK` and the
`link_reply` + path in the message buffer. The pathmgr returns the path string
directly to the `readlink()` caller:

```c
struct _io_connect_link_reply *rep = (struct _io_connect_link_reply *)msg;
memset(rep, 0, sizeof(*rep));
rep->nentries = 0;
rep->path_len = (uint16_t)(tlen + 1);
memcpy((char *)rep + sizeof(*rep), target, tlen + 1);

iov_t rl_iov;
SETIOV(&rl_iov, rep, sizeof(*rep) + tlen + 1);
MsgReplyv(ctp->rcvid, EOK, &rl_iov, 1);  // EOK — NOT _IO_CONNECT_RET_LINK
return _RESMGR_NOREPLY;
```

If `_IO_CONNECT_RET_LINK` is used here the pathmgr will **follow** the symlink
and call readlink again on the target, causing an infinite loop or EINVAL.

#### C. Relative symlink targets in subdirectories

When the pathmgr receives `_IO_CONNECT_RET_LINK` with a relative target, it
resolves from the **mount root**, not the symlink's parent directory. A symlink
`/ramfs/sub/link.txt -> file.txt` will try to open `/ramfs/file.txt` instead
of `/ramfs/sub/file.txt`.

**Fix**: Prepend the symlink's parent directory path for relative targets:

```c
// msg->connect.path = "sub/link.txt", target = "file.txt"
// Strip last component of connect.path to get parent: "sub"
// Send target as: "sub/file.txt"
const char *p = msg->connect.path;
const char *lastslash = strrchr(p, '/');
if (lastslash != NULL && target[0] != '/') {
    char fullpath[PATH_MAX];
    snprintf(fullpath, sizeof(fullpath), "%.*s/%s",
             (int)(lastslash - p), p, target);
    target = fullpath;
}
```

Absolute targets (`target[0] == '/'`) need no adjustment.

---

### 11. `readdir` — Synthesise `.` and `..` as Virtual Entries

The `io_read` handler is called for directory reads (`_IO_XTYPE_READDIR`).
Synthesize `.` and `..` at OCB positions 0 and 1:

```
pos 0 -> "."   inode = current directory
pos 1 -> ".."  inode = parent directory (or self if mount root)
pos 2+ -> real children from the hash table
```

QNX `struct dirent` layout (from `<dirent.h>`):

```c
struct dirent {
    ino_t    d_ino;       // inode number
    off_t    d_offset;    // position in directory stream
    int16_t  d_reclen;    // record length, 8-byte aligned
    int16_t  d_namelen;   // strlen(d_name)
    char     d_name[];    // filename, null-terminated
};

// Record size formula:
size_t reclen = (offsetof(struct dirent, d_name) + namelen + 1 + 7) & ~7;
```

---

### 12. `_IO_SPACE` Covers Both `truncate` and `fallocate`

Both `ftruncate()` and `posix_fallocate()` arrive via the `io_space` handler
(`g_io_funcs.space`). Distinguish by `msg->i.subtype`:

```c
#include <fcntl.h>  // F_FREESP, F_GROWSP, F_ALLOCSP, F_FREESP64, F_GROWSP64

int subtype = msg->i.subtype & 0xff;
if (subtype == F_FREESP || subtype == F_FREESP64) {
    // ftruncate(): shrink/grow to msg->i.start
    tmpfs_file_truncate(ino, (off_t)msg->i.start);
} else if (subtype == F_GROWSP || subtype == F_GROWSP64) {
    // grow-only truncate
    off_t new_size = (off_t)(msg->i.start + msg->i.len);
    if (new_size > ino->attr.nbytes)
        tmpfs_file_truncate(ino, new_size);
} else {
    // F_ALLOCSP: reserve capacity, visible size unchanged
    tmpfs_file_ensure_capacity(ino, (size_t)(msg->i.start + msg->i.len));
}
msg->o = (uint64_t)ino->attr.nbytes;
MsgReply(ctp->rcvid, EOK, &msg->o, sizeof(msg->o));
return _RESMGR_NOREPLY;
```

---

### 13. `rename` and Hard `link` — QNX Packs Destination First

For both `rename(old, new)` and `link(src, dst)`, QNX packs the message as:

```
connect.path   = "destination\0source"   (destination comes first)
extra->path    = "source"                (the source / old path)
```

This is **counterintuitive** — `connect.path` (up to first `\0`) is the
destination/new name, and `extra->path` is the source/old name.

For symlink creation (`ln -s target linkname`):
```
connect.path        = "linkname\0target"
connect.extra_type  = _IO_CONNECT_EXTRA_SYMLINK
extra->path         = "target"
```

---

### 14. Custom OCB Requires `iofunc_funcs_t` Wired Into the Mount

To use an extended OCB type, provide a custom `iofunc_funcs_t` and attach it
to the `iofunc_mount_t`. Without this, `iofunc_close_ocb_default` uses the
default `free()` which corrupts heap if your OCB is larger than `iofunc_ocb_t`.

```c
static iofunc_funcs_t g_iofunc_funcs = {
    .nfuncs     = _IOFUNC_NFUNCS,
    .ocb_calloc = my_ocb_calloc,
    .ocb_free   = my_ocb_free,
};

iofunc_mount_init(&mnt->iofunc_mount, sizeof(mnt->iofunc_mount));
mnt->iofunc_mount.funcs = &g_iofunc_funcs;  // <-- required
```

See Note 7 for the locking constraint in `ocb_free`.

---

### 15. `iofunc_attr_t` and `iofunc_ocb_t` Must Be First Members

The resmgr framework casts between `iofunc_attr_t *` and your custom inode
type, and between `iofunc_ocb_t *` and your custom OCB type. These casts are
only valid if the iofunc struct is the **first field** with no padding before it:

```c
typedef struct tmpfs_inode {
    iofunc_attr_t attr;   // MUST BE FIRST
    tmpfs_mount_t *mount;
    /* ... */
} tmpfs_inode_t;
#define INODE_FROM_ATTR(p)  ((tmpfs_inode_t *)(p))

typedef struct tmpfs_ocb {
    iofunc_ocb_t ocb;     // MUST BE FIRST
    tmpfs_inode_t *inode;
    uint32_t dir_pos;
} tmpfs_ocb_t;
#define TMPFS_OCB(p)  ((tmpfs_ocb_t *)(p))
```

---

### 16. `THREAD_POOL_PARAM_T` — Include `<sys/resmgr.h>` Before `<sys/dispatch.h>`

`<sys/resmgr.h>` defines `THREAD_POOL_PARAM_T` as `resmgr_context_t` before
it includes `<sys/dispatch.h>`. If you include `<sys/dispatch.h>` first,
`THREAD_POOL_PARAM_T` defaults to `void` and `thread_pool_attr_t` gets wrong
function pointer types. Always include headers in this order:

```c
#include <sys/iofunc.h>    // defines iofunc types
#include <sys/resmgr.h>    // defines THREAD_POOL_PARAM_T = resmgr_context_t
#include <sys/dispatch.h>  // uses THREAD_POOL_PARAM_T correctly
```

---

### 17. `iofunc_open` Returns `EISDIR` When Direction Bits Are Missing

If a directory is opened with `ioflag` having neither `_IO_FLAG_RD` nor
`_IO_FLAG_WR` set (e.g. `ioflag=0x8880`), `iofunc_open` returns `EISDIR`.
This symptom indicates `_FTYPE_ANY` is being used instead of `_FTYPE_MOUNT`
(see Note 3). With `_FTYPE_MOUNT`, the pathmgr always sets `_IO_FLAG_RD`
for read-only directory opens.

---

### 18. `procmgr_daemon` — Initialise Everything After the Fork

`procmgr_daemon()` forks and the parent exits immediately. The child continues
as the daemon. All persistent state (dispatch context, resmgr attachments,
thread pool) must be created **after** the `procmgr_daemon()` call:

```
1. procmgr_ability()      <- before daemon (needs parent's credentials)
2. procmgr_daemon()       <- fork; parent exits, child continues
3. dispatch_create()      <- in child
4. iofunc_func_init()     <- in child
5. resmgr_attach()        <- in child
6. thread_pool_create()   <- in child
7. thread_pool_start()    <- blocks forever in child
```

Note: `PROCMGR_DAEMON_NOCLOSE` prevents stdout/stderr redirection to
`/dev/null`, which is useful during development.

---

### 19. `_IO_CONNECT` Subtypes — Know Which Operation Uses Which

| Value | Constant                    | Triggered by                     |
|-------|-----------------------------|----------------------------------|
| 0     | `_IO_CONNECT_COMBINE`       | open + IO op (fd kept open)      |
| 1     | `_IO_CONNECT_COMBINE_CLOSE` | `stat`, `lstat`, `access`, etc.  |
| 2     | `_IO_CONNECT_OPEN`          | `open()` — OCB is created        |
| 3     | `_IO_CONNECT_UNLINK`        | `unlink()`, `rmdir()`            |
| 4     | `_IO_CONNECT_RENAME`        | `rename()`                       |
| 5     | `_IO_CONNECT_MKNOD`         | `mkdir()`, `mknod()`             |
| 6     | `_IO_CONNECT_READLINK`      | `readlink()`                     |
| 7     | `_IO_CONNECT_LINK`          | `link()`, `symlink()`            |

Key rules:
- Only follow symlinks when `subtype == _IO_CONNECT_OPEN` (2).
- `mkdir()` arrives as `MKNOD` (5), not `OPEN` — handle in `connect_funcs.mknod`.
- `COMBINE_CLOSE` (1) creates a temporary OCB; the close happens automatically.

---

### 20. `mmap` Support Requires SHM-Backed File Data

Plain `malloc` buffers cannot be `mmap`'d by clients. Every regular file must
be backed by a `SHM_ANON` shared memory object (see File Data Backing section).
The `io_mmap` handler redirects the client to the underlying shm fd:

```c
struct _io_mmap_reply *reply = &msg->o;
reply->zero         = 0;
reply->allowed_prot = PROT_READ | PROT_WRITE | PROT_EXEC;
reply->offset       = offset;
reply->fd           = ino->shm_fd;
reply->coid         = 0;
MsgReply(ctp->rcvid, _IO_MMAP_REPLY_FLAGS_SERVER_SHMEM_OBJECT,
         reply, sizeof(*reply));
return _RESMGR_NOREPLY;
```

`_IO_MMAP_REPLY_FLAGS_SERVER_SHMEM_OBJECT` tells the kernel to duplicate
`shm_fd` into the client and map those pages directly (zero copy).

---

### 21. Building on QNX 8 Self-Hosted (No `qcc`)

QNX 8 self-hosted systems use `clang` directly (not `qcc`). Minimal Makefile:

```makefile
CC     = clang
CFLAGS = -Wall -Wextra -O2 -g -D_QNX_SOURCE
LIBS   = -lc
```

- `-D_QNX_SOURCE` enables QNX-specific extensions in system headers.
- All resmgr, iofunc, and thread pool symbols are in `libc.so` — no
  separate `-lresmgr` or `-liofunc` flags needed.
- Target triple: `aarch64-unknown-qnx` (QNX 8 on aarch64).
- `stdatomic.h` lives at `/usr/lib/clang/21/include/stdatomic.h`.

---

### 22. Quick Reference — Problems and Fixes

| Symptom | Root Cause | Fix |
|---------|------------|-----|
| `resmgr_attach` returns EPERM | Non-root, missing abilities | `procmgr_ability(PATHSPACE + PUBLIC_CHANNEL)` before daemon |
| `ls` / `opendir` returns EISDIR | Using `_FTYPE_ANY` | Switch to `_FTYPE_MOUNT` |
| Open handler never called | Real dir shadows mount | Add `_RESMGR_FLAG_BEFORE` |
| `mkdir` fails EPERM for non-root | `iofunc_check_access` broken | Replace with manual mode-bit check |
| `mkdir` not reaching mknod handler | QNX uses MKNOD subtype | Register `connect_funcs.mknod` |
| Daemon crashes on `chmod` / close | Double-free of OCB | Save inode ptr before `iofunc_close_ocb_default`; only free OCB in `ocb_free` |
| Read-only open treated as write | `O_WRONLY=1=_IO_FLAG_RD` | Never check `O_WRONLY` manually; delegate to `iofunc_open` |
| `readlink()` returns wrong/garbled data | Wrong reply format | `MsgReplyv(EOK, [link_reply+path])`, not raw string |
| Symlink follow causes infinite loop | `_IO_CONNECT_RET_LINK` in readlink | Use `EOK` in readlink handler; `RET_LINK` only in open handler |
| Relative symlinks broken in subdirs | pathmgr resolves from mount root | Prepend parent dir to relative target in open handler |
| `rename` / `link` has wrong src/dst | QNX packs dest first | `connect.path` = dest; `extra->path` = src |
| `sysconf(_SC_PHYS_PAGES)` returns -1 | Not supported on this QNX build | Use `MsgSend(MEMMGR_COID, _MEM_INFO)` |
| `truncate()` silently does nothing | Wrong handler registered | Handle `F_FREESP` in `io_space`, not a separate `io_truncate` |
| Thread pool type-mismatch errors | `dispatch_*` vs `resmgr_*` | Use `resmgr_block/handler/context_alloc/context_free` |
| devctl data not received by client | IOV pointed to stack variable | Write into `msg->o` (resmgr buffer); use `_RESMGR_PTR` |
