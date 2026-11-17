#define _GNU_SOURCE
#include "../../src/cpu_capacity.h"
#include <errno.h>
#include <inttypes.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef LUTHR_BENCH_SCHEDULER
#include <luthr/tasks.h>
#include <poll.h>
#endif
#ifdef LUTHR_BENCH_GC_PROBE
#include "gc_probe.h"
#endif

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

struct memory {
  uint64_t live, peak;
};
struct resident {
  uint64_t rss, hwm, threads;
};
struct job {
  lua_State *state;
  uint64_t latency;
#ifdef LUTHR_BENCH_SCHEDULER
  luthr_lua_task *task;
#endif
#ifdef LUTHR_BENCH_GC_PROBE
  GcProbe probe;
#endif
};
static unsigned delay_us;

static uint64_t now(clockid_t clock) {
  struct timespec stamp;
  require(clock_gettime(clock, &stamp) == 0);
  return (uint64_t)stamp.tv_sec * 1000000000 + (uint64_t)stamp.tv_nsec;
}
static struct resident resident(void) {
  struct resident result = {0};
  FILE *file = fopen("/proc/self/status", "r");
  char line[128];
  require(file != NULL);
  while (fgets(line, sizeof(line), file) != NULL) {
    if (sscanf(line, "VmRSS: %" SCNu64 " kB", &result.rss) == 1)
      continue;
    if (sscanf(line, "VmHWM: %" SCNu64 " kB", &result.hwm) == 1)
      continue;
    (void)sscanf(line, "Threads: %" SCNu64, &result.threads);
  }
  require(fclose(file) == 0 && result.threads != 0);
  return result;
}
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct memory *memory = context;
  uint64_t live, peak;
  void *result;
  if (old == NULL)
    old_size = 0;
  if (size == 0) {
    free(old);
    __atomic_fetch_sub(&memory->live, old_size, __ATOMIC_RELAXED);
    return NULL;
  }
  result = realloc(old, size);
  if (result == NULL)
    return NULL;
  if (size >= old_size)
    live =
        __atomic_fetch_add(&memory->live, size - old_size, __ATOMIC_RELAXED) +
        size - old_size;
  else
    live =
        __atomic_fetch_sub(&memory->live, old_size - size, __ATOMIC_RELAXED) -
        (old_size - size);
  peak = __atomic_load_n(&memory->peak, __ATOMIC_RELAXED);
  while (peak < live &&
         !__atomic_compare_exchange_n(&memory->peak, &peak, live, 0,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
#ifdef LUTHR_BENCH_GC_PROBE
  gc_probe_allocation(live);
#endif
  return result;
}
static int pause_native(lua_State *L) {
  struct timespec remaining = {delay_us / 1000000, (delay_us % 1000000) * 1000};
  (void)L;
  while (nanosleep(&remaining, &remaining) != 0)
    require(errno == EINTR);
  return 0;
}
static int number(const char *text, unsigned minimum, unsigned maximum,
                  unsigned *out) {
  char *end;
  unsigned long value;
  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno || end == text || *end || value < minimum || value > maximum)
    return 0;
  *out = (unsigned)value;
  return 1;
}
static const char *error_text(lua_State *L) {
  const char *message = lua_tostring(L, -1);
  return message != NULL ? message : "non-string Lua error";
}
static int call(lua_State *L, int index) {
  lua_pushvalue(L, index);
  if (lua_pcall(L, 0, 0, 0) == LUA_OK)
    return 0;
  fprintf(stderr, "benchmark control: %s\n", error_text(L));
  lua_pop(L, 1);
  return 1;
}
static int compare(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}
static void field(const char *name, uint64_t value) {
  printf(",\"%s\":%" PRIu64, name, value);
}
static void capacity(const struct luthr_capacity *cpus) {
  field("affinity_count", cpus->cpus);
  field("quota_us", cpus->quota_us);
  field("period_us", cpus->period_us);
  field("effective_cpu_milli", cpus->effective_milli);
  printf(",\"quota_status\":\"%s\"", luthr_quota_name(cpus->status));
}

int main(int argc, char **argv) {
  struct luthr_capacity cpus;
  struct memory memory = {0};
  struct job *jobs;
  uint64_t *latencies;
  lua_State *L;
  lua_Integer expected;
  unsigned slots, native, tasks, iterations, samples, warmups;
  unsigned selected_slots = 1, selected_carriers = 0, selected_native = 0;
  int failed = 0, stopped;
#ifdef LUTHR_BENCH_GC_PROBE
  GcProbe host_probe = {0};
#endif
#ifdef LUTHR_BENCH_SCHEDULER
  luthr_lua_scheduler *scheduler;
  luthr_lua_scheduler_config config = {0};
  luthr_lua_scheduler_stats stats;
#endif
  require(luthr_capacity_read(&cpus, "/proc/self/cgroup",
                              "/proc/self/mountinfo") == 0);
  if (argc == 2 && strcmp(argv[1], "--capacity") == 0) {
    printf("{\"affinity_cpus\":[");
    unsigned count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
      if (CPU_ISSET(cpu, &cpus.affinity))
        printf("%s%d", count++ ? "," : "", cpu);
    printf("]");
    capacity(&cpus);
    puts("}");
    return 0;
  }
  if ((argc != 11 && argc != 12) || !number(argv[4], 0, 64, &slots) ||
      !number(argv[5], 0, 64, &native) || !number(argv[6], 1, 4096, &tasks) ||
      !number(argv[7], 1, 1000000, &iterations) ||
      !number(argv[8], 1, 1000, &samples) ||
      !number(argv[9], 0, 1000, &warmups) ||
      !number(argv[10], 0, 1000000, &delay_us) ||
      (strcmp(argv[3], "incremental") != 0 &&
       strcmp(argv[3], "generational") != 0) ||
      (argc == 12 && strcmp(argv[11], "stopped") != 0)) {
    fprintf(stderr,
            "usage: %s script workload collector slots native_limit "
            "tasks iterations samples warmups delay_us [stopped]\n",
            argv[0]);
    return 2;
  }
  stopped = argc == 12;
#ifndef LUTHR_BENCH_SCHEDULER
  if (slots != 1 || native != 0) {
    fputs("upstream reference requires slots=1 and native_limit=0\n", stderr);
    return 2;
  }
#endif
  L = lua_newstate(allocate, &memory, 0);
  require(L != NULL);
  luaL_openlibs(L);
  lua_pushcfunction(L, pause_native);
  lua_setglobal(L, "benchmark_delay");
  lua_gc(L, strcmp(argv[3], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  if (luaL_loadfile(L, argv[1]) != LUA_OK) {
    fprintf(stderr, "benchmark source: %s\n", error_text(L));
    lua_close(L);
    return 1;
  }
  lua_pushstring(L, argv[2]);
  lua_pushinteger(L, iterations);
  lua_pushinteger(L, tasks);
  if (lua_pcall(L, 3, 4, 0) != LUA_OK) {
    fprintf(stderr, "benchmark setup: %s\n", error_text(L));
    lua_close(L);
    return 1;
  }
  require(lua_isfunction(L, 1) && lua_isfunction(L, 2) &&
          lua_isfunction(L, 3) && lua_isinteger(L, 4));
  expected = lua_tointeger(L, 4);
  require(expected > 0 && lua_checkstack(L, (int)tasks + 8));
  if (delay_us != 0) {
    require(luaL_loadstring(
                L,
                "local run=...; return function(id) "
                "if id%4==0 then benchmark_delay() end; return run(id) end") ==
            LUA_OK);
    lua_pushvalue(L, 1);
    require(lua_pcall(L, 1, 1, 0) == LUA_OK);
    lua_replace(L, 1);
  }
  jobs = calloc(tasks, sizeof(*jobs));
  latencies = calloc(tasks, sizeof(*latencies));
  require(jobs != NULL && latencies != NULL);
  for (unsigned i = 0; i < tasks; ++i)
    jobs[i].state = lua_newthread(L);
#ifdef LUTHR_BENCH_GC_PROBE
  gc_probe_bind(L, &host_probe);
#endif
#ifdef LUTHR_BENCH_SCHEDULER
  config.parallelism = slots;
  config.native_limit = native;
  require(luthr_lua_scheduler_new(L, &config, &scheduler) ==
          LUTHR_LUA_RUNTIME_OK);
  require(luthr_lua_scheduler_snapshot(scheduler, &stats) ==
          LUTHR_LUA_RUNTIME_OK);
  selected_slots = stats.parallelism;
  selected_carriers = stats.carrier_limit;
  selected_native = stats.native_limit;
#endif
  for (unsigned sample = 0; sample < samples + warmups; ++sample) {
    uint64_t cpu_start, released, wall, cpu, live, peak, retained, checksum = 0;
    unsigned done = 0, native_peak = 0;
    struct resident usage;
#ifdef LUTHR_BENCH_GC_PROBE
    GcProbe total = {0};
#endif
    if (call(L, 2)) {
      failed = 1;
      break;
    }
    for (unsigned i = 0; i < tasks; ++i) {
      require(lua_gettop(jobs[i].state) == 0);
      lua_pushvalue(L, 1);
      lua_xmove(L, jobs[i].state, 1);
      lua_pushinteger(jobs[i].state, i + 1);
    }
    lua_gc(L, LUA_GCCOLLECT);
    live = __atomic_load_n(&memory.live, __ATOMIC_RELAXED);
    __atomic_store_n(&memory.peak, live, __ATOMIC_RELAXED);
    if (stopped)
      lua_gc(L, LUA_GCSTOP);
#ifdef LUTHR_BENCH_GC_PROBE
    memset(&host_probe, 0, sizeof(host_probe));
    host_probe.enabled = 1;
    for (unsigned i = 0; i < tasks; ++i) {
      memset(&jobs[i].probe, 0, sizeof(jobs[i].probe));
      jobs[i].probe.enabled = 1;
    }
#endif
    cpu_start = now(CLOCK_PROCESS_CPUTIME_ID);
    released = now(CLOCK_MONOTONIC);
#ifdef LUTHR_BENCH_SCHEDULER
    for (unsigned i = 0; i < tasks; ++i) {
      const luthr_lua_task_options *settings = NULL;
#ifdef LUTHR_BENCH_GC_PROBE
      luthr_lua_task_options options = {0};
      options.context = &jobs[i].probe;
      settings = &options;
#endif
      require(luthr_lua_task_submit(scheduler, jobs[i].state, 1, settings,
                                    &jobs[i].task) == LUTHR_LUA_RUNTIME_OK);
    }
    while (done != tasks) {
      require(luthr_lua_scheduler_snapshot(scheduler, &stats) ==
              LUTHR_LUA_RUNTIME_OK);
      if (stats.native_active > native_peak)
        native_peak = stats.native_active;
      for (unsigned i = 0; i < tasks; ++i) {
        luthr_lua_task_result result;
        if (jobs[i].task == NULL)
          continue;
        luthr_lua_runtime_status status =
            luthr_lua_task_poll(jobs[i].task, &result);
        if (status == LUTHR_LUA_RUNTIME_PENDING)
          continue;
        require(status == LUTHR_LUA_RUNTIME_OK);
        /* Host-observed completion includes notification/poll latency. */
        jobs[i].latency = now(CLOCK_MONOTONIC) - released;
        if (result.status != LUTHR_LUA_RUNTIME_OK) {
          fprintf(stderr, "job %u: %s\n", i + 1,
                  luthr_lua_task_error(jobs[i].task));
          failed = 1;
        }
        require(luthr_lua_task_collect(jobs[i].task, L, &result) ==
                LUTHR_LUA_RUNTIME_OK);
        if (!failed) {
          require(result.result_count == 1 && lua_isinteger(L, -1));
          require(lua_tointeger(L, -1) == expected);
          checksum += (uint64_t)lua_tointeger(L, -1);
        }
        lua_pop(L, result.result_count);
        require(luthr_lua_task_free(jobs[i].task) == LUTHR_LUA_RUNTIME_OK);
        jobs[i].task = NULL;
        done++;
      }
      if (done != tasks) {
        struct pollfd descriptor = {luthr_lua_scheduler_fd(scheduler), POLLIN,
                                    0};
        int status;
        do {
          status = poll(&descriptor, 1, -1);
        } while (status < 0 && errno == EINTR);
        require(status == 1 && (descriptor.revents & POLLIN));
        require(luthr_lua_scheduler_drain(scheduler) == LUTHR_LUA_RUNTIME_OK);
      }
    }
#else
    for (unsigned i = 0; i < tasks; ++i) {
      int results, status = lua_resume(jobs[i].state, NULL, 1, &results);
      jobs[i].latency = now(CLOCK_MONOTONIC) - released;
      if (status != LUA_OK) {
        fprintf(stderr, "job %u: %s\n", i + 1, error_text(jobs[i].state));
        failed = 1;
      } else {
        require(results == 1 && lua_isinteger(jobs[i].state, -1));
        require(lua_tointeger(jobs[i].state, -1) == expected);
        checksum += (uint64_t)lua_tointeger(jobs[i].state, -1);
      }
      lua_pop(jobs[i].state, results);
      done++;
    }
#endif
    wall = now(CLOCK_MONOTONIC) - released;
    cpu = now(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
#ifdef LUTHR_BENCH_GC_PROBE
    host_probe.enabled = 0;
    gc_probe_add(&total, &host_probe);
    for (unsigned i = 0; i < tasks; ++i) {
      jobs[i].probe.enabled = 0;
      gc_probe_add(&total, &jobs[i].probe);
    }
#endif
    if (stopped)
      lua_gc(L, LUA_GCRESTART);
    peak = __atomic_load_n(&memory.peak, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < tasks; ++i) {
      latencies[i] = jobs[i].latency;
      int status = lua_closethread(jobs[i].state, NULL);
      require(failed || status == LUA_OK);
      lua_settop(jobs[i].state, 0);
    }
    if (failed || call(L, 3)) {
      failed = 1;
      break;
    }
    require(checksum == (uint64_t)expected * tasks);
    lua_gc(L, LUA_GCCOLLECT);
    retained = __atomic_load_n(&memory.live, __ATOMIC_RELAXED);
    usage = resident();
    qsort(latencies, tasks, sizeof(*latencies), compare);
    if (sample < warmups)
      continue;
    printf("{\"sample\":%u", sample - warmups + 1);
    field("checksum", checksum);
    field("cpu_ns", cpu);
    field("wall_ns", wall);
    field("completion_p50_ns", latencies[(tasks + 1) / 2 - 1]);
    field("completion_p95_ns", latencies[(tasks * 95 + 99) / 100 - 1]);
    field("completion_max_ns", latencies[tasks - 1]);
    field("lua_live_bytes", live);
    field("lua_peak_bytes", peak);
    field("lua_retained_bytes", retained);
    field("rss_kib", usage.rss);
    field("rss_hwm_kib", usage.hwm);
    field("threads", usage.threads);
    field("parallelism", selected_slots);
    field("carrier_limit", selected_carriers);
    field("native_limit", selected_native);
    field("observed_native_peak", native_peak);
    field("completed", done);
    field("automatic_gc_stopped", stopped);
#ifdef LUTHR_BENCH_GC_PROBE
    gc_probe_emit(&total);
#endif
    capacity(&cpus);
    puts("}");
  }
#ifdef LUTHR_BENCH_SCHEDULER
  require(luthr_lua_scheduler_free(scheduler) == LUTHR_LUA_RUNTIME_OK);
#endif
  lua_close(L);
  require(memory.live == 0);
  free(latencies);
  free(jobs);
  return failed ? 1 : 0;
}
