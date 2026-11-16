#define _GNU_SOURCE
#include "private/cpu_capacity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)

int main(int argc, char **argv) {
  struct bench_capacity capacity;
  cpu_set_t original, selected;
  unsigned original_count;
  require(argc == 6);
  require(bench_capacity_read(&capacity, argv[1], argv[2]) == 0);
  require(strcmp(bench_quota_name(capacity.status), argv[3]) == 0);
  original = capacity.affinity;
  original_count = capacity.cpus;
  if (capacity.status == BENCH_QUOTA_LIMITED) {
    require(capacity.quota_us == strtoull(argv[4], NULL, 10));
    require(capacity.period_us == strtoull(argv[5], NULL, 10));
  }
  require(capacity.effective_milli <= (uint64_t)capacity.cpus * 1000);
  if (capacity.status == BENCH_QUOTA_UNKNOWN)
    require(capacity.effective_milli == 0);
  else
    require(capacity.effective_milli > 0);
  CPU_ZERO(&selected);
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &original)) {
      CPU_SET(cpu, &selected);
      break;
    }
  require(sched_setaffinity(0, sizeof(selected), &selected) == 0);
  require(bench_capacity_read(&capacity, argv[1], argv[2]) == 0);
  require(capacity.cpus == 1 && CPU_COUNT(&capacity.affinity) == 1);
  require(capacity.effective_milli <= 1000);
  require(strcmp(bench_quota_name(capacity.status), argv[3]) == 0);
  require(sched_setaffinity(0, sizeof(original), &original) == 0);
  require(original_count != 0);
  return 0;
}
