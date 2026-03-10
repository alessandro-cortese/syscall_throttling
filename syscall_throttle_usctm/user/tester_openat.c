// tester_openat.c
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

/*
 * Stressa openat/close su /dev/null.
 * Utile se vuoi provare a registrare openat.
 */
int main(void) {
    uint64_t i = 0;
    for (;;) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) close(fd);
        i++;
        if (write(1, ".", 1) < 0) {
            /* se stdout chiuso, basta uscire */
            _exit(1);
        }
    }
    return 0;
}