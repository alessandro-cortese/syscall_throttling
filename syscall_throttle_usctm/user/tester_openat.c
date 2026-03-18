// tester_openat.c
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

/*
 * Redirects openat/close to /dev/null.
 * Useful if you want to try recording openat.
 */
int main(void) {
    uint64_t i = 0;
    for (;;) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) close(fd);
        i++;
        if (write(1, ".", 1) < 0) {
            /* if stdout is closed, simply exit */
            _exit(1);
        }
    }
    return 0;
}