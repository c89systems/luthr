#include <errno.h>
#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lscheduler.h>
#include <lualib.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  int armed, sweep, entered, release;
  int pause, paused, pause_release, hold_driver;
  unsigned after_pause;
  unsigned workers, batches;
} probe = {.mutex = PTHREAD_MUTEX_INITIALIZER,
           .changed = PTHREAD_COND_INITIALIZER};
static LuaScheduler *root_scheduler;

void __real_luaW_armtrace(lua_State *);
void __wrap_luaW_armtrace(lua_State *L) {
  if (root_scheduler != NULL) {
    for (unsigned i = 0; i < root_scheduler->config.slots; ++i)
      require(!iswhite(G(L), root_scheduler->slots[i].collector));
  }
  __real_luaW_armtrace(L);
}

static void observe(lua_State *L, int sweep) {
  LuaExecution *execution = luaQ_current();
  struct timespec deadline;
  int worker = execution != NULL && execution->task == NULL;
  require(pthread_mutex_lock(&probe.mutex) == 0);
  if (!probe.armed || (probe.armed == 1 && probe.sweep != sweep)) {
    require(pthread_mutex_unlock(&probe.mutex) == 0);
    return;
  }
  if (worker) {
    probe.batches++;
    if (probe.pause && probe.release && !probe.paused)
      probe.after_pause++;
    if (probe.armed == 1 && !probe.entered) {
      probe.entered = 1;
      probe.workers++;
      require(pthread_cond_broadcast(&probe.changed) == 0);
      while (!probe.release)
        require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
      probe.workers--;
    }
  } else if (execution == NULL && probe.armed == 1 &&
             (probe.entered || luaC_workerready(G(L)))) {
    /* The driver may hold every ready root in its first batch. Let that
     * batch publish its children before waiting for another scanner. */
    require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 10;
    while (!probe.entered)
      require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
              0);
    while (probe.hold_driver && !probe.release)
      require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
  }
  require(pthread_mutex_unlock(&probe.mutex) == 0);
}

void __real_luaC_scantrace(lua_State *, LuaTraceWork *, unsigned);
void __wrap_luaC_scantrace(lua_State *L, LuaTraceWork *work, unsigned limit) {
  observe(L, 0);
  __real_luaC_scantrace(L, work, limit);
}
void __real_luaC_scansweep(lua_State *, LuaSweepWork *);
void __wrap_luaC_scansweep(lua_State *L, LuaSweepWork *work) {
  observe(L, 1);
  __real_luaC_scansweep(L, work);
}

static void *collect(void *argument) {
  lua_gc(argument, LUA_GCCOLLECT);
  return NULL;
}

static void *pause_world(void *argument) {
  lua_State *L = argument;
  lua_lock(L);
  luaW_stop(L);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.paused = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  while (!probe.pause_release)
    require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  luaW_start(L);
  lua_unlock(L);
  return NULL;
}

static void *try_stopped(void *argument) {
  LuaScheduler *scheduler = argument;
  LuaCarrier carrier = {0};
  LuaExecutionSlot *slot = &scheduler->slots[0];
  void *expected = NULL;
  carrier.scheduler = scheduler;
  require(__atomic_compare_exchange_n(&slot->owner, &expected,
                                      &slot->collector_execution, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
  luaSC_collectidle(&carrier, slot);
  require(luaW_active(&slot->collector->participation) == 0);
  __atomic_store_n(&slot->owner, NULL, __ATOMIC_SEQ_CST);
  return NULL;
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate(), *task_state, *receiver;
  LuaScheduler scheduler = {0};
  LuaScheduledTask task = {0};
  LuaSchedulerConfig config = {2, 3, 1, 1, 1};
  pthread_t collector, pauser;
  struct timespec deadline;
  int status, results, single, stopped, pause;
  require(argc == 2 && L != NULL);
  single = strncmp(argv[1], "single_", 7) == 0;
  stopped = strcmp(argv[1], "stopped_incremental") == 0;
  pause = strstr(argv[1], "pause") != NULL;
  probe.hold_driver = pause;
  if (single) {
    config.slots = 1;
    config.carriers = 2;
  }
  probe.sweep = strncmp(argv[1], "sweep_", 6) == 0;
  luaL_openlibs(L);
  lua_gc(L, strstr(argv[1], "generational") ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_dostring(L,
                        "local t={} for i=1,16000 do "
                        "t[i]={i,i+1,tag='retained'} end return t") == LUA_OK);
  task_state = lua_newthread(L);
  receiver = lua_newthread(L);
  require(luaL_loadstring(task_state,
                          "local n=0 for i=1,10000 do n=n+i end return n") ==
          LUA_OK);
  if (stopped) {
    unsigned steps = 0;
    lua_gc(L, LUA_GCRESTART);
    do {
      lua_gc(L, LUA_GCSTEP, (size_t)0);
      require(++steps < 10000);
    } while (!luaC_sweeping(L));
    lua_gc(L, LUA_GCSTOP);
    require(luaL_loadstring(task_state,
                            "local t={} for i=1,10000 do t[i]={i} end "
                            "return #t") == LUA_OK);
    lua_remove(task_state, 1);
  }
  require(luaSC_init(L, &scheduler, &config) == 0);
  if (strcmp(argv[1], "roots") == 0) {
    uintptr_t collectors[2];
    for (unsigned i = 0; i < 2; ++i)
      collectors[i] = (uintptr_t)scheduler.slots[i].collector;
    root_scheduler = &scheduler;
    require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
    require(luaC_tracing(L));
    root_scheduler = NULL;
    require(luaSC_shutdown(&scheduler) == LUA_OK);
    require(luaSC_destroy(L, &scheduler) == 0);
    lua_gc(L, LUA_GCCOLLECT);
    for (LuaParticipation *part = G(L)->world.participants; part != NULL;
         part = part->next)
      for (unsigned i = 0; i < 2; ++i)
        require((uintptr_t)part->thread != collectors[i]);
    lua_close(L);
    return 0;
  }
  if (single || stopped) {
    probe.armed =
        2; /* Record without holding a batch or waiting for a helper. */
    if (single)
      lua_gc(L, LUA_GCCOLLECT);
    require(luaSC_submit(&scheduler, &task, task_state, 0) == LUA_OK);
    luaSC_wait(&task);
    require(luaSC_collect(receiver, &task, &status, &results) == LUA_OK);
    require(status == LUA_OK && results == 1 &&
            lua_tointeger(receiver, -1) == (stopped ? 10000 : 50005000));
    lua_pop(receiver, 1);
    require(pthread_mutex_lock(&probe.mutex) == 0);
    require(probe.batches == 0);
    probe.armed = 0;
    require(pthread_mutex_unlock(&probe.mutex) == 0);
    lua_gc(L, LUA_GCRESTART);
    lua_gc(L, LUA_GCCOLLECT);
    require(lua_rawlen(L, 1) == 16000);
    require(luaSC_shutdown(&scheduler) == LUA_OK);
    require(luaSC_destroy(L, &scheduler) == 0);
    lua_close(L);
    return 0;
  }
  if (strcmp(argv[1], "admission") == 0) {
    /* A phase requester can need managed capacity to resume. A late idle
     * helper must refuse entry rather than park while retaining that slot. */
    lua_lock(L);
    luaW_stop(L);
    require(pthread_create(&collector, NULL, try_stopped, &scheduler) == 0);
    require(pthread_join(collector, NULL) == 0);
    luaW_start(L);
    lua_unlock(L);
    require(luaSC_destroy(L, &scheduler) == 0);
    lua_close(L);
    return 0;
  }
  lua_gc(L, LUA_GCRESTART);
  probe.armed = 1;
  require(pthread_create(&collector, NULL, collect, L) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  while (!probe.entered)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(probe.workers == 1);
  require(pthread_mutex_unlock(&probe.mutex) == 0);

  /* A real collector batch retains one managed slot. A newly submitted Lua
   * task must still complete on the other slot before that scanner resumes. */
  require(luaSC_submit(&scheduler, &task, task_state, 0) == LUA_OK);
  luaSC_wait(&task);
  require(luaSC_collect(receiver, &task, &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1 &&
          lua_tointeger(receiver, -1) == 50005000);
  lua_pop(receiver, 1);
  if (pause) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.pause = 1;
    require(pthread_mutex_unlock(&probe.mutex) == 0);
    require(pthread_create(&pauser, NULL, pause_world, receiver) == 0);
    while (!luaW_stopped(&G(L)->world))
      sched_yield();
  }
  require(pthread_mutex_lock(&probe.mutex) == 0);
  require(probe.workers == 1 && probe.batches != 0);
  probe.release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  if (pause) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    while (!probe.paused)
      require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
    require(probe.after_pause == 0);
    probe.pause_release = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
    require(pthread_join(pauser, NULL) == 0);
  }
  require(pthread_join(collector, NULL) == 0);
  probe.armed = 0;
  require(lua_rawlen(L, 1) == 16000);
  lua_rawgeti(L, 1, 16000);
  lua_rawgeti(L, -1, 2);
  require(lua_tointeger(L, -1) == 16001);
  lua_pop(L, 2);
  require(luaSC_shutdown(&scheduler) == LUA_OK);
  require(luaSC_destroy(L, &scheduler) == 0);
  lua_settop(L, 0);
  lua_gc(L, LUA_GCCOLLECT);
  lua_close(L);
  require(pthread_cond_destroy(&probe.changed) == 0);
  require(pthread_mutex_destroy(&probe.mutex) == 0);
  return 0;
}
