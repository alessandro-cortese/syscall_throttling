#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "../kernel/scth_ioctl.h"

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int dev_open(void) {
    int fd = open("/dev/scth", O_RDWR);
    if (fd < 0) die("open /dev/scth");
    return fd;
}

static void cmd_addprog(int fd, const char *name) {
    struct scth_prog_req r;
    memset(&r, 0, sizeof(r));
    strncpy(r.name, name, SCTH_MAX_PROG_LEN-1);
    if (ioctl(fd, SCTH_IOC_ADD_PROG, &r) < 0) die("ioctl ADD_PROG");
}

static void cmd_delprog(int fd, const char *name) {
    struct scth_prog_req r;
    memset(&r, 0, sizeof(r));
    strncpy(r.name, name, SCTH_MAX_PROG_LEN-1);
    if (ioctl(fd, SCTH_IOC_DEL_PROG, &r) < 0) die("ioctl DEL_PROG");
}

static void cmd_adduid(int fd, uint32_t uid) {
    struct scth_uid_req r = { .euid = uid };
    if (ioctl(fd, SCTH_IOC_ADD_UID, &r) < 0) die("ioctl ADD_UID");
}

static void cmd_deluid(int fd, uint32_t uid) {
    struct scth_uid_req r = { .euid = uid };
    if (ioctl(fd, SCTH_IOC_DEL_UID, &r) < 0) die("ioctl DEL_UID");
}

static void cmd_addsys(int fd, uint32_t nr) {
    struct scth_sys_req r = { .nr = nr };
    if (ioctl(fd, SCTH_IOC_ADD_SYSCALL, &r) < 0) die("ioctl ADD_SYSCALL");
}

static void cmd_delsys(int fd, uint32_t nr) {
    struct scth_sys_req r = { .nr = nr };
    if (ioctl(fd, SCTH_IOC_DEL_SYSCALL, &r) < 0) die("ioctl DEL_SYSCALL");
}

static void cmd_setmax(int fd, uint32_t max) {
    struct scth_max_req r = { .max_per_sec = max };
    if (ioctl(fd, SCTH_IOC_SET_MAX, &r) < 0) die("ioctl SET_MAX");
}

static void cmd_on(int fd) {
    if (ioctl(fd, SCTH_IOC_MON_ON) < 0) die("ioctl MON_ON");
}

static void cmd_off(int fd) {
    if (ioctl(fd, SCTH_IOC_MON_OFF) < 0) die("ioctl MON_OFF");
}

static void cmd_listprog(int fd) {
    char buf[SCTH_MAX_LIST][SCTH_MAX_PROG_LEN];
    struct scth_list_resp lr;

    memset(&buf, 0, sizeof(buf));
    lr.count = SCTH_MAX_LIST;
    lr.ptr = (uint64_t)(uintptr_t)buf;

    if (ioctl(fd, SCTH_IOC_LIST_PROG, &lr) < 0) die("ioctl LIST_PROG");

    printf("Programs (%u):\n", lr.count);
    for (uint32_t i = 0; i < lr.count; i++)
        printf("  %s\n", buf[i]);
}

static void cmd_listuid(int fd) {
    uint32_t buf[SCTH_MAX_LIST];
    struct scth_list_resp lr;

    memset(&buf, 0, sizeof(buf));
    lr.count = SCTH_MAX_LIST;
    lr.ptr = (uint64_t)(uintptr_t)buf;

    if (ioctl(fd, SCTH_IOC_LIST_UID, &lr) < 0) die("ioctl LIST_UID");

    printf("UIDs (%u):\n", lr.count);
    for (uint32_t i = 0; i < lr.count; i++)
        printf("  %u\n", buf[i]);
}

static void cmd_listsys(int fd) {
    uint32_t buf[SCTH_MAX_LIST];
    struct scth_list_resp lr;

    memset(&buf, 0, sizeof(buf));
    lr.count = SCTH_MAX_LIST;
    lr.ptr = (uint64_t)(uintptr_t)buf;

    if (ioctl(fd, SCTH_IOC_LIST_SYSCALL, &lr) < 0) die("ioctl LIST_SYSCALL");

    printf("Syscalls (%u):\n", lr.count);
    for (uint32_t i = 0; i < lr.count; i++)
        printf("  %u\n", buf[i]);
}

static void cmd_stats(int fd) {
    struct scth_stats st;
    if (ioctl(fd, SCTH_IOC_GET_STATS, &st) < 0) die("ioctl GET_STATS");

    printf("monitor_on=%u  max_current_per_sec=%u  max_next_per_sec=%u\n", st.monitor_on, st.max_current_per_sec, st.max_next_per_sec);
    printf("peak_delay_ns=%llu  peak_prog=%s  peak_uid=%u\n", (unsigned long long)st.peak_delay_ns, st.peak_prog, st.peak_euid);
    printf("peak_blocked_threads=%u  avg_blocked_threads=%.3f\n", st.peak_blocked_threads, st.avg_blocked_threads_x1000 / 1000.0);
}

static void cmd_resetstats(int fd) {
    if (ioctl(fd, SCTH_IOC_RESET_STATS) < 0) die("ioctl RESET_STATS");
}

static void cmd_wakewaiters(int fd) {
    if (ioctl(fd, SCTH_IOC_WAKE_WAITERS) < 0) die("ioctl WAKE_WAITERS");
}

static void usage(const char *p) {
    fprintf(stderr,
        "Usage:\n"
        "  %s addprog <comm>\n"
        "  %s delprog <comm>\n"
        "  %s adduid <euid>\n"
        "  %s deluid <euid>\n"
        "  %s addsys <nr>\n"
        "  %s delsys <nr>\n"
        "  %s setmax <max_per_sec>\n"
        "  %s on | off\n"
        "  %s listprog | listuid | listsys\n"
        "  %s stats\n"
        "  %s resetstats\n"
        "  %s wakewaiters\n"
        "  %s print\n",
        p, p, p, p, p, p, p, p, p, p, p, p, p);
    exit(2);
}

void print_title(void) {
    puts("");
    puts("");
    puts("");
    printf(
        "  ░██████   ░██     ░██   ░██████     ░██████     ░███    ░██         ░██            ░██████████░█████████  ░██     ░██   ░██████   ░██████████░██████████░██         ░██████░███    ░██   ░██████  \n"
        " ░██   ░██   ░██   ░██   ░██   ░██   ░██   ░██   ░██░██   ░██         ░██                ░██    ░██     ░██ ░██     ░██  ░██   ░██      ░██        ░██    ░██           ░██  ░████   ░██  ░██   ░██ \n"
        "░██           ░██ ░██   ░██         ░██         ░██  ░██  ░██         ░██                ░██    ░██     ░██ ░██     ░██ ░██     ░██     ░██        ░██    ░██           ░██  ░██░██  ░██ ░██        \n"
        " ░████████     ░████     ░████████  ░██        ░█████████ ░██         ░██                ░██    ░█████████  ░██████████ ░██     ░██     ░██        ░██    ░██           ░██  ░██ ░██ ░██ ░██  █████ \n"
        "        ░██     ░██             ░██ ░██        ░██    ░██ ░██         ░██                ░██    ░██   ░██   ░██     ░██ ░██     ░██     ░██        ░██    ░██           ░██  ░██  ░██░██ ░██     ██ \n"
        " ░██   ░██      ░██      ░██   ░██   ░██   ░██ ░██    ░██ ░██         ░██                ░██    ░██    ░██  ░██     ░██  ░██   ░██      ░██        ░██    ░██           ░██  ░██   ░████  ░██  ░███ \n"
        "  ░██████       ░██       ░██████     ░██████  ░██    ░██ ░██████████ ░██████████        ░██    ░██     ░██ ░██     ░██   ░██████       ░██        ░██    ░██████████ ░██████░██    ░███   ░█████░█ \n"
    );
    puts("");
    puts("");
    puts("");
}

int main(int argc, char **argv) {
    int fd;

    if (argc < 2) usage(argv[0]);

    fd = dev_open();

    if      (!strcmp(argv[1], "addprog")        && argc == 3)       cmd_addprog(fd, argv[2]);
    else if (!strcmp(argv[1], "delprog")        && argc == 3)       cmd_delprog(fd, argv[2]);
    else if (!strcmp(argv[1], "adduid")         && argc == 3)       cmd_adduid(fd, (uint32_t)strtoul(argv[2], 0, 10));
    else if (!strcmp(argv[1], "deluid")         && argc == 3)       cmd_deluid(fd, (uint32_t)strtoul(argv[2], 0, 10));
    else if (!strcmp(argv[1], "addsys")         && argc == 3)       cmd_addsys(fd, (uint32_t)strtoul(argv[2], 0, 10));
    else if (!strcmp(argv[1], "delsys")         && argc == 3)       cmd_delsys(fd, (uint32_t)strtoul(argv[2], 0, 10));
    else if (!strcmp(argv[1], "setmax")         && argc == 3)       cmd_setmax(fd, (uint32_t)strtoul(argv[2], 0, 10));
    else if (!strcmp(argv[1], "on")             && argc == 2)       cmd_on(fd);
    else if (!strcmp(argv[1], "off")            && argc == 2)       cmd_off(fd);
    else if (!strcmp(argv[1], "listprog")       && argc == 2)       cmd_listprog(fd);
    else if (!strcmp(argv[1], "listuid")        && argc == 2)       cmd_listuid(fd);
    else if (!strcmp(argv[1], "listsys")        && argc == 2)       cmd_listsys(fd);
    else if (!strcmp(argv[1], "stats")          && argc == 2)       cmd_stats(fd);
    else if (!strcmp(argv[1], "resetstats")     && argc == 2)       cmd_resetstats(fd);
    else if (!strcmp(argv[1], "wakewaiters")    && argc == 2)       cmd_wakewaiters(fd);
    else if (!strcmp(argv[1], "print")    && argc == 2)             print_title();
    else                                                            usage(argv[0]);

    close(fd);
    return 0;
}