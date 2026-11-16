#ifndef LUTHR_TEST_CPU_CAPACITY_H
#define LUTHR_TEST_CPU_CAPACITY_H

#include <sched.h>
#include <stdint.h>

enum bench_quota {
  BENCH_QUOTA_UNKNOWN,
  BENCH_QUOTA_UNLIMITED,
  BENCH_QUOTA_LIMITED
};
struct bench_capacity {
  cpu_set_t affinity;
  unsigned cpus;
  enum bench_quota status;
  uint64_t quota_us, period_us, effective_milli;
};

/* Paths are injectable for ordinary cgroup fixtures. No files are changed. */
int bench_capacity_read(struct bench_capacity *, const char *groups,
                        const char *mounts);
const char *bench_quota_name(enum bench_quota);

#endif
