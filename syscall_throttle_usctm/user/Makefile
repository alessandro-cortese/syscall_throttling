CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

all: scthctl tester_getpid tester_openat tester_latency

scthctl: scthctl.c
	$(CC) $(CFLAGS) -o $@ $<

tester_getpid: tester_getpid.c
	$(CC) $(CFLAGS) -o $@ $<

tester_openat: tester_openat.c
	$(CC) $(CFLAGS) -o $@ $<

tester_latency: tester_latency.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f scthctl tester_getpid tester_openat tester_latency