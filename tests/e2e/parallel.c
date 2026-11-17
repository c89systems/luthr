#define _GNU_SOURCE
#include "../private/cpu_capacity.h"
#include <errno.h>
#include <inttypes.h>
#include <lauxlib.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)

struct memory {
  uint64_t live, peak;
};
struct job {
  lua_State *state;
  lua_Integer checksum;
  uint64_t service, latency;
};
struct pool;
struct worker {
  struct pool *pool;
  pthread_t thread;
  int cpu;
  size_t stack, guard;
};
struct pool {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  struct worker *workers;
  struct job *jobs;
  unsigned width, count, next, done, idle, generation;
  int stop, failed;
  uint64_t released;
  lua_Integer expected;
};

static uint64_t now(clockid_t clock) {
  struct timespec stamp;
  require(clock_gettime(clock, &stamp) == 0);
  return (uint64_t)stamp.tv_sec * 1000000000 + (uint64_t)stamp.tv_nsec;
}

/* Unlike ru_maxrss, this does not inherit the launcher's pre-exec peak. */
static uint64_t resident_peak(void) {
  char line[128];
  uint64_t peak = 0;
  FILE *file = fopen("/proc/self/status", "r");
  if (file == NULL)
    return 0;
  while (fgets(line, sizeof(line), file) != NULL)
    if (sscanf(line, "VmHWM: %" SCNu64 " kB", &peak) == 1)
      break;
  fclose(file);
  return peak;
}

static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct memory *memory = context;
  void *result;
  uint64_t live, peak;
  if (old == NULL)
    old_size = 0;
  if (size == 0) {
    free(old);
    (void)__atomic_fetch_sub(&memory->live, old_size, __ATOMIC_RELAXED);
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
  return result;
}

static void lock(struct pool *pool) {
  require(pthread_mutex_lock(&pool->mutex) == 0);
}
static void unlock(struct pool *pool) {
  require(pthread_mutex_unlock(&pool->mutex) == 0);
}
static void notice(struct pool *pool) {
  require(pthread_cond_broadcast(&pool->changed) == 0);
}
static void wait_change(struct pool *pool) {
  require(pthread_cond_wait(&pool->changed, &pool->mutex) == 0);
}

static void *run_worker(void *argument) {
  struct worker *worker = argument;
  struct pool *pool = worker->pool;
  unsigned generation = 0;
  cpu_set_t selected;
  pthread_attr_t attributes;
  void *base;
  CPU_ZERO(&selected);
  CPU_SET(worker->cpu, &selected);
  require(pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected) ==
          0);
  require(pthread_getattr_np(pthread_self(), &attributes) == 0);
  require(pthread_attr_getstack(&attributes, &base, &worker->stack) == 0);
  require(pthread_attr_getguardsize(&attributes, &worker->guard) == 0);
  require(pthread_attr_destroy(&attributes) == 0);
  lock(pool);
  for (;;) {
    pool->idle++;
    notice(pool);
    while (!pool->stop && generation == pool->generation)
      wait_change(pool);
    if (pool->stop) {
      unlock(pool);
      return NULL;
    }
    generation = pool->generation;
    unlock(pool);
    for (;;) {
      struct job *job;
      int results = 0, status;
      unsigned id;
      uint64_t started, finished;
      lock(pool);
      if (pool->next == pool->count) {
        unlock(pool);
        break;
      }
      id = pool->next++;
      unlock(pool);
      job = &pool->jobs[id];
      started = now(CLOCK_MONOTONIC);
      status = lua_resume(job->state, NULL, 1, &results);
      if (status == LUA_OK && results == 1 && lua_isinteger(job->state, -1))
        job->checksum = lua_tointeger(job->state, -1);
      else
        job->checksum = -1;
      finished = now(CLOCK_MONOTONIC);
      job->service = finished - started;
      job->latency = finished - pool->released;
      lock(pool);
      if (status != LUA_OK || job->checksum != pool->expected) {
        const char *error = status == LUA_OK ? "incorrect benchmark result"
                                             : lua_tostring(job->state, -1);
        fprintf(stderr, "job %u: %s\n", id + 1,
                error != NULL ? error : "non-string Lua error");
        pool->failed = 1;
      }
      pool->done++;
      notice(pool);
      unlock(pool);
    }
    lock(pool);
  }
}

static int number(const char *text, unsigned minimum, unsigned maximum,
                  unsigned *result) {
  char *end;
  unsigned long value;
  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno || end == text || *end || value < minimum || value > maximum)
    return 0;
  *result = (unsigned)value;
  return 1;
}

static int call(lua_State *state, int index) {
  lua_pushvalue(state, index);
  if (lua_pcall(state, 0, 0, 0) == LUA_OK)
    return 0;
  fprintf(stderr, "benchmark control: %s\n", lua_tostring(state, -1));
  lua_pop(state, 1);
  return -1;
}

static int compare_u64(const void *a, const void *b) {
  uint64_t first = *(const uint64_t *)a, second = *(const uint64_t *)b;
  return (first > second) - (first < second);
}

static void capacity_json(const struct bench_capacity *capacity) {
  printf("{\"affinity_cpus\":[");
  unsigned count = 0;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &capacity->affinity))
      printf("%s%d", count++ ? "," : "", cpu);
  printf("],\"affinity_count\":%u,\"quota_status\":\"%s\",", capacity->cpus,
         bench_quota_name(capacity->status));
  if (capacity->status == BENCH_QUOTA_UNKNOWN)
    printf("\"effective_cpu_milli\":null,");
  else
    printf("\"effective_cpu_milli\":%" PRIu64 ",", capacity->effective_milli);
  printf("\"quota_us\":%" PRIu64 ",\"period_us\":%" PRIu64 "}\n",
         capacity->quota_us, capacity->period_us);
}

int main(int argc, char **argv) {
  struct bench_capacity capacity;
  struct memory memory = {0, 0};
  struct pool pool = {0};
  lua_State *state;
  uint64_t *services, *latencies, stacks = 0, guards = 0;
  unsigned iterations, samples, warmups, created = 0;
  int failed = 0, cpu = 0;
  if (bench_capacity_read(&capacity, "/proc/self/cgroup",
                          "/proc/self/mountinfo") != 0) {
    perror("benchmark CPU affinity");
    return 2;
  }
  if (argc == 2 && strcmp(argv[1], "--capacity") == 0) {
    capacity_json(&capacity);
    return 0;
  }
  if (argc != 9 || !number(argv[4], 1, 64, &pool.width) ||
      !number(argv[5], 1, 4096, &pool.count) ||
      !number(argv[6], 1, 1000000, &iterations) ||
      !number(argv[7], 1, 1000, &samples) ||
      !number(argv[8], 0, 1000, &warmups) || pool.count < pool.width ||
      (strcmp(argv[3], "incremental") != 0 &&
       strcmp(argv[3], "generational") != 0)) {
    fprintf(stderr,
            "usage: %s script workload collector workers tasks iterations "
            "samples warmups\n",
            argv[0]);
    return 2;
  }
#ifndef LUTHR_BENCH_SHARED_VM
  if (pool.width != 1) {
    fputs("upstream shared-VM reference requires workers=1\n", stderr);
    return 2;
  }
#endif
  state = lua_newstate(allocate, &memory, 0);
  require(state != NULL);
  luaL_openlibs(state);
  (void)lua_gc(state,
               strcmp(argv[3], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  if (luaL_loadfile(state, argv[1]) != LUA_OK) {
    fprintf(stderr, "benchmark source: %s\n", lua_tostring(state, -1));
    lua_close(state);
    return 1;
  }
  lua_pushstring(state, argv[2]);
  lua_pushinteger(state, iterations);
  lua_pushinteger(state, pool.count);
  if (lua_pcall(state, 3, 4, 0) != LUA_OK) {
    fprintf(stderr, "benchmark setup: %s\n", lua_tostring(state, -1));
    lua_close(state);
    return 1;
  }
  require(lua_isfunction(state, 1) && lua_isfunction(state, 2) &&
          lua_isfunction(state, 3) && lua_isinteger(state, 4));
  pool.expected = lua_tointeger(state, 4);
  require(pool.expected > 0 && lua_checkstack(state, (int)pool.count + 8));
  pool.jobs = calloc(pool.count, sizeof(*pool.jobs));
  pool.workers = calloc(pool.width, sizeof(*pool.workers));
  services = calloc(pool.count, sizeof(*services));
  latencies = calloc(pool.count, sizeof(*latencies));
  require(pool.jobs != NULL && pool.workers != NULL && services != NULL &&
          latencies != NULL);
  require(pthread_mutex_init(&pool.mutex, NULL) == 0 &&
          pthread_cond_init(&pool.changed, NULL) == 0);
  for (unsigned i = 0; i < pool.count; ++i)
    pool.jobs[i].state = lua_newthread(state);
  for (unsigned i = 0; i < pool.width; ++i) {
    struct worker *worker = &pool.workers[i];
    while (!CPU_ISSET(cpu, &capacity.affinity))
      cpu = (cpu + 1) % CPU_SETSIZE;
    worker->cpu = cpu;
    cpu = (cpu + 1) % CPU_SETSIZE;
    worker->pool = &pool;
    require(pthread_create(&worker->thread, NULL, run_worker, worker) == 0);
    created++;
  }
  lock(&pool);
  while (pool.idle != pool.width)
    wait_change(&pool);
  unlock(&pool);
  for (unsigned i = 0; i < pool.width; ++i) {
    stacks += pool.workers[i].stack;
    guards += pool.workers[i].guard;
  }
  for (unsigned sample = 0; sample < warmups + samples; ++sample) {
    uint64_t cpu_start, elapsed_cpu, elapsed_wall, live, peak, retained,
        checksum = 0;
    if (call(state, 2) != 0) {
      failed = 1;
      break;
    }
    for (unsigned i = 0; i < pool.count; ++i) {
      lua_State *child = pool.jobs[i].state;
      require(lua_gettop(child) == 0);
      lua_pushvalue(state, 1);
      lua_xmove(state, child, 1);
      lua_pushinteger(child, i + 1);
      pool.jobs[i].checksum = 0;
    }
    (void)lua_gc(state, LUA_GCCOLLECT);
    live = __atomic_load_n(&memory.live, __ATOMIC_RELAXED);
    __atomic_store_n(&memory.peak, live, __ATOMIC_RELAXED);
    lock(&pool);
    pool.next = pool.done = pool.idle = 0;
    cpu_start = now(CLOCK_PROCESS_CPUTIME_ID);
    pool.released = now(CLOCK_MONOTONIC);
    pool.generation++;
    notice(&pool);
    while (pool.done != pool.count || pool.idle != pool.width)
      wait_change(&pool);
    failed = pool.failed;
    unlock(&pool);
    for (unsigned i = 0; i < pool.count; ++i) {
      checksum += (uint64_t)pool.jobs[i].checksum;
      services[i] = pool.jobs[i].service;
      latencies[i] = pool.jobs[i].latency;
      int closed = lua_closethread(pool.jobs[i].state, NULL);
      if (!failed)
        require(closed == LUA_OK);
    }
    if (failed || call(state, 3) != 0) {
      failed = 1;
      break;
    }
    require(checksum == (uint64_t)pool.expected * pool.count);
    (void)lua_gc(state, LUA_GCCOLLECT);
    elapsed_wall = now(CLOCK_MONOTONIC) - pool.released;
    elapsed_cpu = now(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
    peak = __atomic_load_n(&memory.peak, __ATOMIC_RELAXED);
    retained = __atomic_load_n(&memory.live, __ATOMIC_RELAXED);
    qsort(services, pool.count, sizeof(*services), compare_u64);
    qsort(latencies, pool.count, sizeof(*latencies), compare_u64);
    unsigned median = (pool.count + 1) / 2 - 1,
             p95 = (95 * pool.count + 99) / 100 - 1;
    if (sample >= warmups)
      printf("%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
             ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
             ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%u,%" PRIu64
             ",%" PRIu64 ",%" PRIu64 ",%s\n",
             sample - warmups + 1, checksum, elapsed_cpu, elapsed_wall,
             services[median], services[p95], latencies[median], latencies[p95],
             latencies[pool.count - 1], live, peak, retained, resident_peak(),
             stacks, guards, pool.done, capacity.cpus, capacity.quota_us,
             capacity.period_us, capacity.effective_milli,
             bench_quota_name(capacity.status));
  }
  lock(&pool);
  pool.stop = 1;
  notice(&pool);
  unlock(&pool);
  for (unsigned i = 0; i < created; ++i)
    require(pthread_join(pool.workers[i].thread, NULL) == 0);
  require(pthread_cond_destroy(&pool.changed) == 0 &&
          pthread_mutex_destroy(&pool.mutex) == 0);
  lua_close(state);
  require(__atomic_load_n(&memory.live, __ATOMIC_RELAXED) == 0);
  free(pool.jobs);
  free(pool.workers);
  free(services);
  free(latencies);
  return failed ? 1 : 0;
}
