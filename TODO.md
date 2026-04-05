# fs-tmpfs — Known Limitations and TODO Items

This file tracks POSIX features that are missing, partially implemented, or
behave differently from standard filesystems due to QNX FTYPE_MOUNT resmgr
constraints.  Each item was discovered during test development and verified
against observed behaviour on QNX 8.0.

---

## TODO-1: mmap is not supported

**Symptom**: `mmap(2)` on a file in a tmpfs mount returns `ENODEV`.

**Root cause**: The `_IO_MMAP_REPLY_FLAGS_SERVER_SHMEM_OBJECT` reply mechanism
does not work for user-space resmgr servers on QNX 8.0.  The memory manager
(memmgr, pid=1) sends `_IO_MMAP` to our handler and we reply with
`SERVER_SHMEM_OBJECT` pointing at our `SHM_ANON` backing fd.  The memmgr
rejects the reply with `EBADF` regardless of what fd/coid values we supply.

Exhaustive investigation showed:
- The `_IO_MMAP` handler **is** called (confirmed via debug prints).
- `MsgReply(rcvid, SERVER_SHMEM_OBJECT, &reply, sizeof(reply))` succeeds (rc=0).
- The memmgr still returns `EBADF` to the mmap(2) caller.
- The same approach works for directory-level mmap on a FTYPE_ANY resmgr
  (ENOSYS is returned, not EBADF), confirming the mechanism is wired but
  the fd interpretation is wrong for file OCB bindings.
- Named shm objects (`/dev/shmem/*`), which are managed directly by memmgr,
  work correctly because no inter-process fd reference is needed.

**Impact**: Files in tmpfs cannot be memory-mapped.  Reads and writes via
`read(2)`/`write(2)` work correctly and efficiently (zero-copy in our
implementation).

**Possible future fix**: Investigate whether `ConnectDupFd` / `dupfd_register`
can be used to register our `SHM_ANON` fd with the memmgr before replying, or
whether the `io_openfd` handler needs to return the shm fd via the dupfd
mechanism so the memmgr can map it directly.

**Affected tests**: `TestMmap` — tests verify that mmap fails cleanly with
`ENODEV` and that normal I/O is unaffected afterwards.

---

## TODO-2: `stat()` does not follow symlinks; dangling symlinks are not detected

**Symptom**:
- `os.stat(symlink)` returns `S_IFLNK` instead of the target's file type.
- `os.stat(dangling_symlink)` returns `S_IFLNK` instead of raising
  `FileNotFoundError`.

**Root cause**: `stat(2)` and `lstat(2)` both use `_IO_CONNECT_COMBINE_CLOSE`
(subtype=1) internally.  Symlink following in `tmpfs_connect_open` is gated on
`subtype == _IO_CONNECT_OPEN` (subtype=2).

Enabling symlink following for `COMBINE_CLOSE` breaks `O_CREAT|O_EXCL`:
the QNX pathmgr sends a `COMBINE_CLOSE` post-check after the OPEN handler
creates the file; if our handler returns `_IO_CONNECT_RET_LINK` for that
post-check, the pathmgr enters an infinite resolution loop or returns
`EEXIST` incorrectly.

The two subtypes (`COMBINE_CLOSE` for stat vs. `COMBINE_CLOSE` for O_EXCL
post-check) are structurally identical — same subtype, same ioflag pattern —
and cannot be distinguished without tracking per-request state that is
inherently racy.

**Impact**:
- `os.stat(symlink)` always behaves like `os.lstat(symlink)`.
- Code that relies on `os.path.isfile(symlink_to_file)` will incorrectly
  return `False`.
- Tools that follow symlinks via `stat` (e.g., `find -L`, `ls -L`) may
  not work correctly within the mount.

**Workaround**: Use `os.path.realpath()` to resolve the symlink manually,
then stat the resolved path.

**Affected tests**: `test_symlink_lstat_vs_stat`, `test_dangling_symlink_lstat`
— both tests accept either the ideal or the known-limited behaviour.

---

## TODO-3: `O_CREAT|O_EXCL` creates the file but `os.path.exists()` may transiently return `False`

**Symptom**: Immediately after a successful `O_CREAT|O_EXCL` open, calling
`os.path.exists(path)` (or any other `COMBINE_CLOSE`-based check) returns
`False`, even though the file was created and is accessible via all other
operations.

**Root cause**: The QNX pathmgr's `O_CREAT|O_EXCL` flow sends the `OPEN`
message first (which creates the file), then sends a `COMBINE_CLOSE`
post-check to verify whether the file pre-existed.  If we return `EOK` for
that post-check, the pathmgr concludes the file pre-existed and raises
`EEXIST` to the caller (making O_EXCL appear to always fail).

Our fix — clearing `O_EXCL` from `msg->connect.ioflag` after creating the
file — causes the pathmgr to proceed without the EEXIST check.  The post-check
`COMBINE_CLOSE` then falls through normally and `os.path.exists` works.

The transient `False` from `os.path.exists` was an artefact of an earlier
(now reverted) `excl_pending` workaround.  With the current implementation,
`os.path.exists` returns the correct result.

**Current status**: Resolved.  `O_CREAT|O_EXCL` creates files correctly and
`os.path.exists` returns `True` after creation.

---

## ~~TODO-4~~: `statvfs` f_ffree always reports 0 — **RESOLVED**

**Resolution**: Implemented per-mount inode cap via `-o nr_inodes=N` option.
`statvfs` now reports:
- `f_files`  = `inode_cap` (the limit)
- `f_ffree`  = `inode_cap - inode_count` (free slots)
- `f_favail` = same as `f_ffree`

**Default cap** mirrors Linux tmpfs: `total_ram / (PAGE_SIZE × 2)` = half the
number of physical RAM pages.  On a 32 GiB machine this is 4,194,304 inodes.

**Enforcement**: allocation of any inode (file, directory, symlink) checks the
cap atomically via a CAS loop and returns `ENOSPC` when full.  See
Implementation Note 27.

---

## TODO-5: No `xattr` (extended attribute) support

Extended attributes (`setxattr`, `getxattr`, `listxattr`, `removexattr`) are
not implemented.  The `io_devctl` handler returns `ENOSYS` for all devctl
commands except `DCMD_FSYS_STATVFS`.

Extended attributes are not required for basic POSIX compliance but are needed
for systems that use them for security labels (SELinux, AppArmor) or for tools
like `rsync --xattrs`.

---

## TODO-6: No `fcntl` advisory locking (`F_SETLK` / `F_SETLKW`)

`fcntl(2)` advisory byte-range locking is not implemented.  The `io_lock`
handler is `NULL` (returns `ENOSYS`).  `flock(2)` is similarly unimplemented.

Most in-memory filesystem uses don't require locking, but applications that
rely on advisory locks for inter-process coordination will not work correctly.

---

## TODO-7: `O_SYNC` / `O_DSYNC` are accepted but silently ignored

`O_SYNC` and `O_DSYNC` are listed in the standard mount options whitelist and
silently ignored.  Since tmpfs is memory-backed there is no meaningful "sync to
storage", so this is arguably correct, but applications that pass these flags
expecting synchronous-write ordering guarantees will not see any difference.

---

## Notes on QNX FTYPE_MOUNT Constraints

Several limitations above stem from fundamental QNX FTYPE_MOUNT resmgr
behaviour rather than implementation choices:

- **`COMBINE_CLOSE` ambiguity**: The pathmgr uses `COMBINE_CLOSE` (subtype=1)
  for stat/lstat/access AND for O_EXCL post-checks.  These two uses are
  structurally indistinguishable at the resmgr level.

- **`_IO_MMAP` SERVER_SHMEM_OBJECT**: The memmgr's fd lookup for this mechanism
  appears to require a kernel-managed fd reference that user-space servers
  cannot supply via normal fd numbers.

- **`mount(8)` exit code**: `mount -t tmpfs` reliably returns exit code 29
  (ENOENT) even when the mount succeeds.  The mount IS functional; the exit
  code is from the mount(8) binary's own post-mount validation, not from our
  driver.  The test suite accounts for this by using `check=False` and
  verifying coordinator startup independently.
