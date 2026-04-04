/*
 * tmpfs-stat.c  --  CLI tool to query fs-tmpfs statistics.
 *
 * Usage:
 *   tmpfs-stat              -- show global stats
 *   tmpfs-stat <mountpoint> -- show stats for a specific mount
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <devctl.h>
#include <limits.h>

#include "../include/tmpfs_ipc.h"

#define CTRL_PATH   "/dev/fs-tmpfs"

static void fmt_bytes(uint64_t b, char *buf, size_t bufsz)
{
    if (b >= 1024ULL * 1024 * 1024)
        snprintf(buf, bufsz, "%.2f GiB", (double)b / (1024.0*1024*1024));
    else if (b >= 1024ULL * 1024)
        snprintf(buf, bufsz, "%.2f MiB", (double)b / (1024.0*1024));
    else if (b >= 1024ULL)
        snprintf(buf, bufsz, "%.2f KiB", (double)b / 1024.0);
    else
        snprintf(buf, bufsz, "%llu B", (unsigned long long)b);
}

static void print_global(int fd)
{
    tmpfs_global_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    int rc = devctl(fd, DCMD_TMPFS_GET_STATS, &stats, sizeof(stats), NULL);
    if (rc != 0) {
        fprintf(stderr, "tmpfs-stat: DCMD_TMPFS_GET_STATS failed: %s\n",
                strerror(rc));
        exit(1);
    }

    char used_str[32], cap_str[32], ram_str[32];
    fmt_bytes(stats.global_used, used_str, sizeof(used_str));
    fmt_bytes(stats.global_cap,  cap_str,  sizeof(cap_str));
    fmt_bytes(stats.total_ram,   ram_str,  sizeof(ram_str));

    uint64_t uptime_s  = stats.uptime_ms / 1000;
    uint64_t uptime_ms = stats.uptime_ms % 1000;

    printf("fs-tmpfs  v%u.%u.%u\n",
           stats.version_major, stats.version_minor, stats.version_patch);
    printf("  Total RAM    : %s\n", ram_str);
    printf("  Global cap   : %s  (50%% of total)\n", cap_str);
    printf("  Global used  : %s  (%.1f%%)\n",
           used_str,
           stats.global_cap ? 100.0 * (double)stats.global_used /
                               (double)stats.global_cap : 0.0);
    printf("  Active mounts: %u\n", stats.mount_count);
    printf("  Uptime       : %llu.%03llu s\n",
           (unsigned long long)uptime_s,
           (unsigned long long)uptime_ms);
}

static void print_mount(int fd, const char *path)
{
    tmpfs_mount_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    strncpy(stats.path, path, PATH_MAX - 1);

    int rc = devctl(fd, DCMD_TMPFS_GET_MOUNT, &stats, sizeof(stats), NULL);
    if (rc != 0) {
        fprintf(stderr, "tmpfs-stat: DCMD_TMPFS_GET_MOUNT(%s) failed: %s\n",
                path, strerror(rc));
        exit(1);
    }

    char used_str[32], cap_str[32];
    fmt_bytes(stats.mount_used, used_str, sizeof(used_str));
    fmt_bytes(stats.mount_cap,  cap_str,  sizeof(cap_str));

    printf("Mount: %s\n", stats.path);
    printf("  Cap          : %s\n", cap_str);
    printf("  Used         : %s  (%.1f%%)\n",
           used_str,
           stats.mount_cap ? 100.0 * (double)stats.mount_used /
                             (double)stats.mount_cap : 0.0);
    printf("  Files        : %llu\n", (unsigned long long)stats.file_count);
    printf("  Directories  : %llu\n", (unsigned long long)stats.dir_count);
    printf("  Symlinks     : %llu\n", (unsigned long long)stats.symlink_count);
    printf("  Total inodes : %llu\n", (unsigned long long)stats.inode_count);
}

int main(int argc, char *argv[])
{
    int fd = open(CTRL_PATH, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "tmpfs-stat: cannot open %s: %s\n"
                        "  (is fs-tmpfs running?)\n",
                CTRL_PATH, strerror(errno));
        return 1;
    }

    if (argc < 2) {
        print_global(fd);
    } else {
        for (int i = 1; i < argc; i++)
            print_mount(fd, argv[i]);
    }

    close(fd);
    return 0;
}
