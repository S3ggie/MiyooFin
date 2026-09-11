/* Fixed-purpose Onion reboot bridge.
 * Installed setuid-root by the root-owned startup helper. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define EXPECTED_ONION_UID 1000
#define QUEUE_PATH "/tmp/cmd_to_run.sh"
#define ONION_QUEUE "/mnt/SDCARD/.tmp_update/cmd_to_run.sh"
#define HANDOFF_LOCK "/tmp/miyoofin-onion-remote-launch.lock"
#define HANDOFF_LEASE_PREFIX ".miyoofin-onion-remote-lease."
#define MAINUI_PATH "/mnt/SDCARD/miyoo/app/MainUI"

static int fail(const char *message)
{
    fprintf(stderr, "miyoofin-reboot: %s\n", message);
    return 1;
}

static int path_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int process_mainui(const char *name)
{
    char path[PATH_MAX];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/%s/comm", name);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char comm[32] = {0};
    n = read(fd, comm, sizeof(comm) - 1);
    close(fd);
    if (n <= 0) return 0;
    comm[strcspn(comm, "\n")] = '\0';
    if (strcmp(comm, "MainUI") != 0) return 0;

    snprintf(path, sizeof(path), "/proc/%s/status", name);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char status[4096] = {0};
    n = read(fd, status, sizeof(status) - 1);
    close(fd);
    if (n <= 0) return 0;
    char *uid_line = strstr(status, "\nUid:");
    if (uid_line) ++uid_line;
    else uid_line = strstr(status, "Uid:");
    unsigned long uid = 1;
    if (!uid_line || sscanf(uid_line, "Uid:\t%lu", &uid) != 1 || uid != 0)
        return 0;

    snprintf(path, sizeof(path), "/proc/%s/exe", name);
    char exe[PATH_MAX];
    n = readlink(path, exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        return strcmp(exe, MAINUI_PATH) == 0;
    }

    snprintf(path, sizeof(path), "/proc/%s/cmdline", name);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char cmdline[32] = {0};
    n = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    return (n == 8 && memcmp(cmdline, "./MainUI", 8) == 0) ||
           (n == 9 && memcmp(cmdline, "./MainUI\0", 9) == 0);
}

static int require_one_mainui(void)
{
    DIR *dir = opendir("/proc");
    if (!dir) return fail("cannot inspect /proc");
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!*name) continue;
        int numeric = 1;
        for (const char *p = name; *p; ++p)
            if (*p < '0' || *p > '9') numeric = 0;
        if (numeric && process_mainui(name)) ++count;
    }
    closedir(dir);
    if (count != 1) return fail("expected exactly one validated MainUI process");
    return 0;
}

static int any_comm(const char *wanted)
{
    DIR *dir = opendir("/proc");
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!*name) continue;
        int numeric = 1;
        for (const char *p = name; *p; ++p)
            if (*p < '0' || *p > '9') numeric = 0;
        if (!numeric) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/proc/%s/comm", name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char comm[32] = {0};
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (n > 0) {
            comm[strcspn(comm, "\n")] = '\0';
            if (strcmp(comm, wanted) == 0) {
                closedir(dir);
                return 1;
            }
        }
    }
    closedir(dir);
    return 0;
}

static int handoff_in_progress(void)
{
    if (access(HANDOFF_LOCK, F_OK) == 0) return 1;
    DIR *dir = opendir("/tmp");
    if (!dir) return 1;
    struct dirent *entry;
    size_t prefix_len = strlen(HANDOFF_LEASE_PREFIX);
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, HANDOFF_LEASE_PREFIX, prefix_len) == 0) {
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return fail("arguments are not accepted");
    if (getuid() != EXPECTED_ONION_UID) return fail("caller is not onion");
    if (geteuid() != 0) return fail("helper is not setuid-root");
    unsetenv("PATH");
    if (any_comm("miyoofin")) return fail("MiyooFin is still running");
    if (require_one_mainui() != 0) return 1;
    if (path_exists(QUEUE_PATH) || path_exists(ONION_QUEUE))
        return fail("an Onion launch queue is staged");
    if (handoff_in_progress()) return fail("a developer handoff is in progress");
    sync();
    if (reboot(RB_AUTOBOOT) != 0) return fail("normal reboot request failed");
    return 0;
}
