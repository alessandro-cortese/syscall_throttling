// tester_getpid.c
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Generates a huge number of getpid() system calls, 39 on x86_64.
 * Prints a dot every now and then to show it's still running.
 */
int main(void) {
    uint64_t i = 0;
    for (;;) {
        (void)getpid();
        i++;
        if ((i % 10000000ULL) == 0) {
            if (write(1, ".", 1) < 0) {
                /* if stdout is closed, simply exit */
                _exit(1);
            }
        }
    }
    return 0;
}