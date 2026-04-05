/*
 * main.c  --  Entry point, coordinator bootstrap, RAM cap calculation.
 *
 * Behaviour:
 *   1. Parse argv for mount path and -o size= option.
 *      When invoked as "mount_tmpfs" (via `mount -t tmpfs`), the argument
 *      format is: mount_tmpfs -o <opts> <special> <mountpoint>
 *      Standard mount options (rw, ro, noexec, etc.) are silently ignored;
 *      only tmpfs-specific options (size=) are acted upon.
 *   2. Try to connect to an existing coordinator at TMPFS_CTRL_PATH.
 *      - If found: send DCMD_TMPFS_ADD_MOUNT and exit.
 *      - If not:   become the coordinator (daemonise, init, attach).
 *   3. After all mounts are removed, drain briefly then exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/memmsg.h>
#include <sys/procmgr.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <pthread.h>
#include <devctl.h>

#include "../include/tmpfs_internal.h"
#include "../include/tmpfs_ipc.h"
#include "memory.h"
#include "mount.h"
#include "control.h"
#include "resmgr.h"

/* -------------------------------------------------------------------------
 * Global state instance (declared extern in tmpfs_internal.h)
 * ---------------------------------------------------------------------- */
tmpfs_global_t g_tmpfs;

/* -------------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-o size=<N[B|K|M|G|%%]>] <mountpoint>\n"
        "\n"
        "  -o size=256M    mount with 256 MiB limit\n"
        "  -o size=10%%     mount with 10%% of total RAM\n"
        "\n"
        "  If size= is omitted, defaults to 25%% of total RAM.\n"
        "  Global cap across all mounts is 50%% of total RAM.\n"
        "\n"
        "  Can also be invoked as 'mount_tmpfs' by the system mount command:\n"
        "  mount -t tmpfs [-o size=N] none <mountpoint>\n",
        prog);
}

/* -------------------------------------------------------------------------
 * Parse -o option string into a tmpfs_mount_req_t.
 * Supports: size=<value>
 * Standard mount options (rw, ro, noexec, nosuid, noatime, etc.) are
 * silently accepted so the system mount command can pass them through.
 * ---------------------------------------------------------------------- */
static int parse_options(const char *opts, uint64_t total_ram,
                          tmpfs_mount_req_t *req)
{
    /* Standard mount options we silently accept and ignore */
    static const char * const std_opts[] = {
        "rw", "ro", "exec", "noexec", "suid", "nosuid",
        "atime", "noatime", "dev", "nodev", "remount",
        "before", "after", "opaque", "nostat", "implied",
        NULL
    };

    if (opts == NULL)
        return EOK;

    char buf[256];
    strncpy(buf, opts, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = strtok(buf, ",");
    while (tok != NULL) {
        if (strncmp(tok, "size=", 5) == 0) {
            uint64_t sz = 0;
            int rc = tmpfs_parse_size(tok + 5, total_ram, &sz);
            if (rc != EOK) {
                fprintf(stderr, "fs-tmpfs: invalid size option '%s'\n", tok);
                return rc;
            }
            req->size_opt.bytes = sz;
        } else {
            /* Check against known standard options before warning */
            int known = 0;
            for (int i = 0; std_opts[i] != NULL; i++) {
                if (strcmp(tok, std_opts[i]) == 0) {
                    known = 1;
                    break;
                }
            }
            if (!known)
                fprintf(stderr, "fs-tmpfs: unknown option '%s' (ignored)\n", tok);
        }
        tok = strtok(NULL, ",");
    }
    return EOK;
}

/* -------------------------------------------------------------------------
 * Get total physical RAM via the QNX memory manager (_MEM_INFO message).
 * Falls back to 256 MB if the query fails.
 * ---------------------------------------------------------------------- */
static uint64_t get_total_ram(void)
{
    mem_info_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.i.type  = _MEM_INFO;
    msg.i.zero  = 0;
    msg.i.flags = 0;
    msg.i.fd    = NOFD;

    if (MsgSend(MEMMGR_COID, &msg.i, sizeof(msg.i),
                             &msg.o, sizeof(msg.o)) != 0) {
        fprintf(stderr,
                "fs-tmpfs: warning: could not query RAM size (%s), "
                "defaulting to 256 MB\n", strerror(errno));
        return 256ULL * 1024 * 1024;
    }

    return (uint64_t)msg.o.info.__posix_tmi_total;
}

/* -------------------------------------------------------------------------
 * Try to delegate to an existing coordinator via devctl.
 * Returns:
 *   0   -- successfully delegated (caller should exit)
 *   -1  -- no coordinator found (caller should become coordinator)
 *   >0  -- coordinator found but returned an error (errno value)
 * ---------------------------------------------------------------------- */
static int try_delegate(const tmpfs_mount_req_t *req)
{
    int fd = open(TMPFS_CTRL_PATH, O_RDWR);
    if (fd == -1)
        return -1;  /* no coordinator */

    int rc = devctl(fd, DCMD_TMPFS_ADD_MOUNT, (void *)req,
                    sizeof(*req), NULL);
    close(fd);

    if (rc != EOK) {
        fprintf(stderr, "fs-tmpfs: coordinator rejected mount: %s\n",
                strerror(rc));
        return rc;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Drain thread: watches mount_count; exits the process after the grace
 * period once the last mount is removed.
 * ---------------------------------------------------------------------- */
static void *drain_thread(void *arg)
{
    (void)arg;
    for (;;) {
        /* Poll every 50 ms */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);

        pthread_rwlock_rdlock(&g_tmpfs.mounts_lock);
        uint32_t count = g_tmpfs.mount_count;
        pthread_rwlock_unlock(&g_tmpfs.mounts_lock);

        if (count == 0) {
            /* Grace period */
            struct timespec grace = {
                .tv_sec  =  TMPFS_DRAIN_GRACE_MS / 1000,
                .tv_nsec = (TMPFS_DRAIN_GRACE_MS % 1000) * 1000 * 1000
            };
            nanosleep(&grace, NULL);

            /* Check again after grace period */
            pthread_rwlock_rdlock(&g_tmpfs.mounts_lock);
            count = g_tmpfs.mount_count;
            pthread_rwlock_unlock(&g_tmpfs.mounts_lock);

            if (count == 0) {
                tmpfs_control_fini();
                exit(0);
            }
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    const char *mount_path = NULL;
    const char *opt_string = NULL;

    /*
     * Detect if we were invoked as "mount_tmpfs" by the system mount command.
     *
     * The mount(8) utility spawns us as:
     *   mount_tmpfs -o <options> <special> <mountpoint>
     *
     * where <special> is "none" (or a device hint we ignore for tmpfs) and
     * <mountpoint> is the target path. The -o string contains standard mount
     * options (rw, noexec, ...) plus any tmpfs-specific options (size=N).
     *
     * In this mode we skip the -D and -h flags and treat the last argument
     * as the mountpoint and second-to-last as the special device.
     */
    const char *progname = strrchr(argv[0], '/');
    progname = progname ? progname + 1 : argv[0];
    int mount_cmd_mode = (strcmp(progname, "mount_tmpfs") == 0);

    /* ---- Argument parsing ---- */
    int opt;
    int no_daemon = 0;

    if (mount_cmd_mode) {
        /*
         * mount_tmpfs -o <opts> <special> <mountpoint>
         * getopt with just "o:" -- we only care about -o.
         * The last argument is mountpoint, second-to-last is special (ignored).
         */
        while ((opt = getopt(argc, argv, "o:")) != -1) {
            if (opt == 'o')
                opt_string = optarg;
            /* ignore unknown options - mount may pass flags we don't know */
        }

        /* Need at least special + mountpoint after options */
        if (optind + 2 > argc) {
            fprintf(stderr, "mount_tmpfs: usage: mount_tmpfs -o opts special mountpoint\n");
            return 1;
        }

        /* mountpoint is always the last argument; special is ignored */
        mount_path = argv[argc - 1];

    } else {
        /* Normal fs-tmpfs invocation */
        while ((opt = getopt(argc, argv, "o:Dh")) != -1) {
            switch (opt) {
            case 'o':
                opt_string = optarg;
                break;
            case 'D':
                no_daemon = 1;
                break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
            }
        }

        if (optind >= argc) {
            usage(argv[0]);
            return 1;
        }
        mount_path = argv[optind];
    }

    /* ---- Determine total RAM and global cap ---- */
    uint64_t total_ram = get_total_ram();

    /* ---- Build mount request ---- */
    tmpfs_mount_req_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.path, mount_path, PATH_MAX - 1);
    req.uid  = getuid();
    req.gid  = getgid();
    req.mode = 0755;

    int rc = parse_options(opt_string, total_ram, &req);
    if (rc != EOK)
        return 1;

    /* ---- Try to delegate to an existing coordinator ---- */
    rc = try_delegate(&req);
    if (rc == 0) {
        /* Successfully handed off to existing coordinator */
        return 0;
    }
    if (rc > 0) {
        /* Coordinator rejected us */
        return 1;
    }
    /* rc == -1: no coordinator, we become it */

    /* ---- Initialise global state ---- */
    memset(&g_tmpfs, 0, sizeof(g_tmpfs));
    g_tmpfs.total_ram   = total_ram;
    g_tmpfs.global_cap  = total_ram / TMPFS_GLOBAL_RAM_FRACTION;
    atomic_init(&g_tmpfs.global_used, 0);
    g_tmpfs.mount_count = 0;
    g_tmpfs.mounts      = NULL;
    g_tmpfs.ctrl_resmgr_id = -1;
    pthread_rwlock_init(&g_tmpfs.mounts_lock, NULL);
    clock_gettime(CLOCK_MONOTONIC, &g_tmpfs.start_time);

    fprintf(stderr,
            "fs-tmpfs: coordinator starting. "
            "total_ram=%llu MB, global_cap=%llu MB\n",
            (unsigned long long)(total_ram  / (1024 * 1024)),
            (unsigned long long)(g_tmpfs.global_cap / (1024 * 1024)));

    /* ---- Request required abilities (needed for resmgr_attach) ---- */
    if (procmgr_ability(0,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PATHSPACE,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PUBLIC_CHANNEL,
            PROCMGR_AID_EOL) != EOK) {
        /* Non-fatal: may already have abilities (running as root) */
    }

    /* ---- Daemonise ---- */
    if (!no_daemon) {
        if (procmgr_daemon(0, PROCMGR_DAEMON_NOCHDIR) == -1) {
            perror("fs-tmpfs: procmgr_daemon");
            return 1;
        }
    }

    /* ---- Init resmgr dispatch and thread pool ---- */
    rc = tmpfs_resmgr_init();
    if (rc != EOK) {
        fprintf(stderr, "fs-tmpfs: resmgr init failed: %s\n", strerror(rc));
        return 1;
    }

    /* ---- Register /dev/fs-tmpfs control channel ---- */
    rc = tmpfs_control_init();
    if (rc != EOK) {
        fprintf(stderr, "fs-tmpfs: control init failed: %s\n", strerror(rc));
        return 1;
    }

    /* ---- Attach the first mount ---- */
    rc = tmpfs_mount_add(&req);
    if (rc != EOK) {
        fprintf(stderr, "fs-tmpfs: mount '%s' failed: %s\n",
                mount_path, strerror(rc));
        tmpfs_control_fini();
        return 1;
    }

    /* ---- Start drain watcher thread ---- */
    pthread_t drain_tid;
    pthread_attr_t tattr;
    pthread_attr_init(&tattr);
    pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
    pthread_create(&drain_tid, &tattr, drain_thread, NULL);
    pthread_attr_destroy(&tattr);

    /* ---- Enter dispatch loop (blocks here) ---- */
    rc = tmpfs_resmgr_start();

    /* If we get here the pool was destroyed */
    tmpfs_control_fini();
    pthread_rwlock_destroy(&g_tmpfs.mounts_lock);
    return rc == EOK ? 0 : 1;
}
