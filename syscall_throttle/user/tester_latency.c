// user/tester_latency.c
#define _GNU_SOURCE
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static inline uint64_t nsec_now(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv){
  double seconds = (argc > 1) ? atof(argv[1]) : 2.0;
  uint64_t end = nsec_now() + (uint64_t)(seconds*1e9);

  uint64_t n=0, sum=0, max=0;
  while(nsec_now() < end){
    uint64_t t0 = nsec_now();
    (void)getpid();
    uint64_t t1 = nsec_now();
    uint64_t dt = t1 - t0;
    sum += dt;
    if(dt > max) max = dt;
    n++;
  }

  double avg = (n ? (double)sum/n : 0.0);
  printf("calls=%llu avg_ns=%.1f max_ns=%llu\n", (unsigned long long)n, avg, (unsigned long long)max);
  return 0;
}