#ifndef LUTHR_CPU_CAPACITY_H
#define LUTHR_CPU_CAPACITY_H

#include <sched.h>
#include <stdint.h>

enum luthr_quota {
  LUTHR_QUOTA_UNKNOWN,
  LUTHR_QUOTA_UNLIMITED,
  LUTHR_QUOTA_LIMITED
};
struct luthr_capacity {
  cpu_set_t affinity;
  unsigned cpus;
  enum luthr_quota status;
  uint64_t quota_us, period_us, effective_milli;
};

/* Paths are injectable for ordinary cgroup fixtures. No files are changed. */
int luthr_capacity_read(struct luthr_capacity *, const char *groups,
                        const char *mounts);
const char *luthr_quota_name(enum luthr_quota);

#endif
