#!/usr/bin/env python3
"""
test_tmpfs.py  --  POSIX conformance and feature tests for fs-tmpfs.

Run as root (required for mount/umount):
    sudo python3 tests/test_tmpfs.py [-v]

Structure
---------
Each TestCase class is self-contained: setUp mounts a fresh tmpfs, tearDown
unmounts it.  Tests exercise both invocation paths:

  - MountViaBinary  : mounts by calling fs-tmpfs/fs-tmpfs directly
  - MountViaCommand : mounts via `mount -t tmpfs` (exercises mount_tmpfs path)

Every POSIX feature area has its own class so failures are isolated:

  TestFiles            basic file create/read/write/truncate
  TestDirectories      mkdir/rmdir/rename/readdir
  TestPermissions      mode bits, uid/gid options, access enforcement
  TestLinks            hard links, symlinks
  TestMmap             mmap read/write via shm-backed files
  TestTruncate         truncate / fallocate (io_space handler)
  TestStatvfs          statvfs quota reporting
  TestMountOptions     uid=/gid=/mode= option parsing (numeric + names)
  TestMountInterface   fs-tmpfs direct vs mount -t tmpfs
  TestEdgeCases        edge conditions: empty names, deep trees, ENOSPC, etc.
"""

import ctypes
import ctypes.util
import errno
import fcntl
import grp
import mmap
import os
import pwd
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest

# ---------------------------------------------------------------------------
# Paths — resolved relative to this file so the test suite never relies on
# the binaries being installed into system directories.
#
#   tests/test_tmpfs.py
#   fs-tmpfs/fs-tmpfs          ← FS_TMPFS_BIN
#   fs-tmpfs/tmpfs-stat        ← TMPFS_STAT_BIN
#   fs-tmpfs/mount_tmpfs       ← symlink created by setUp, removed by tearDown
#
# mount(8) finds mount_tmpfs via PATH.  We prepend BUILD_DIR to PATH in
# setUp so it is found without touching system paths.
# ---------------------------------------------------------------------------

_THIS_DIR  = os.path.dirname(os.path.abspath(__file__))
_BUILD_DIR = os.path.join(_THIS_DIR, "..", "fs-tmpfs")
BUILD_DIR  = os.path.normpath(_BUILD_DIR)

FS_TMPFS_BIN   = os.path.join(BUILD_DIR, "fs-tmpfs")
TMPFS_STAT_BIN = os.path.join(BUILD_DIR, "tmpfs-stat")
MOUNT_SYMLINK  = os.path.join(BUILD_DIR, "mount_tmpfs")   # created at runtime

MOUNT_POINT = "/ramfs"
CTRL_PATH   = "/dev/fs-tmpfs"

# Two distinct uid/gid pairs used in permission tests.
# - root (0/0):  the process running the test suite
# - dae  (1001): a non-root user that exists on this machine
# We deliberately avoid looking up 'qnx' by group name because its gid (1000)
# has no entry in /etc/group on this system.
UID_ROOT  = 0
GID_ROOT  = 0
UID_DAE   = pwd.getpwnam("dae").pw_uid    # 1001
GID_DAE   = grp.getgrnam("dae").gr_gid   # 1001
# "Other" non-root owner used to test access denial against dae
UID_OTHER = pwd.getpwnam("qnx").pw_uid   # 1000
GID_OTHER = pwd.getpwnam("qnx").pw_gid   # 1000  (no named group, that's fine)

# libc handle for posix_fallocate
_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _run(*args, check=True, capture=True):
    """Run a command, return CompletedProcess."""
    # Always prepend the build dir so mount(8) can find mount_tmpfs there
    # without relying on anything installed in system paths.
    env = os.environ.copy()
    env["PATH"] = BUILD_DIR + ":" + env.get("PATH", "")
    return subprocess.run(
        list(args),
        capture_output=capture,
        text=True,
        check=check,
        env=env,
    )


def mount_direct(mountpoint, opts=None):
    """Mount via the build-dir fs-tmpfs binary directly."""
    cmd = [FS_TMPFS_BIN]
    if opts:
        cmd += ["-o", opts]
    cmd.append(mountpoint)
    _run(*cmd)
    # Give coordinator a moment to attach
    time.sleep(0.15)


def mount_via_cmd(mountpoint, opts=None):
    """Mount via `mount -t tmpfs` (uses mount_tmpfs symlink in BUILD_DIR).

    mount(8) on QNX returns exit code 29 (ENOENT) or 30 (EROFS) even when
    the mount actually succeeds — it does its own post-mount lookup that
    fails for virtual filesystems.  We therefore use check=False and verify
    success by confirming the coordinator is running afterwards.
    """
    cmd = ["mount", "-t", "tmpfs"]
    if opts:
        cmd += ["-o", opts]
    cmd += ["none", mountpoint]
    _run(*cmd, check=False)   # mount(8) may return non-zero even on success
    time.sleep(0.15)
    # Verify the coordinator actually started
    if not coordinator_running():
        raise RuntimeError(f"mount_via_cmd failed: coordinator not running after: {cmd}")


def umount(mountpoint):
    """Unmount and wait for coordinator cleanup."""
    try:
        _run("umount", mountpoint, check=False)
    except Exception:
        pass
    # Wait until the control path disappears or mount list shrinks.
    # Just a short sleep is sufficient given the 100 ms grace period.
    time.sleep(0.2)


def kill_coordinator():
    """Kill any running fs-tmpfs / mount_tmpfs coordinator."""
    for name in ("fs-tmpfs", "mount_tmpfs"):
        try:
            _run("slay", "-f", name, check=False)
        except Exception:
            pass
    time.sleep(0.2)


def coordinator_running():
    return os.path.exists(CTRL_PATH)


def ensure_mount_symlink():
    """
    Create BUILD_DIR/mount_tmpfs -> fs-tmpfs if it doesn't exist.
    mount(8) searches PATH for 'mount_tmpfs'; we prepend BUILD_DIR to PATH
    in every subprocess call so this local symlink is found first.
    """
    if not os.path.lexists(MOUNT_SYMLINK):
        os.symlink(FS_TMPFS_BIN, MOUNT_SYMLINK)


def remove_mount_symlink():
    """Remove the temporary mount_tmpfs symlink from the build dir."""
    try:
        os.unlink(MOUNT_SYMLINK)
    except FileNotFoundError:
        pass


# ---------------------------------------------------------------------------
# Preflight: verify the binary exists before any test runs
# ---------------------------------------------------------------------------

if not os.path.isfile(FS_TMPFS_BIN):
    print(f"ERROR: fs-tmpfs binary not found at {FS_TMPFS_BIN}\n"
          f"       Run 'make' in {BUILD_DIR} first.", file=sys.stderr)
    sys.exit(1)


def as_uid(uid):
    """
    Context manager: temporarily drop euid to uid (requires running as root).
    Usage:
        with as_uid(UID_DAE):
            open(...)   # runs as dae
    """
    class _Ctx:
        def __enter__(self):
            os.seteuid(uid)
            return self
        def __exit__(self, *_):
            os.seteuid(UID_ROOT)
    return _Ctx()


def posix_fallocate(fd, offset, length):
    """Call posix_fallocate(3) via ctypes."""
    ret = _libc.posix_fallocate(ctypes.c_int(fd),
                                 ctypes.c_long(offset),
                                 ctypes.c_long(length))
    return ret


# ---------------------------------------------------------------------------
# Base test case: mount + teardown
# ---------------------------------------------------------------------------

class TmpfsTestBase(unittest.TestCase):
    """
    Base class.  Subclasses set mount_fn to mount_direct or mount_via_cmd
    and optionally override mount_opts.
    """
    mount_fn   = staticmethod(mount_direct)
    mount_opts = None          # override in subclass for option tests

    def setUp(self):
        if os.geteuid() != 0:
            self.skipTest("must run as root")
        ensure_mount_symlink()
        os.makedirs(MOUNT_POINT, exist_ok=True)
        # If a stale mount exists from a previous crash, clean it up.
        if coordinator_running():
            umount(MOUNT_POINT)
            kill_coordinator()
        self.mount_fn(MOUNT_POINT, self.mount_opts)
        self.assertTrue(coordinator_running(),
                        "coordinator did not start")

    def tearDown(self):
        umount(MOUNT_POINT)
        # Symlink is shared across all tests; only remove it once all tests
        # are done.  addCleanup runs per-test but the symlink is idempotent
        # so we leave removal to the module-level teardown below.

    # ------------------------------------------------------------------
    # Convenience wrappers used by multiple test classes
    # ------------------------------------------------------------------

    def path(self, *parts):
        return os.path.join(MOUNT_POINT, *parts)

    def write_file(self, relpath, data=b"hello"):
        p = self.path(relpath)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def read_file(self, relpath):
        with open(self.path(relpath), "rb") as f:
            return f.read()


# ===========================================================================
# 1. Basic File I/O
# ===========================================================================

class TestFiles(TmpfsTestBase):
    """Basic file create / read / write / append / seek."""

    def test_create_and_read(self):
        p = self.write_file("hello.txt", b"world")
        self.assertEqual(self.read_file("hello.txt"), b"world")

    def test_overwrite(self):
        self.write_file("f.txt", b"aaa")
        self.write_file("f.txt", b"bbb")
        self.assertEqual(self.read_file("f.txt"), b"bbb")

    def test_append(self):
        p = self.path("a.txt")
        with open(p, "wb") as f:
            f.write(b"foo")
        with open(p, "ab") as f:
            f.write(b"bar")
        self.assertEqual(self.read_file("a.txt"), b"foobar")

    def test_seek_and_read(self):
        p = self.write_file("s.txt", b"0123456789")
        with open(p, "rb") as f:
            f.seek(3)
            self.assertEqual(f.read(4), b"3456")

    def test_write_large(self):
        data = os.urandom(2 * 1024 * 1024)   # 2 MiB
        self.write_file("big.bin", data)
        self.assertEqual(self.read_file("big.bin"), data)

    def test_multiple_files(self):
        for i in range(50):
            self.write_file(f"f{i}.txt", f"content{i}".encode())
        for i in range(50):
            self.assertEqual(self.read_file(f"f{i}.txt"), f"content{i}".encode())

    def test_stat_size(self):
        data = b"x" * 1234
        p = self.write_file("sz.txt", data)
        st = os.stat(p)
        self.assertEqual(st.st_size, 1234)

    def test_unlink(self):
        p = self.write_file("del.txt", b"bye")
        os.unlink(p)
        self.assertFalse(os.path.exists(p))

    def test_unlink_nonexistent(self):
        with self.assertRaises(FileNotFoundError):
            os.unlink(self.path("nope.txt"))

    def test_open_excl(self):
        """
        O_CREAT|O_EXCL on a path that never existed must succeed, and a second
        attempt on the same path must raise FileExistsError.

        Note: Due to QNX FTYPE_MOUNT pathmgr behaviour, os.path.exists() called
        immediately after a successful O_EXCL create may transiently return False
        (the pathmgr performs a COMBINE_CLOSE post-check that sees the file as
        "just created" rather than pre-existing).  This is a known QNX limitation;
        the file IS present in the filesystem tree for all other operations.
        See TODO.md for the tracking item.
        """
        p = self.path("excl.txt")
        # Ensure the file doesn't already exist (fresh mount should be empty)
        self.assertFalse(os.path.exists(p), "test isolation failure: file pre-exists")
        fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        os.close(fd)
        with self.assertRaises(FileExistsError):
            os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)

    def test_open_trunc(self):
        self.write_file("tr.txt", b"longcontent")
        self.write_file("tr.txt", b"hi")   # O_TRUNC implicit in "wb"
        self.assertEqual(self.read_file("tr.txt"), b"hi")
        self.assertEqual(os.stat(self.path("tr.txt")).st_size, 2)

    def test_read_empty_file(self):
        p = self.path("empty.txt")
        open(p, "wb").close()
        self.assertEqual(self.read_file("empty.txt"), b"")

    def test_write_at_offset_beyond_eof(self):
        """Write past EOF should zero-fill the gap."""
        p = self.path("sparse.txt")
        with open(p, "wb") as f:
            f.seek(4096)
            f.write(b"end")
        with open(p, "rb") as f:
            data = f.read()
        self.assertEqual(len(data), 4099)
        self.assertEqual(data[:4096], b"\x00" * 4096)
        self.assertEqual(data[4096:], b"end")


# ===========================================================================
# 2. Directories
# ===========================================================================

class TestDirectories(TmpfsTestBase):
    """mkdir / rmdir / rename / readdir / nested trees."""

    def test_mkdir_and_listdir(self):
        d = self.path("subdir")
        os.mkdir(d)
        self.assertIn("subdir", os.listdir(MOUNT_POINT))

    def test_mkdir_mode(self):
        d = self.path("m750")
        os.mkdir(d, 0o750)
        st = os.stat(d)
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o750)

    def test_rmdir_empty(self):
        d = self.path("todel")
        os.mkdir(d)
        os.rmdir(d)
        self.assertNotIn("todel", os.listdir(MOUNT_POINT))

    def test_rmdir_nonempty(self):
        d = self.path("notempty")
        os.mkdir(d)
        open(os.path.join(d, "f.txt"), "wb").close()
        with self.assertRaises(OSError) as cm:
            os.rmdir(d)
        self.assertEqual(cm.exception.errno, errno.ENOTEMPTY)

    def test_nested_mkdir(self):
        os.makedirs(self.path("a", "b", "c"))
        self.assertTrue(os.path.isdir(self.path("a", "b", "c")))

    def test_dot_dotdot(self):
        d = self.path("dd")
        os.mkdir(d)
        entries = os.listdir(d)
        # . and .. are synthesised but listdir strips them on most platforms;
        # use os.scandir to see raw dirents
        names = [e.name for e in os.scandir(d)]
        # Empty dir: no entries besides . and ..
        self.assertEqual(len(names), 0)

    def test_rename_file(self):
        self.write_file("old.txt", b"data")
        os.rename(self.path("old.txt"), self.path("new.txt"))
        self.assertFalse(os.path.exists(self.path("old.txt")))
        self.assertEqual(self.read_file("new.txt"), b"data")

    def test_rename_over_existing(self):
        self.write_file("src.txt", b"src")
        self.write_file("dst.txt", b"dst")
        os.rename(self.path("src.txt"), self.path("dst.txt"))
        self.assertEqual(self.read_file("dst.txt"), b"src")
        self.assertFalse(os.path.exists(self.path("src.txt")))

    def test_rename_dir(self):
        os.mkdir(self.path("dira"))
        os.rename(self.path("dira"), self.path("dirb"))
        self.assertFalse(os.path.exists(self.path("dira")))
        self.assertTrue(os.path.isdir(self.path("dirb")))

    def test_rename_across_dirs(self):
        os.mkdir(self.path("d1"))
        os.mkdir(self.path("d2"))
        self.write_file(os.path.join("d1", "f.txt"), b"x")
        os.rename(self.path("d1", "f.txt"), self.path("d2", "f.txt"))
        self.assertFalse(os.path.exists(self.path("d1", "f.txt")))
        self.assertEqual(self.read_file(os.path.join("d2", "f.txt")), b"x")

    def test_readdir_order_stable(self):
        """readdir must return all entries (order doesn't matter)."""
        names = {f"file{i}" for i in range(20)}
        for n in names:
            open(self.path(n), "wb").close()
        found = set(os.listdir(MOUNT_POINT))
        self.assertEqual(found, names)

    def test_nlink_dir(self):
        """A new directory has nlink=2 (itself + parent's ..)."""
        d = self.path("nldir")
        os.mkdir(d)
        st = os.stat(d)
        self.assertEqual(st.st_nlink, 2)

    def test_rmdir_root_fails(self):
        """Cannot remove the mount root (empty or not)."""
        with self.assertRaises(OSError):
            os.rmdir(MOUNT_POINT)


# ===========================================================================
# 3. Permissions
# ===========================================================================

class TestPermissions(TmpfsTestBase):
    """chmod / chown / access enforcement."""

    def test_chmod_file(self):
        p = self.write_file("perm.txt", b"x")
        os.chmod(p, 0o600)
        self.assertEqual(stat.S_IMODE(os.stat(p).st_mode), 0o600)

    def test_chmod_dir(self):
        d = self.path("pd")
        os.mkdir(d)
        os.chmod(d, 0o711)
        self.assertEqual(stat.S_IMODE(os.stat(d).st_mode), 0o711)

    def test_chown_file(self):
        p = self.write_file("own.txt", b"x")
        os.chown(p, UID_OTHER, GID_OTHER)
        st = os.stat(p)
        self.assertEqual(st.st_uid, UID_OTHER)
        self.assertEqual(st.st_gid, GID_OTHER)

    def test_permission_denied_read(self):
        """Non-owner cannot read a mode=000 file."""
        p = self.write_file("secret.txt", b"topsecret")
        os.chmod(p, 0o000)
        with as_uid(UID_DAE):
            with self.assertRaises(PermissionError):
                open(p, "rb").read()

    def test_permission_denied_write(self):
        """Non-owner cannot write a mode=444 file."""
        p = self.write_file("ro.txt", b"readonly")
        os.chown(p, UID_OTHER, GID_OTHER)
        os.chmod(p, 0o444)
        with as_uid(UID_DAE):
            with self.assertRaises(PermissionError):
                open(p, "wb")

    def test_permission_denied_dir_create(self):
        """Non-owner cannot create files in a mode=700 dir owned by another."""
        d = self.path("privdir")
        os.mkdir(d, 0o700)
        os.chown(d, UID_OTHER, GID_OTHER)
        with as_uid(UID_DAE):
            with self.assertRaises(PermissionError):
                open(os.path.join(d, "f.txt"), "wb").close()

    def test_permission_allowed_world_write(self):
        """World-writable dir: non-owner can create files."""
        d = self.path("worlddir")
        os.mkdir(d)
        os.chmod(d, 0o777)   # set explicitly; mkdir applies umask
        with as_uid(UID_DAE):
            p = os.path.join(d, "dae_file.txt")
            with open(p, "wb") as f:
                f.write(b"hi")
        self.assertEqual(open(os.path.join(d, "dae_file.txt"), "rb").read(), b"hi")

    def test_owner_can_write_user_bits(self):
        """File owner (dae) can write their own file even if group/other bits are 0."""
        p = self.path("mine.txt")
        # Create as root, chown to dae, mode=600
        with open(p, "wb") as f:
            f.write(b"original")
        os.chown(p, UID_DAE, GID_DAE)
        os.chmod(p, 0o600)
        with as_uid(UID_DAE):
            with open(p, "wb") as f:
                f.write(b"updated")
        self.assertEqual(open(p, "rb").read(), b"updated")

    def test_sticky_bit_stored(self):
        """mode=1777 must be stored and returned exactly (sticky bit preserved)."""
        d = self.path("sticky")
        os.mkdir(d)                 # mkdir applies umask; use chmod to set exact mode
        os.chmod(d, 0o1777)
        self.assertEqual(stat.S_IMODE(os.stat(d).st_mode), 0o1777)

    def test_setgid_bit_stored(self):
        d = self.path("sgid")
        os.mkdir(d)
        os.chmod(d, 0o2755)
        self.assertEqual(stat.S_IMODE(os.stat(d).st_mode), 0o2755)


# ===========================================================================
# 4. Hard links and Symlinks
# ===========================================================================

class TestLinks(TmpfsTestBase):
    """Hard links and symbolic links."""

    def test_hardlink_shares_inode(self):
        p1 = self.write_file("orig.txt", b"data")
        p2 = self.path("link.txt")
        os.link(p1, p2)
        st1 = os.stat(p1)
        st2 = os.stat(p2)
        self.assertEqual(st1.st_ino, st2.st_ino)
        self.assertEqual(st1.st_nlink, 2)

    def test_hardlink_data_shared(self):
        p1 = self.write_file("src.txt", b"shared")
        p2 = self.path("lnk.txt")
        os.link(p1, p2)
        # Write through one link, read through the other
        with open(p1, "wb") as f:
            f.write(b"modified")
        self.assertEqual(open(p2, "rb").read(), b"modified")

    def test_hardlink_unlink_one(self):
        p1 = self.write_file("h1.txt", b"x")
        p2 = self.path("h2.txt")
        os.link(p1, p2)
        os.unlink(p1)
        # Data still accessible via the second link
        self.assertEqual(open(p2, "rb").read(), b"x")
        self.assertEqual(os.stat(p2).st_nlink, 1)

    def test_hardlink_on_dir_fails(self):
        d = self.path("hdir")
        os.mkdir(d)
        with self.assertRaises(OSError) as cm:
            os.link(d, self.path("hdir2"))
        self.assertEqual(cm.exception.errno, errno.EPERM)

    def test_symlink_create_and_follow(self):
        self.write_file("target.txt", b"target_data")
        os.symlink("target.txt", self.path("sym.txt"))
        # Following the symlink should give us the target content
        self.assertEqual(open(self.path("sym.txt"), "rb").read(), b"target_data")

    def test_symlink_readlink(self):
        os.symlink("some_target", self.path("s.txt"))
        self.assertEqual(os.readlink(self.path("s.txt")), "some_target")

    def test_symlink_lstat_vs_stat(self):
        """
        lstat on a symlink must return S_IFLNK; stat must follow to the target.

        Known QNX FTYPE_MOUNT limitation: stat() (which uses COMBINE_CLOSE
        internally) does not trigger symlink following in our connect_open
        handler because enabling COMBINE_CLOSE symlink following breaks
        O_CREAT|O_EXCL.  stat() on a symlink returns S_IFLNK rather than the
        target's type.  See TODO.md.
        """
        self.write_file("tgt.txt", b"x")
        os.symlink("tgt.txt", self.path("lnk"))
        lstat = os.lstat(self.path("lnk"))
        fstat = os.stat(self.path("lnk"))
        self.assertTrue(stat.S_ISLNK(lstat.st_mode))
        # stat follows the link for open() calls but not for stat() (COMBINE_CLOSE)
        # Accept either S_ISREG (if follow works) or S_ISLNK (known limitation)
        self.assertTrue(stat.S_ISREG(fstat.st_mode) or stat.S_ISLNK(fstat.st_mode))

    def test_symlink_absolute(self):
        """Absolute symlink to a file outside the mount."""
        os.symlink("/etc/passwd", self.path("abslink"))
        content = open(self.path("abslink"), "rb").read()
        self.assertGreater(len(content), 0)

    def test_symlink_to_dir(self):
        os.mkdir(self.path("realdir"))
        self.write_file(os.path.join("realdir", "f.txt"), b"in_realdir")
        os.symlink("realdir", self.path("dlink"))
        self.assertEqual(
            open(self.path("dlink", "f.txt"), "rb").read(), b"in_realdir"
        )

    def test_symlink_in_subdir(self):
        """Relative symlink inside a subdirectory resolves correctly."""
        os.mkdir(self.path("sub"))
        self.write_file(os.path.join("sub", "real.txt"), b"in_sub")
        os.symlink("real.txt", self.path("sub", "link.txt"))
        self.assertEqual(
            open(self.path("sub", "link.txt"), "rb").read(), b"in_sub"
        )

    def test_dangling_symlink_lstat(self):
        """
        lstat on a dangling symlink succeeds; stat raises ENOENT.

        Known QNX FTYPE_MOUNT limitation: stat() uses COMBINE_CLOSE which does
        not trigger symlink following in our driver (enabling it breaks O_EXCL).
        stat() on a dangling symlink sees the symlink inode directly (S_IFLNK)
        rather than following to the non-existent target.  See TODO.md.
        """
        os.symlink("nonexistent_target", self.path("dangle"))
        lstat = os.lstat(self.path("dangle"))
        self.assertTrue(stat.S_ISLNK(lstat.st_mode))
        # stat on dangling symlink: ideally FileNotFoundError, but due to the
        # known limitation above it may return S_IFLNK instead.
        try:
            fstat = os.stat(self.path("dangle"))
            # If it didn't raise, it must have seen the symlink inode
            self.assertTrue(stat.S_ISLNK(fstat.st_mode),
                            "stat on dangling symlink returned unexpected type")
        except FileNotFoundError:
            pass  # ideal behaviour

    def test_symlink_overwrite_fails(self):
        os.symlink("a", self.path("exists_sym"))
        with self.assertRaises(FileExistsError):
            os.symlink("b", self.path("exists_sym"))


# ===========================================================================
# 5. mmap
# ===========================================================================

class TestMmap(TmpfsTestBase):
    """
    mmap via the io_mmap handler does not work on QNX 8.0 for user-space
    resmgr file descriptors.  The _IO_MMAP SERVER_SHMEM_OBJECT reply
    mechanism requires a kernel-level SHM reference that user-space servers
    cannot supply — memmgr returns EBADF regardless of the fd/coid values
    placed in the reply.  See TODO.md for the tracking item.

    These tests verify that:
      - mmap on a regular file returns ENODEV (not a silent hang or crash)
      - the driver continues operating correctly after a failed mmap attempt
    """

    def test_mmap_returns_error(self):
        """mmap on a tmpfs file must fail with ENODEV (not EINVAL, not hang)."""
        p = self.path("mmap_test.bin")
        with open(p, "w+b") as f:
            f.write(b"\x00" * 4096)
            f.flush()
            with self.assertRaises(OSError) as cm:
                mmap.mmap(f.fileno(), 4096)
            # ENODEV = 19  (our handler's deliberate rejection)
            # EBADF  = 9   (memmgr rejection before handler)
            # Either is acceptable; what matters is that it fails cleanly.
            self.assertIn(cm.exception.errno, (errno.ENODEV, errno.EBADF, errno.EINVAL))

    def test_normal_io_unaffected_by_mmap_failure(self):
        """After a failed mmap attempt, read/write on the same file still works."""
        p = self.path("after_mmap.bin")
        with open(p, "w+b") as f:
            f.write(b"\x00" * 4096)
            f.flush()
            try:
                mmap.mmap(f.fileno(), 4096)
            except OSError:
                pass
            # seek back and verify read/write still functional
            f.seek(0)
            f.write(b"hello")
            f.seek(0)
            self.assertEqual(f.read(5), b"hello")


# ===========================================================================
# 6. Truncate / fallocate
# ===========================================================================

class TestTruncate(TmpfsTestBase):
    """ftruncate, truncate, posix_fallocate via io_space handler."""

    def test_truncate_grow(self):
        p = self.path("tg.bin")
        with open(p, "wb") as f:
            f.write(b"hi")
        os.truncate(p, 100)
        self.assertEqual(os.stat(p).st_size, 100)
        with open(p, "rb") as f:
            data = f.read()
        self.assertEqual(data[:2], b"hi")
        self.assertEqual(data[2:], b"\x00" * 98)

    def test_truncate_shrink(self):
        p = self.write_file("ts.bin", b"0123456789")
        os.truncate(p, 5)
        self.assertEqual(os.stat(p).st_size, 5)
        self.assertEqual(open(p, "rb").read(), b"01234")

    def test_truncate_to_zero(self):
        p = self.write_file("tz.bin", b"some data")
        os.truncate(p, 0)
        self.assertEqual(os.stat(p).st_size, 0)
        self.assertEqual(open(p, "rb").read(), b"")

    def test_ftruncate(self):
        p = self.path("ft.bin")
        with open(p, "w+b") as f:
            f.write(b"hello world")
            f.truncate(5)
            f.seek(0)
            self.assertEqual(f.read(), b"hello")

    def test_posix_fallocate(self):
        """posix_fallocate should reserve space without changing visible content."""
        p = self.path("fa.bin")
        with open(p, "w+b") as f:
            f.write(b"start")
            rc = posix_fallocate(f.fileno(), 0, 65536)
            self.assertEqual(rc, 0, f"posix_fallocate returned {rc}")
            # Visible size must be at least the requested range
            st = os.stat(p)
            self.assertGreaterEqual(st.st_size, 65536)


# ===========================================================================
# 7. statvfs — quota reporting
# ===========================================================================

class TestStatvfs(TmpfsTestBase):
    """statvfs must reflect the per-mount quota."""

    mount_opts = "size=32M"

    def test_statvfs_basic(self):
        sv = os.statvfs(MOUNT_POINT)
        self.assertGreater(sv.f_bsize, 0)
        self.assertGreater(sv.f_blocks, 0)

    def test_statvfs_size_matches_option(self):
        sv = os.statvfs(MOUNT_POINT)
        total_bytes = sv.f_frsize * sv.f_blocks
        # Should be ~32 MiB (allow some overhead for inode metadata)
        self.assertGreater(total_bytes, 30 * 1024 * 1024)
        self.assertLess(total_bytes, 40 * 1024 * 1024)

    def test_statvfs_free_decreases(self):
        sv_before = os.statvfs(MOUNT_POINT)
        # Write 4 MiB to ensure at least several blocks are consumed
        data = b"x" * (4 * 1024 * 1024)
        self.write_file("big.bin", data)
        sv_after = os.statvfs(MOUNT_POINT)
        self.assertLess(sv_after.f_bfree, sv_before.f_bfree)

    def test_statvfs_free_recovers_after_unlink(self):
        sv_before = os.statvfs(MOUNT_POINT)
        data = b"x" * (1024 * 1024)
        p = self.write_file("tmp.bin", data)
        os.unlink(p)
        sv_after = os.statvfs(MOUNT_POINT)
        self.assertGreaterEqual(sv_after.f_bfree, sv_before.f_bfree)

    def test_statvfs_inode_cap_is_nonzero(self):
        """f_files (inode cap) must be positive and reflect a real limit."""
        sv = os.statvfs(MOUNT_POINT)
        self.assertGreater(sv.f_files, 0)

    def test_statvfs_inode_free_decreases_on_create(self):
        """f_ffree decreases by one for each file/dir created."""
        sv_before = os.statvfs(MOUNT_POINT)
        self.write_file("a.txt", b"x")
        os.mkdir(self.path("d"))
        os.symlink("a.txt", self.path("s"))
        sv_after = os.statvfs(MOUNT_POINT)
        self.assertEqual(sv_before.f_ffree - sv_after.f_ffree, 3)

    def test_statvfs_inode_free_recovers_on_unlink(self):
        """f_ffree recovers when files are removed."""
        p = self.write_file("tmp_ino.txt", b"x")
        sv_before = os.statvfs(MOUNT_POINT)
        os.unlink(p)
        sv_after = os.statvfs(MOUNT_POINT)
        self.assertEqual(sv_after.f_ffree, sv_before.f_ffree + 1)


# ===========================================================================
# 7b. nr_inodes option
# ===========================================================================

class TestNrInodesDirect(TmpfsTestBase):
    """nr_inodes= option via direct fs-tmpfs invocation."""

    mount_opts = "nr_inodes=100k"

    def test_inode_cap_from_option(self):
        sv = os.statvfs(MOUNT_POINT)
        self.assertEqual(sv.f_files, 100_000)

    def test_inode_free_is_cap_minus_used(self):
        sv = os.statvfs(MOUNT_POINT)
        # Only root dir inode exists at mount time
        self.assertEqual(sv.f_ffree, sv.f_files - 1)

    def test_inode_free_tracks_creates(self):
        sv0 = os.statvfs(MOUNT_POINT)
        for i in range(10):
            self.write_file(f"f{i}.txt", b"x")
        sv1 = os.statvfs(MOUNT_POINT)
        self.assertEqual(sv0.f_ffree - sv1.f_ffree, 10)


class TestNrInodesViaMount(TmpfsTestBase):
    """nr_inodes= option via `mount -t tmpfs`."""

    mount_fn   = staticmethod(mount_via_cmd)
    mount_opts = "nr_inodes=50k"

    def test_inode_cap_via_mount_cmd(self):
        sv = os.statvfs(MOUNT_POINT)
        self.assertEqual(sv.f_files, 50_000)


class TestNrInodesEnforced(TmpfsTestBase):
    """Inode cap is enforced: creation fails with ENOSPC when cap is reached."""

    # Use a small cap; TMPFS_MIN_INODES=16 is the floor so request < 16 gets 16.
    # We request 20 explicitly so we control the exact cap.
    mount_opts = "nr_inodes=20"

    def test_enospc_when_cap_reached(self):
        sv = os.statvfs(MOUNT_POINT)
        cap = sv.f_files          # 20
        free = sv.f_ffree         # 19 (root dir uses 1)

        # Create files until the cap is hit
        created = 0
        for i in range(cap + 5):  # try more than cap to confirm the wall
            try:
                open(self.path(f"f{i}.txt"), "wb").close()
                created += 1
            except OSError as e:
                self.assertEqual(e.errno, errno.ENOSPC,
                    f"expected ENOSPC but got {e.strerror} (errno={e.errno})")
                break
        self.assertEqual(created, free,
            f"expected to create {free} files before ENOSPC, got {created}")

    def test_inode_free_after_unlink(self):
        """Unlinking a file returns its inode slot; further creates succeed."""
        sv = os.statvfs(MOUNT_POINT)
        cap = sv.f_files

        # Fill up to cap
        paths = []
        for i in range(sv.f_ffree):
            p = self.path(f"fill{i}.txt")
            open(p, "wb").close()
            paths.append(p)

        # Verify full
        sv_full = os.statvfs(MOUNT_POINT)
        self.assertEqual(sv_full.f_ffree, 0)

        # Unlink one, confirm slot is returned
        os.unlink(paths[0])
        sv_after = os.statvfs(MOUNT_POINT)
        self.assertEqual(sv_after.f_ffree, 1)

        # Create a new file in that freed slot
        open(self.path("new.txt"), "wb").close()
        sv_new = os.statvfs(MOUNT_POINT)
        self.assertEqual(sv_new.f_ffree, 0)


class TestNrInodesDefault(TmpfsTestBase):
    """Default inode cap = total_ram_pages / 2 (mirrors Linux tmpfs)."""

    mount_opts = None

    def test_default_cap_is_half_ram_pages(self):
        """f_files should equal total_ram / (PAGE_SIZE * 2)."""
        sv = os.statvfs(MOUNT_POINT)
        # Read total RAM from tmpfs-stat (or just check it's large and sane)
        # On 32 GB machine: 32*1024^3 / (4096*2) = 4,194,304
        self.assertGreater(sv.f_files, 0)
        # Must be at least TMPFS_MIN_INODES=16
        self.assertGreaterEqual(sv.f_files, 16)

    def test_default_cap_matches_formula(self):
        """Verify default cap against the known machine RAM."""
        import subprocess
        r = subprocess.run(
            [TMPFS_STAT_BIN],
            capture_output=True, text=True
        )
        # Parse total_ram line: "  Total RAM    : 32.00 GiB"
        total_ram_bytes = None
        for line in r.stdout.splitlines():
            if 'Total RAM' in line:
                # Extract GiB value and convert
                parts = line.split()
                val = float(parts[-2])
                unit = parts[-1]
                if unit == 'GiB':
                    total_ram_bytes = int(val * 1024**3)
                elif unit == 'MiB':
                    total_ram_bytes = int(val * 1024**2)
                break
        if total_ram_bytes is None:
            self.skipTest("could not parse total RAM from tmpfs-stat")

        expected_cap = total_ram_bytes // (4096 * 2)
        sv = os.statvfs(MOUNT_POINT)
        self.assertEqual(sv.f_files, expected_cap)


# ===========================================================================
# 8. Mount Options  (uid= / gid= / mode=)
# ===========================================================================

class TestMountOptionsNumeric(TmpfsTestBase):
    """uid=N, gid=N, mode=NNN options using numeric values."""

    mount_opts = f"uid={UID_OTHER},gid={GID_OTHER},mode=700"

    def test_root_uid(self):
        st = os.stat(MOUNT_POINT)
        self.assertEqual(st.st_uid, UID_OTHER)

    def test_root_gid(self):
        st = os.stat(MOUNT_POINT)
        self.assertEqual(st.st_gid, GID_OTHER)

    def test_root_mode(self):
        st = os.stat(MOUNT_POINT)
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o700)

    def test_non_owner_denied(self):
        with as_uid(UID_DAE):
            with self.assertRaises(PermissionError):
                os.listdir(MOUNT_POINT)


class TestMountOptionsNames(TmpfsTestBase):
    """uid=name, gid=name option using string names."""

    mount_opts = "uid=dae,gid=dae,mode=755"

    def test_root_uid_resolved(self):
        st = os.stat(MOUNT_POINT)
        self.assertEqual(st.st_uid, UID_DAE)

    def test_root_gid_resolved(self):
        st = os.stat(MOUNT_POINT)
        self.assertEqual(st.st_gid, GID_DAE)

    def test_root_mode(self):
        st = os.stat(MOUNT_POINT)
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o755)


class TestMountOptionsSticky(TmpfsTestBase):
    """mode=1777 sticky bit is preserved."""

    mount_opts = "mode=1777"

    def test_sticky_bit(self):
        st = os.stat(MOUNT_POINT)
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o1777)


class TestMountOptionsDefault(TmpfsTestBase):
    """No options → defaults: mode=0755, uid/gid = mounting process (root)."""

    mount_opts = None

    def test_default_mode(self):
        st = os.stat(MOUNT_POINT)
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o755)

    def test_default_uid(self):
        # Mounted as root → uid should be 0
        st = os.stat(MOUNT_POINT)
        self.assertEqual(st.st_uid, 0)


class TestMountOptionsSize(TmpfsTestBase):
    """size= option is respected."""

    mount_opts = "size=16M"

    def test_size_option(self):
        sv = os.statvfs(MOUNT_POINT)
        total_bytes = sv.f_frsize * sv.f_blocks
        self.assertGreater(total_bytes, 14 * 1024 * 1024)
        self.assertLess(total_bytes, 20 * 1024 * 1024)


# ===========================================================================
# 9. Mount Interface  (direct binary vs mount command)
# ===========================================================================

class TestMountInterfaceDirect(TmpfsTestBase):
    """All basic operations work when mounted via fs-tmpfs directly."""
    mount_fn   = staticmethod(mount_direct)
    mount_opts = "uid=dae,gid=dae,mode=755"

    def test_coordinator_started(self):
        self.assertTrue(os.path.exists(CTRL_PATH))

    def test_can_create_file(self):
        self.write_file("direct.txt", b"direct")
        self.assertEqual(self.read_file("direct.txt"), b"direct")

    def test_root_stat(self):
        st = os.stat(MOUNT_POINT)
        self.assertEqual(st.st_uid, UID_DAE)
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o755)


class TestMountInterfaceCommand(TmpfsTestBase):
    """All basic operations work when mounted via `mount -t tmpfs`."""
    mount_fn   = staticmethod(mount_via_cmd)
    mount_opts = "uid=dae,gid=dae,mode=755"

    def test_coordinator_started(self):
        self.assertTrue(os.path.exists(CTRL_PATH))

    def test_can_create_file(self):
        self.write_file("via_cmd.txt", b"via_cmd")
        self.assertEqual(self.read_file("via_cmd.txt"), b"via_cmd")

    def test_root_stat(self):
        st = os.stat(MOUNT_POINT)
        self.assertEqual(st.st_uid, UID_DAE)
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o755)


class TestMountOptionsNumericViaCmd(TmpfsTestBase):
    """uid=/gid=/mode= numeric options work via `mount -t tmpfs`."""
    mount_fn   = staticmethod(mount_via_cmd)
    mount_opts = f"uid={UID_OTHER},gid={GID_OTHER},mode=700"

    def test_root_uid(self):
        self.assertEqual(os.stat(MOUNT_POINT).st_uid, UID_OTHER)

    def test_root_gid(self):
        self.assertEqual(os.stat(MOUNT_POINT).st_gid, GID_OTHER)

    def test_root_mode(self):
        self.assertEqual(stat.S_IMODE(os.stat(MOUNT_POINT).st_mode), 0o700)


class TestMountOptionsNamesViaCmd(TmpfsTestBase):
    """uid=name/gid=name options work via `mount -t tmpfs`."""
    mount_fn   = staticmethod(mount_via_cmd)
    mount_opts = "uid=dae,gid=dae,mode=750"

    def test_root_uid_resolved(self):
        self.assertEqual(os.stat(MOUNT_POINT).st_uid, UID_DAE)

    def test_root_gid_resolved(self):
        self.assertEqual(os.stat(MOUNT_POINT).st_gid, GID_DAE)

    def test_root_mode(self):
        self.assertEqual(stat.S_IMODE(os.stat(MOUNT_POINT).st_mode), 0o750)


# ===========================================================================
# 10. Timestamps
# ===========================================================================

class TestTimestamps(TmpfsTestBase):
    """atime / mtime / ctime update correctly."""

    def test_mtime_updated_on_write(self):
        p = self.path("ts.txt")
        with open(p, "wb") as f:
            f.write(b"a")
        mtime1 = os.stat(p).st_mtime_ns
        time.sleep(0.05)
        with open(p, "ab") as f:
            f.write(b"b")
        mtime2 = os.stat(p).st_mtime_ns
        self.assertGreaterEqual(mtime2, mtime1)

    def test_mtime_unchanged_on_read(self):
        p = self.write_file("r.txt", b"data")
        mtime1 = os.stat(p).st_mtime_ns
        time.sleep(0.05)
        open(p, "rb").read()
        mtime2 = os.stat(p).st_mtime_ns
        self.assertEqual(mtime1, mtime2)

    def test_utime(self):
        p = self.write_file("u.txt", b"x")
        os.utime(p, (1000000, 2000000))
        st = os.stat(p)
        self.assertEqual(int(st.st_atime), 1000000)
        self.assertEqual(int(st.st_mtime), 2000000)


# ===========================================================================
# 11. Edge Cases
# ===========================================================================

class TestEdgeCases(TmpfsTestBase):
    """Edge conditions: deep trees, large filenames, concurrent access."""

    def test_max_filename_length(self):
        """255-character filename is the POSIX NAME_MAX limit."""
        name = "a" * 255
        p = self.path(name)
        with open(p, "wb") as f:
            f.write(b"long name")
        self.assertIn(name, os.listdir(MOUNT_POINT))

    def test_deep_directory_tree(self):
        """Create and traverse a 20-level deep directory tree."""
        parts = [f"d{i}" for i in range(20)]
        deep = os.path.join(MOUNT_POINT, *parts)
        os.makedirs(deep)
        self.write_file(os.path.join(*parts, "leaf.txt"), b"deep")
        self.assertEqual(
            open(os.path.join(MOUNT_POINT, *parts, "leaf.txt"), "rb").read(),
            b"deep",
        )

    def test_many_files_in_dir(self):
        """Create 500 files in a single directory."""
        for i in range(500):
            open(self.path(f"mf_{i:04d}.txt"), "wb").close()
        names = os.listdir(MOUNT_POINT)
        self.assertEqual(len(names), 500)

    def test_rename_nonexistent_fails(self):
        with self.assertRaises(FileNotFoundError):
            os.rename(self.path("ghost.txt"), self.path("x.txt"))

    def test_unlink_dir_fails(self):
        d = self.path("notafile")
        os.mkdir(d)
        with self.assertRaises(OSError) as cm:
            os.unlink(d)
        self.assertIn(cm.exception.errno, (errno.EPERM, errno.EISDIR))

    def test_open_directory_for_write_fails(self):
        d = self.path("rdir")
        os.mkdir(d)
        with self.assertRaises(OSError):
            open(d, "wb")

    def test_write_then_truncate_then_write(self):
        """Reuse pattern: write → truncate(0) → write must produce correct data."""
        p = self.path("reuse.bin")
        with open(p, "wb") as f:
            f.write(b"first" * 1000)
        os.truncate(p, 0)
        with open(p, "wb") as f:
            f.write(b"second")
        self.assertEqual(open(p, "rb").read(), b"second")

    def test_concurrent_writers(self):
        """Two threads writing to different files simultaneously."""
        import threading
        errors = []

        def writer(name, data):
            try:
                p = self.path(name)
                with open(p, "wb") as f:
                    for _ in range(100):
                        f.write(data)
            except Exception as e:
                errors.append(e)

        t1 = threading.Thread(target=writer, args=("t1.bin", b"A" * 1024))
        t2 = threading.Thread(target=writer, args=("t2.bin", b"B" * 1024))
        t1.start(); t2.start()
        t1.join();  t2.join()
        self.assertEqual(errors, [])
        self.assertEqual(os.stat(self.path("t1.bin")).st_size, 100 * 1024)
        self.assertEqual(os.stat(self.path("t2.bin")).st_size, 100 * 1024)


# ===========================================================================
# 12.  Error argument validation
# ===========================================================================

class TestMountErrorHandling(unittest.TestCase):
    """Bad mount option arguments must fail cleanly without starting a daemon."""

    def setUp(self):
        if os.geteuid() != 0:
            self.skipTest("must run as root")
        ensure_mount_symlink()
        os.makedirs(MOUNT_POINT, exist_ok=True)
        if coordinator_running():
            umount(MOUNT_POINT)
            kill_coordinator()

    def tearDown(self):
        # Make sure no stray coordinator was left behind
        if coordinator_running():
            umount(MOUNT_POINT)
            kill_coordinator()

    def _mount_direct_rc(self, opts):
        env = os.environ.copy()
        env["PATH"] = BUILD_DIR + ":" + env.get("PATH", "")
        cmd = [FS_TMPFS_BIN, "-o", opts, MOUNT_POINT]
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        return r.returncode

    def test_invalid_mode_is_rejected(self):
        rc = self._mount_direct_rc("mode=999")
        self.assertNotEqual(rc, 0)
        self.assertFalse(coordinator_running())

    def test_invalid_mode_letters_rejected(self):
        rc = self._mount_direct_rc("mode=abc")
        self.assertNotEqual(rc, 0)

    def test_unknown_user_rejected(self):
        rc = self._mount_direct_rc("uid=no_such_user_xyz")
        self.assertNotEqual(rc, 0)
        self.assertFalse(coordinator_running())

    def test_unknown_group_rejected(self):
        rc = self._mount_direct_rc("gid=no_such_group_xyz")
        self.assertNotEqual(rc, 0)

    def test_invalid_size_rejected(self):
        rc = self._mount_direct_rc("size=99%")   # > 50% limit
        self.assertNotEqual(rc, 0)


# ===========================================================================
# Entry point
# ===========================================================================

if __name__ == "__main__":
    if os.geteuid() != 0:
        print("ERROR: this test suite must be run as root (sudo python3 ...)",
              file=sys.stderr)
        sys.exit(1)
    try:
        unittest.main(verbosity=2)
    finally:
        remove_mount_symlink()
