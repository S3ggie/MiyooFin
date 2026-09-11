/* Fixed-purpose Onion MiyooFin graceful-exit bridge.
 * Installed setuid-root by the root-owned startup helper. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define EXPECTED_ONION_UID 1000
#define MIYOOFIN_PATH "/mnt/SDCARD/App/MiyooFin/miyoofin"

static int fail(const char *message)
{
    fprintf(stderr, "miyoofin-graceful-exit: %s\n", message);
    return 1;
}

static int is_miyoofin(const char *name)
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
    if (strcmp(comm, "miyoofin") != 0) return 0;

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
        return strcmp(exe, MIYOOFIN_PATH) == 0;
    }

    snprintf(path, sizeof(path), "/proc/%s/cmdline", name);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char cmdline[64] = {0};
    n = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    return (n == 9 && memcmp(cmdline, "./miyoofin\0", 9) == 0) ||
           (n == 8 && memcmp(cmdline, "./miyoofin", 8) == 0);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return fail("arguments are not accepted");
    if (getuid() != EXPECTED_ONION_UID) return fail("caller is not onion");
    if (geteuid() != 0) return fail("helper is not setuid-root");
    unsetenv("PATH");

    DIR *dir = opendir("/proc");
    if (!dir) return fail("cannot inspect /proc");
    pid_t found = -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!*name) continue;
        int numeric = 1;
        for (const char *p = name; *p; ++p)
            if (*p < '0' || *p > '9') numeric = 0;
        if (!numeric || !is_miyoofin(name)) continue;
        if (found != -1) {
            closedir(dir);
            return fail("multiple valid MiyooFin processes exist");
        }
        found = (pid_t)strtol(name, NULL, 10);
    }
    closedir(dir);
    if (found == -1) return fail("no valid MiyooFin process exists");
    if (kill(found, SIGUSR1) != 0) return fail("validated SIGUSR1 failed");
    return 0;
}
