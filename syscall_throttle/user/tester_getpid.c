// tester_getpid.c
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Genera un numero enorme di syscall getpid() (nr=39 su x86_64).
 * Stampa un puntino ogni tanto per vedere che è vivo.
 */
int main(void) {
    uint64_t i = 0;
    for (;;) {
        (void)getpid();
        i++;
        if ((i % 10000000ULL) == 0) {
            if (write(1, ".", 1) < 0) {
                /* se stdout chiuso, basta uscire */
                _exit(1);
            }
        }
    }
    return 0;
}