#define _GNU_SOURCE
#include <errno.h>
#include <lapi.h>
#include <lauxlib.h>
#include <lscheduler.h>
#include <lualib.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define TASKS 12
#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

struct fixture {
  LuaScheduler scheduler;
  LuaScheduledTask tasks[TASKS];
  lua_State *main;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_mutex_t idle_mutex;
  pthread_cond_t idle_changed;
  pthread_barrier_t parallel;
  LuaWaiter *waiters;
  unsigned started, seen[TASKS];
  unsigned waiting, native_entered, native_release;
  unsigned admission_pause, admission_paused, admission_release;
  unsigned hold_grant_attach, grant_attach_seen, grant_attach_release;
  int admission_fifo, cancel_grant;
  unsigned pause_hold, pause_seen, pause_release;
  unsigned finalized, nursery_finished;
  unsigned native_callback_done;
  unsigned idle_entries, host_channel_entries;
  lua_State *metadata_peer;
  unsigned metadata_held, metadata_queued, metadata_released;
  size_t live;
  uint64_t poll_time_ns;
  unsigned native_lease_test, native_lease_polls;
  int inspect_polls, shutting_down, parallel_check, deny;
};
static struct fixture *fixture;
static __thread int inspecting;
static __thread int measuring_scheduler;
static __thread unsigned engine_locks, queue_locks;
static unsigned poll_locks, poll_calls;
static int fail_mutex = -1, fail_condition = -1, fail_carrier = -1;
static int fail_mapping, fail_protection;
static unsigned live_stacks;
static int observe_idle;
static pthread_t idle_host;

void __real_luaC_endtask(lua_State *);
void __wrap_luaC_endtask(lua_State *L) {
  struct fixture *f = fixture;
  if (f != NULL && f->tasks[0].root.thread == L) {
    require(L->task_tables != NULL);
    f->nursery_finished++;
  }
  __real_luaC_endtask(L);
}

void __real_luaW_attach(lua_State *, LuaWorldScope);
void __wrap_luaW_attach(lua_State *L, LuaWorldScope scope) {
  struct fixture *f = fixture;
  if (f != NULL && f->hold_grant_attach) {
    LuaExecution *execution = luaQ_current();
    if (execution == &f->tasks[1].fiber.execution &&
        __atomic_load_n(&f->tasks[1].native, __ATOMIC_RELAXED) != 0) {
      require(pthread_mutex_lock(&f->mutex) == 0);
      f->grant_attach_seen = 1;
      require(pthread_cond_broadcast(&f->changed) == 0);
      while (!f->grant_attach_release)
        require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
      require(pthread_mutex_unlock(&f->mutex) == 0);
    }
  }
  __real_luaW_attach(L, scope);
}

int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t clock, struct timespec *time) {
  if (clock == CLOCK_MONOTONIC && fixture != NULL && luaQ_current() != NULL) {
    uint64_t ns = __atomic_load_n(&fixture->poll_time_ns, __ATOMIC_RELAXED);
    if (ns != 0) {
      time->tv_sec = (time_t)(ns / 1000000000);
      time->tv_nsec = (long)(ns % 1000000000);
      return 0;
    }
  }
  return __real_clock_gettime(clock, time);
}

void __real_luaQ_wait(lua_State *, pthread_mutex_t *, pthread_cond_t *,
                      LuaWaiter **);
void __wrap_luaQ_wait(lua_State *L, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters) {
  if (fixture != NULL && L == fixture->metadata_peer &&
      mutex == &G(L)->world.mutex) {
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->metadata_queued = 1;
    luaQ_wake(&fixture->changed, &fixture->waiters);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
  }
  __real_luaQ_wait(L, mutex, changed, waiters);
}

int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __wrap_pthread_cond_wait(pthread_cond_t *condition,
                             pthread_mutex_t *mutex) {
  if (observe_idle && mutex == &fixture->scheduler.mutex &&
      !pthread_equal(pthread_self(), idle_host)) {
    require(pthread_mutex_lock(&fixture->idle_mutex) == 0);
    fixture->idle_entries++;
    if (condition == &fixture->scheduler.changed)
      fixture->host_channel_entries++;
    require(pthread_cond_broadcast(&fixture->idle_changed) == 0);
    require(pthread_mutex_unlock(&fixture->idle_mutex) == 0);
  }
  return __real_pthread_cond_wait(condition, mutex);
}

void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__wrap_mmap(void *address, size_t size, int protection, int flags, int fd,
                  off_t offset) {
  void *mapping;
  if (fail_mapping) {
    fail_mapping = 0;
    errno = ENOMEM;
    return MAP_FAILED;
  }
  mapping = __real_mmap(address, size, protection, flags, fd, offset);
  if (mapping != MAP_FAILED)
    __atomic_fetch_add(&live_stacks, 1, __ATOMIC_RELAXED);
  return mapping;
}
int __real_mprotect(void *, size_t, int);
int __wrap_mprotect(void *address, size_t size, int protection) {
  if (fail_protection) {
    fail_protection = 0;
    errno = ENOMEM;
    return -1;
  }
  return __real_mprotect(address, size, protection);
}
int __real_munmap(void *, size_t);
int __wrap_munmap(void *address, size_t size) {
  int status = __real_munmap(address, size);
  if (status == 0)
    __atomic_fetch_sub(&live_stacks, 1, __ATOMIC_RELAXED);
  return status;
}

static int fail_at(int *counter) {
  if (*counter < 0)
    return 0;
  if (*counter == 0) {
    *counter = -1;
    return 1;
  }
  --*counter;
  return 0;
}
int __real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int __wrap_pthread_mutex_init(pthread_mutex_t *mutex,
                              const pthread_mutexattr_t *attributes) {
  return fail_at(&fail_mutex) ? EAGAIN
                              : __real_pthread_mutex_init(mutex, attributes);
}
int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int __wrap_pthread_cond_init(pthread_cond_t *condition,
                             const pthread_condattr_t *attributes) {
  return fail_at(&fail_condition)
             ? EAGAIN
             : __real_pthread_cond_init(condition, attributes);
}
int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*entry)(void *), void *argument) {
  return fail_at(&fail_carrier)
             ? EAGAIN
             : __real_pthread_create(thread, attributes, entry, argument);
}

int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  struct fixture *f = fixture;
  if (inspecting)
    __atomic_fetch_add(&poll_locks, 1, __ATOMIC_RELAXED);
  if (measuring_scheduler) {
    if (mutex == &f->scheduler.mutex)
      engine_locks++;
    for (unsigned i = 0; i < f->scheduler.config.slots; i++)
      if (mutex == &f->scheduler.slots[i].mutex)
        queue_locks++;
  }
  if (f != NULL && f->admission_pause && mutex == &f->scheduler.native_mutex &&
      luaQ_current() == &f->tasks[1].fiber.execution) {
    require(__real_pthread_mutex_lock(&f->mutex) == 0);
    f->admission_paused = 1;
    require(pthread_cond_broadcast(&f->changed) == 0);
    while (!f->admission_release)
      require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  return __real_pthread_mutex_lock(mutex);
}

void __real_luaQ_wakewaiter(LuaWaiter *);
void __wrap_luaQ_wakewaiter(LuaWaiter *waiter) {
  struct fixture *f = fixture;
  if (f->cancel_grant && waiter->execution == &f->tasks[1].fiber.execution) {
    require(f->tasks[1].native);
    require(pthread_mutex_lock(&f->mutex) == 0);
    f->admission_paused = 1;
    require(pthread_cond_broadcast(&f->changed) == 0);
    while (!f->admission_release)
      require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  __real_luaQ_wakewaiter(waiter);
}
void __real_luaX_poll(lua_State *);
void __wrap_luaX_poll(lua_State *L) {
  LuaExecution *execution = luaQ_current();
  unsigned lease_poll = 0;
  if (fixture->native_lease_test && execution != NULL &&
      L == fixture->tasks[0].root.thread && fixture->seen[0] && isLua(L->ci) &&
      execution->native_depth == 0 && fixture->native_lease_polls < 2) {
    lease_poll = ++fixture->native_lease_polls;
    if (fixture->native_lease_test == 1 && lease_poll == 2)
      __atomic_fetch_add(&fixture->poll_time_ns, 2000000, __ATOMIC_RELAXED);
  }
  if (fixture->inspect_polls && execution != NULL) {
    inspecting = 1;
    __atomic_fetch_add(&poll_calls, 1, __ATOMIC_RELAXED);
    __real_luaX_poll(L);
    inspecting = 0;
  } else
    __real_luaX_poll(L);
  if (lease_poll != 0) {
    LuaScheduler *s = &fixture->scheduler;
    require(pthread_mutex_lock(&s->native_mutex) == 0);
    require((s->native_waiters.head != NULL) == (lease_poll == 1));
    require(pthread_mutex_unlock(&s->native_mutex) == 0);
  }
  if (fixture->metadata_peer != NULL && L == fixture->tasks[0].root.thread) {
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    if (fixture->metadata_released && !fixture->pause_release) {
      /* The first handoff poll ran while the recipient's context was still
       * unpublished. Queue that recipient before allowing Lua to continue. */
      LuaScheduler *s = &fixture->scheduler;
      require(fixture->pause_seen);
      fixture->pause_release = 1;
      require(pthread_cond_broadcast(&fixture->changed) == 0);
      require(pthread_mutex_unlock(&fixture->mutex) == 0);
      require(pthread_mutex_lock(&s->mutex) == 0);
      while (__atomic_load_n(&s->runnable, __ATOMIC_ACQUIRE) == 0)
        require(pthread_cond_wait(&s->changed, &s->mutex) == 0);
      require(pthread_mutex_unlock(&s->mutex) == 0);
    } else
      require(pthread_mutex_unlock(&fixture->mutex) == 0);
  }
}
void __real_luaCT_pause(LuaFiber *);
void __wrap_luaCT_pause(LuaFiber *fiber) {
  struct fixture *f = fixture;
  LuaScheduledTask *task = fiber->execution.context;
  if (task->waiter != NULL &&
      ((f->pause_hold &&
        (!f->admission_fifo ||
         task->waiter->queue == &f->scheduler.native_waiters)) ||
       task->root.thread == f->metadata_peer)) {
    require(pthread_mutex_lock(&f->mutex) == 0);
    if (!f->pause_seen) {
      f->pause_seen = 1;
      require(pthread_cond_broadcast(&f->changed) == 0);
      while (!f->pause_release)
        require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
    }
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  __real_luaCT_pause(fiber);
}

static void initlimits(struct fixture *f, unsigned slots, unsigned native) {
  LuaSchedulerConfig config = {slots, slots + native, native, 1, 1};
  fixture = f;
  f->main = luaL_newstate();
  require(f->main != NULL);
  luaL_openlibs(f->main);
  require(pthread_mutex_init(&f->mutex, NULL) == 0);
  require(pthread_cond_init(&f->changed, NULL) == 0);
  if (observe_idle) {
    require(pthread_mutex_init(&f->idle_mutex, NULL) == 0);
    require(pthread_cond_init(&f->idle_changed, NULL) == 0);
  }
  require(luaSC_init(f->main, &f->scheduler, &config) == 0);
  require(f->scheduler.created == config.carriers);
  if (slots == 2) {
    f->parallel_check = 1;
    require(pthread_barrier_init(&f->parallel, NULL, 2) == 0);
  }
}

static void init(struct fixture *f, unsigned slots) { initlimits(f, slots, 1); }

static void finish(struct fixture *f) {
  require(luaSC_destroy(f->main, &f->scheduler) == 0);
  require(f->main->l_G->slot_caches == NULL);
  lua_close(f->main);
  if (observe_idle) {
    require(pthread_cond_destroy(&f->idle_changed) == 0);
    require(pthread_mutex_destroy(&f->idle_mutex) == 0);
  }
  require(pthread_cond_destroy(&f->changed) == 0);
  require(pthread_mutex_destroy(&f->mutex) == 0);
  if (f->parallel_check)
    require(pthread_barrier_destroy(&f->parallel) == 0);
}

static void managed(lua_State *L, const char *name, lua_CFunction function) {
  luaQ_pushmanaged(L, function, 0);
  lua_setglobal(L, name);
}

static void submit(struct fixture *f, unsigned i, const char *source, int id) {
  lua_State *L = lua_newthread(f->main);
  require(luaL_loadstring(L, source) == LUA_OK);
  lua_pushinteger(L, id);
  require(luaSC_submit(&f->scheduler, &f->tasks[i], L, 1) == 0);
  lua_pop(f->main, 1); /* only the admitted root retains the task */
}

static lua_Integer collect(struct fixture *f, unsigned i, int expected) {
  int status = -9, results = -9;
  LuaScheduledTask *task = &f->tasks[i];
  lua_Integer value;
  luaSC_wait(task);
  require(task->slot == NULL && task->fiber.execution.allocation_cache == NULL);
  require(task->fiber.execution.managed_depth == 0);
  require(task->fiber.execution.native_depth == 0);
  require(luaSC_collect(f->main, task, &status, &results) == LUA_OK);
  require(status == expected && results == 1);
  value = lua_tointeger(f->main, -1);
  lua_pop(f->main, 1);
  require(luaSC_collect(f->main, task, &status, &results) == LUA_TK_COLLECTED);
  return value;
}

static int metadata_owner(lua_State *L) {
  struct fixture *f = fixture;
  lua_lock(L);
  luaW_heapenter(L);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->metadata_held = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  /* Keep ownership while the peer queues; this controlled internal wait
   * releases the sole execution slot without running a GC-dependent action. */
  while (!f->metadata_queued)
    luaQ_wait(L, &f->mutex, &f->changed, &f->waiters);
  while (!f->pause_seen)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  luaW_heapleave(L);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->metadata_released = 1;
  require(pthread_mutex_unlock(&f->mutex) == 0);
  lua_unlock(L);
  return 0;
}

static int metadata_need(lua_State *L) {
  lua_lock(L);
  luaW_heapenter(L);
  luaW_heapleave(L);
  lua_unlock(L);
  return 0;
}

static void metadata_progress(struct fixture *f, unsigned quantum_polls) {
  lua_State *owner, *peer;
  if (quantum_polls == 0)
    f->poll_time_ns = 1; /* require a requested handoff */
  init(f, 1);
  f->scheduler.config.quantum_polls = quantum_polls;
  managed(f->main, "metadata_owner", metadata_owner);
  managed(f->main, "metadata_need", metadata_need);
  lua_pushboolean(f->main, 0);
  lua_setglobal(f->main, "metadata_ready");
  owner = lua_newthread(f->main);
  peer = lua_newthread(f->main);
  require(luaL_loadstring(
              owner, "metadata_owner(); "
                     "for i=1,2000000 do "
                     "if metadata_ready then return 1 end end; "
                     "error('metadata recipient did not progress')") == LUA_OK);
  require(
      luaL_loadstring(peer, "metadata_need(); metadata_ready=true; return 2") ==
      LUA_OK);
  f->metadata_peer = peer;
  lua_gc(f->main, LUA_GCSTOP);
  require(luaSC_submit(&f->scheduler, &f->tasks[0], owner, 0) == 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->metadata_held)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(luaSC_submit(&f->scheduler, &f->tasks[1], peer, 0) == 0);
  require(collect(f, 0, LUA_OK) == 1);
  require(collect(f, 1, LUA_OK) == 2);
  f->metadata_peer = NULL;
  finish(f);
}

static int started(lua_State *L) {
  unsigned id = (unsigned)lua_tointeger(L, 1);
  LuaExecution *execution = luaQ_current();
  LuaScheduledTask *task = execution->context;
  require(id < TASKS && task->slot != NULL);
  require(execution->allocation_cache == &task->slot->allocation);
  require(__atomic_fetch_add(&fixture->seen[id], 1, __ATOMIC_RELAXED) == 0);
  __atomic_fetch_add(&fixture->started, 1, __ATOMIC_RELEASE);
  if (fixture->parallel_check && id < 2) {
    LuaWorldScope world = luaW_suspendapi();
    int status = pthread_barrier_wait(&fixture->parallel);
    require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
    luaW_restoreapi(world);
  }
  return 0;
}

static int all_started(lua_State *L) {
  lua_pushboolean(L, __atomic_load_n(&fixture->started, __ATOMIC_ACQUIRE) ==
                         TASKS);
  return 1;
}

static int finalize(lua_State *L) {
  (void)L;
  __atomic_fetch_add(&fixture->finalized, 1, __ATOMIC_RELAXED);
  return 0;
}

static int mark_running(lua_State *L) {
  struct fixture *f = fixture;
  (void)L;
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->waiting = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  return 0;
}

enum { SLICE_PEERS = 3 };

static int first_slice_peer(lua_State *L) {
  LuaScheduler *scheduler = &fixture->scheduler;
  /* A spare carrier may take this task before the interrupted fiber is
   * saved. Keep the other two peers queued until that fiber is published. */
  require(pthread_mutex_lock(&scheduler->mutex) == 0);
  while (__atomic_load_n(&scheduler->runnable, __ATOMIC_ACQUIRE) < SLICE_PEERS)
    require(pthread_cond_wait(&scheduler->changed, &scheduler->mutex) == 0);
  require(pthread_mutex_unlock(&scheduler->mutex) == 0);
  return started(L);
}

static int poll_slice(lua_State *L) {
  struct fixture *f = fixture;
  uint64_t slice_ns = (uint64_t)luaL_checkinteger(L, 1);
  lua_lock(L);
  while (__atomic_load_n(&f->scheduler.runnable, __ATOMIC_ACQUIRE) <
         SLICE_PEERS)
    luaX_pollrequests(L);
  /* Repeated polls keep the current task until its slice expires. */
  for (unsigned i = 0; i < 16; i++)
    luaQ_poll(L);
  __atomic_fetch_add(&f->poll_time_ns, slice_ns - 1, __ATOMIC_RELAXED);
  luaQ_poll(L);
  require(__atomic_load_n(&f->started, __ATOMIC_ACQUIRE) == 0);
  __atomic_fetch_add(&f->poll_time_ns, 1, __ATOMIC_RELAXED);
  luaQ_poll(L);
  /* The queue head stays ahead at publication. If a spare carrier already
   * took peer 1, peer 2 can also run ahead. Later slices rotate behind all
   * three peers. */
  require(__atomic_load_n(&f->seen[1], __ATOMIC_ACQUIRE) == 1);
  require(__atomic_load_n(&f->seen[SLICE_PEERS], __ATOMIC_ACQUIRE) ==
          (slice_ns == 250000 ? 0u : 1u));
  lua_unlock(L);
  return 0;
}

static void timed_slice(struct fixture *f, int continuation) {
  f->poll_time_ns = 1;
  init(f, 1);
  f->scheduler.config.quantum_polls = 0;
  lua_gc(f->main, LUA_GCSTOP);
  managed(f->main, "poll_slice", poll_slice);
  managed(f->main, "first_slice_peer", first_slice_peer);
  managed(f->main, "started", started);
  managed(f->main, "mark_running", mark_running);
  submit(f, 0,
         continuation
             ? "coroutine.yield(); mark_running(); poll_slice(...); return 10"
             : "mark_running(); poll_slice(...); return 10",
         continuation ? 10000000 : 250000);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->waiting == 0)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "first_slice_peer(...); return ...", 1);
  for (unsigned i = 2; i <= SLICE_PEERS; i++)
    submit(f, i, "started(...); return ...", (int)i);
  require(collect(f, 0, LUA_OK) == 10);
  for (unsigned i = 1; i <= SLICE_PEERS; i++)
    require(collect(f, i, LUA_OK) == (lua_Integer)i);
  finish(f);
}

static void fairness(struct fixture *f, unsigned slots, int generational) {
  unsigned i, handoffs = 0;
  init(f, slots);
  if (generational)
    lua_gc(f->main, LUA_GCGEN);
  managed(f->main, "started", started);
  managed(f->main, "all_started", all_started);
  managed(f->main, "finalize", finalize);
  for (i = 0; i < TASKS; i++)
    submit(
        f, i,
        "local id=...; started(id); local t=setmetatable({}, {__gc=finalize}); "
        "local n=0; local function step() n=n+1; t[n%23]=n end; "
        "repeat for i=1,100 do step() end until all_started(); "
        "for i=1,200 do step() end; if id==11 then error(t) end; return "
        "id+1000",
        (int)i);
  for (i = 0; i < TASKS; i++) {
    if (i == 11)
      (void)collect(f, i, LUA_ERRRUN);
    else
      require(collect(f, i, LUA_OK) == (lua_Integer)i + 1000);
    handoffs += f->tasks[i].handoffs;
  }
  require(f->started == TASKS && handoffs != 0);
  require(f->scheduler.submitted == TASKS && f->scheduler.outstanding == 0);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(f->finalized == TASKS);
  finish(f);
}

static void no_requests(struct fixture *f) {
  init(f, 1);
  /* Measure idle polling without an outstanding collector-assistance job. */
  lua_gc(f->main, LUA_GCCOLLECT);
  lua_gc(f->main, LUA_GCSTOP);
  f->inspect_polls = 1;
  submit(f, 0, "local n=0; for i=1,10000 do n=n+i end; return n", 0);
  require(collect(f, 0, LUA_OK) == 50005000);
  require(poll_calls > 10 && poll_locks == 0);
  require(f->tasks[0].handoffs == 0);
  finish(f);
}

static int event_wait(lua_State *L) {
  struct fixture *f = fixture;
  LuaWorldScope world = luaW_suspendapi();
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->waiting++;
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->native_release) {
    if (luaQ_waitstop(L, &f->mutex, &f->changed, &f->waiters)) {
      require(pthread_mutex_unlock(&f->mutex) == 0);
      lua_lock(L);
      luaTK_checkstop(L);
      abort();
    }
  }
  require(pthread_mutex_unlock(&f->mutex) == 0);
  luaW_restoreapi(world);
  lua_pushinteger(L, 77);
  return 1;
}

static int local_api(lua_State *L) {
  pthread_t carrier = pthread_self();
  lua_Integer sum = 0;
  unsigned i;
  engine_locks = 0;
  measuring_scheduler = 1;
  for (i = 0; i < 2000; i++) {
    lua_pushinteger(L, (lua_Integer)i);
    sum += lua_tointeger(L, -1);
    lua_settop(L, 0);
  }
  measuring_scheduler = 0;
  require(sum == 1999000 && pthread_equal(pthread_self(), carrier));
  require(engine_locks == 0);
  lua_pushinteger(L, sum);
  return 1;
}

static void native_reentry(struct fixture *f) {
  init(f, 1);
  lua_pushcfunction(f->main, local_api);
  lua_setglobal(f->main, "local_api");
  require(pthread_mutex_lock(&f->scheduler.native_mutex) == 0);
  submit(f, 0, "return local_api()", 0);
  luaSC_wait(&f->tasks[0]);
  require(pthread_mutex_unlock(&f->scheduler.native_mutex) == 0);
  require(collect(f, 0, LUA_OK) == 1999000);
  require(f->scheduler.native_peak == 1 && f->scheduler.native == 0);
  require(f->scheduler.reentry_waiting == 0);
  finish(f);
}

static int pinned_wait(lua_State *L) {
  LuaCarrierPin pin = {0};
  pthread_t carrier = pthread_self();
  int results;
  require(luaQ_pincarrier(&pin) == LUA_OK);
  results = event_wait(L);
  require(luaQ_pinowner(&pin) && pthread_equal(pthread_self(), carrier));
  luaQ_unpincarrier(&pin);
  return results;
}

static void wake_before_publication(struct fixture *f, int generational) {
  init(f, 1);
  if (generational)
    lua_gc(f->main, LUA_GCGEN);
  managed(f->main, "event_wait", event_wait);
  managed(f->main, "finalize", finalize);
  f->pause_hold = 1;
  submit(f, 0,
         "local t=setmetatable({value=42}, {__gc=finalize}); "
         "local r=event_wait(); return t.value+r",
         0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->pause_seen)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(f->finalized == 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  luaQ_wake(&f->changed, &f->waiters);
  f->pause_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 119);
  require(f->tasks[0].early_wakes == 1 && f->tasks[0].parks == 1);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(f->finalized == 1);
  finish(f);
}

static void wake_waiters(struct fixture *f) {
  enum { WAITING_TASKS = 7 };
  LuaScheduler *s = &f->scheduler;
  init(f, 2);
  lua_gc(f->main, LUA_GCSTOP);
  managed(f->main, "event_wait", event_wait);
  managed(f->main, "pinned_wait", pinned_wait);
  f->pause_hold = 1;
  submit(f, 0, "return event_wait()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->pause_seen)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  for (unsigned i = 1; i < WAITING_TASKS; i++)
    submit(f, i,
           i == WAITING_TASKS - 1 ? "return pinned_wait()"
                                  : "return event_wait()",
           0);
  require(pthread_mutex_lock(&s->mutex) == 0);
  for (unsigned i = 1; i < WAITING_TASKS; i++)
    while (!f->tasks[i].published)
      require(pthread_cond_wait(&s->changed, &s->mutex) == 0);
  require(!f->tasks[0].published);
  require(pthread_mutex_unlock(&s->mutex) == 0);

  require(pthread_mutex_lock(&f->mutex) == 0);
  require(f->waiting == WAITING_TASKS);
  f->native_release = 1;
  engine_locks = queue_locks = 0;
  measuring_scheduler = 1;
  luaQ_wake(&f->changed, &f->waiters);
  measuring_scheduler = 0;
  require(engine_locks == 1 && queue_locks == 2 && f->waiters == NULL);
  f->pause_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);

  for (unsigned i = 0; i < WAITING_TASKS; i++) {
    require(collect(f, i, LUA_OK) == 77);
    require(f->tasks[i].early_wakes == (i == 0) && f->tasks[i].parks == 1);
  }
  require(s->pending_ready == 0);
  finish(f);
}

static int park_at_stop(lua_State *L) {
  LuaWorld *world = &G(L)->world;
  mark_running(L);
  while (!luaW_stopped(world))
    sched_yield();
  return event_wait(L);
}

static void stopped_dispatch(struct fixture *f) {
  LuaScheduler *s = &f->scheduler;
  init(f, 1);
  lua_gc(f->main, LUA_GCSTOP);
  managed(f->main, "park_at_stop", park_at_stop);
  submit(f, 0, "park_at_stop(); return 42", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->waiting)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "return 43", 0);
  lua_lock(f->main);
  luaW_stop(f->main);
  for (;;) {
    require(pthread_mutex_lock(&s->mutex) == 0);
    if (f->tasks[0].published && s->idle != NULL) {
      /* The dispatcher has had a chance to run the queued task. */
      require(f->tasks[1].dispatches == 0);
      require(pthread_mutex_unlock(&s->mutex) == 0);
      break;
    }
    require(pthread_mutex_unlock(&s->mutex) == 0);
    sched_yield();
  }
  luaW_start(f->main);
  lua_unlock(f->main);
  /* The existing task is parked on its own resource. Only reopening the
   * world can dispatch the fresh task. */
  require(collect(f, 1, LUA_OK) == 43);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  luaQ_wake(&f->changed, &f->waiters);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 42);
  finish(f);
}

static int block_binding(lua_State *L) {
  struct fixture *f = fixture;
  pthread_t carrier = pthread_self();
  lua_Integer id = f->admission_fifo ? lua_tointeger(L, 1) : 0;
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_entered++;
  if (f->admission_fifo) {
    require(id >= 0 && id < TASKS);
    f->seen[id] = f->native_entered;
  }
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->native_release)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(pthread_equal(carrier, pthread_self()));
  /* Synchronous API reentry happens at native saturation on this carrier. */
  {
    int status =
        luaL_loadstring(L, "local n=0; for i=1,1000 do n=n+i end; return n");
    if (status == LUA_OK)
      status = lua_pcall(L, 0, 1, 0);
    require(status == (f->shutting_down ? LUA_ERRRUN : LUA_OK));
  }
  __atomic_store_n(&f->native_callback_done, 1, __ATOMIC_RELEASE);
  require(pthread_equal(carrier, pthread_self()));
  return 1;
}

static void native_progress(struct fixture *f, int shutdown) {
  unsigned i;
  init(f, 1);
  lua_pushcfunction(f->main, block_binding); /* unchanged foreign binding */
  lua_setglobal(f->main, "block_binding");
  submit(f, 0, "return block_binding()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  for (i = 1; i < 5; i++)
    submit(f, i, "local t={}; for i=1,500 do t[i]=i end; return t[500]",
           (int)i);
  for (i = 1; i < 5; i++)
    require(collect(f, i, LUA_OK) == 500);
  require(!f->tasks[0].complete && f->scheduler.native_peak == 1);
  if (shutdown) {
    f->shutting_down = 1;
    require(luaSC_shutdown(&f->scheduler) == LUA_TK_PENDING);
    require(luaSC_shutdown(&f->scheduler) == LUA_TK_PENDING);
    require(!f->tasks[0].complete);
    require(luaSC_destroy(f->main, &f->scheduler) == EBUSY);
    {
      lua_State *L = lua_newthread(f->main);
      require(luaL_loadstring(L, "return 42") == LUA_OK);
      require(luaSC_submit(&f->scheduler, &f->tasks[5], L, 0) == ESHUTDOWN);
      require(f->tasks[5].root.phase == LUA_TASK_UNUSED);
      lua_pop(f->main, 1);
    }
  }
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  (void)collect(f, 0, shutdown ? LUA_ERRRUN : LUA_OK);
  require(f->scheduler.native == 0);
  if (shutdown)
    require(luaSC_shutdown(&f->scheduler) == LUA_OK);
  finish(f);
}

static void parallel_native_compensation(struct fixture *f) {
  initlimits(f, 2, 2);
  lua_gc(f->main, LUA_GCSTOP);
  managed(f->main, "started", started);
  lua_pushcfunction(f->main, block_binding);
  lua_setglobal(f->main, "block_binding");
  submit(f, 0, "return block_binding()", 0);
  submit(f, 1, "return block_binding()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->native_entered != 2)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);

  /* Both managed executions must reach the barrier before either C call
   * returns. Progress in only one reclaimed slot cannot pass this test. */
  submit(f, 2, "started(...); return 42", 0);
  submit(f, 3, "started(...); return 43", 1);
  require(collect(f, 2, LUA_OK) == 42);
  require(collect(f, 3, LUA_OK) == 43);
  require(luaSC_nativeactive(&f->scheduler) == 2);

  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 500500);
  require(collect(f, 1, LUA_OK) == 500500);
  finish(f);
}

static int independently_blocked(lua_State *L) {
  struct fixture *f = fixture;
  unsigned id;
  require(pthread_mutex_lock(&f->mutex) == 0);
  id = f->native_entered++;
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!(f->native_release & (1 << id)))
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  /* The native caller may have lost its last slot. Allocating must obtain
   * a current cache borrow before it can return to the interpreter. */
  lua_newtable(L);
  lua_pushinteger(L, 50 + id);
  lua_setfield(L, -2, "value");
  return 1;
}

static int wait_for_native_return(lua_State *L) {
  LuaScheduler *s = &fixture->scheduler;
  mark_running(L);
  while (__atomic_load_n(&s->reentry_waiting, __ATOMIC_ACQUIRE) == 0)
    sched_yield();
  require(pthread_mutex_lock(&s->mutex) == 0);
  require(s->reentry_waiting == 1);
  require(pthread_mutex_unlock(&s->mutex) == 0);
  return 0;
}

static void native_return_compensation(struct fixture *f, int handoff) {
  unsigned i;
  initlimits(f, 1, 2);
  lua_gc(f->main, LUA_GCSTOP);
  if (handoff)
    managed(f->main, "wait_for_native_return", wait_for_native_return);
  lua_pushcfunction(f->main, independently_blocked);
  lua_setglobal(f->main, "independently_blocked");
  for (i = 0; i < 2; i++) {
    submit(
        f, i,
        handoff && i == 1
            ? "wait_for_native_return(); return independently_blocked().value"
            : "return independently_blocked().value",
        0);
    require(pthread_mutex_lock(&f->mutex) == 0);
    if (handoff && i == 1) {
      while (!f->waiting)
        require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
      /* The second task holds the slot until the first has blocked on its
       * native return. Entering C then has to notify the first task. */
      f->native_release = 1;
      require(pthread_cond_broadcast(&f->changed) == 0);
    }
    while (f->native_entered != i + 1)
      require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  /* The second call took the sole slot to enter C. The first return must
   * proceed now, with no queued Lua work and the second still blocked. */
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 50);
  require(luaSC_nativeactive(&f->scheduler) == 1);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 3;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 1, LUA_OK) == 51);
  finish(f);
}

static int blocked_leaf(lua_State *L) {
  struct fixture *f = fixture;
  (void)L;
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_entered = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->native_release)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  return 0;
}

static int inspect_leaf_return(lua_State *L) {
  struct fixture *f = fixture;
  lua_State *target = f->tasks[0].root.thread;
  int reservation;
  mark_running(L);
  while (__atomic_load_n(&f->scheduler.reentry_waiting, __ATOMIC_ACQUIRE) == 0)
    sched_yield();
  /* The worker holds the leaf's old slot. The return must leave its stack
   * available for root scanning while it waits for managed capacity. */
  reservation = luaX_tryreserve(target);
  require(reservation == 1);
  luaX_endreserve(target, reservation);
  return 0;
}

static void native_leaf_return(struct fixture *f) {
  init(f, 1);
  lua_gc(f->main, LUA_GCSTOP);
  managed(f->main, "inspect_leaf_return", inspect_leaf_return);
  lua_pushcfunction(f->main, blocked_leaf);
  lua_setglobal(f->main, "blocked_leaf");
  submit(f, 0, "blocked_leaf(); return 51", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "inspect_leaf_return(); return 52", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->waiting)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 51);
  require(collect(f, 1, LUA_OK) == 52);
  finish(f);
}

static int warm_native_permit(lua_State *L) {
  (void)L;
  return 0;
}

static void native_recall_during_stack_borrow(struct fixture *f) {
  lua_State *target;
  int reservation;
  init(f, 1);
  lua_pushcfunction(f->main, warm_native_permit);
  lua_setglobal(f->main, "warm_native_permit");
  lua_pushcfunction(f->main, blocked_leaf);
  lua_setglobal(f->main, "blocked_leaf");
  submit(f, 0,
         "local value={answer=51}; warm_native_permit(); blocked_leaf(value); "
         "return value.answer",
         0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  target = f->tasks[0].root.thread;
  require(pthread_mutex_unlock(&f->mutex) == 0);

  reservation = luaX_tryreserve(target);
  require(reservation == 1);
  require(lua_gc(f->main, LUA_GCCOLLECT) == 0);
  /* The independent task can use the sole slot while this stack stays held. */
  submit(f, 1, "return 52", 0);
  require(collect(f, 1, LUA_OK) == 52);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  while (!(__atomic_load_n(&target->stack_gate.word, __ATOMIC_ACQUIRE) &
           LUA_GATE_WAITING))
    sched_yield();
  luaX_endreserve(target, reservation);
  require(collect(f, 0, LUA_OK) == 51);
  finish(f);
}

static void voluntary(struct fixture *f) {
  init(f, 1);
  submit(f, 0, "for i=1,5 do coroutine.yield(i) end; return 42", 0);
  require(collect(f, 0, LUA_OK) == 42);
  require(f->tasks[0].dispatches >= 6);
  finish(f);
}

static void parked_wait(struct fixture *f, int cancel) {
  LuaScheduler *s;
  init(f, 1);
  s = &f->scheduler;
  managed(f->main, "event_wait", event_wait);
  submit(f, 0, "return event_wait()", 0);
  require(pthread_mutex_lock(&s->mutex) == 0);
  while (!f->tasks[0].published)
    require(pthread_cond_wait(&s->changed, &s->mutex) == 0);
  require(f->tasks[0].slot == NULL && f->tasks[0].waiter != NULL);
  require(pthread_mutex_unlock(&s->mutex) == 0);
  submit(f, 1, "return 42", 0);
  require(collect(f, 1, LUA_OK) == 42);
  lua_gc(f->main, LUA_GCCOLLECT);
  if (cancel)
    require(luaTK_requeststop(&f->tasks[0].root) == LUA_OK);
  else {
    require(pthread_mutex_lock(&f->mutex) == 0);
    f->native_release = 1;
    luaQ_wake(&f->changed, &f->waiters);
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  if (cancel)
    (void)collect(f, 0, LUA_ERRRUN);
  else
    require(collect(f, 0, LUA_OK) == 77);
  /* Cancellation may park again while its notifier borrow retires. */
  require(f->tasks[0].parks >= 1 && (cancel || f->tasks[0].parks == 1));
  require(f->tasks[0].root.stop_wait == NULL && f->tasks[0].waiter == NULL);
  require(f->waiters == NULL);
  finish(f);
}

static void native_saturation(struct fixture *f, int cancel, int early_return) {
  LuaScheduler *s;
  int status = -1, results = -1;
  f->admission_pause = early_return;
  /* A paused admission still has its managed slot until it parks. The
   * first task needs another slot to leave its native activation. */
  initlimits(f, early_return ? 2 : 1, 1);
  s = &f->scheduler;
  lua_pushcfunction(f->main, block_binding);
  lua_setglobal(f->main, "block_binding");
  submit(f, 0, "return block_binding()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->native_entered == 0)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "return block_binding()", 0);
  if (early_return) {
    require(pthread_mutex_lock(&f->mutex) == 0);
    while (!f->admission_paused)
      require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
    require(pthread_mutex_lock(&s->native_mutex) == 0);
    require(s->native_waiters.head == NULL);
    require(pthread_mutex_unlock(&s->native_mutex) == 0);
  } else {
    require(pthread_mutex_lock(&s->native_mutex) == 0);
    while (s->native_waiters.head == NULL) {
      require(pthread_mutex_unlock(&s->native_mutex) == 0);
      sched_yield();
      require(pthread_mutex_lock(&s->native_mutex) == 0);
    }
    require(luaSC_nativeactive(s) == 1);
    require(f->native_entered == 1);
    require(pthread_mutex_unlock(&s->native_mutex) == 0);
    submit(f, 2, "return 42", 0);
    require(collect(f, 2, LUA_OK) == 42);
  }
  if (cancel) {
    require(luaTK_requeststop(&f->tasks[1].root) == LUA_OK);
    (void)collect(f, 1, LUA_ERRRUN);
    require(pthread_mutex_lock(&s->native_mutex) == 0);
    require(s->native_waiters.head == NULL);
    require(pthread_mutex_unlock(&s->native_mutex) == 0);
    require(!f->tasks[0].complete && f->native_entered == 1);
  }
  require(luaSC_collect(f->main, &f->tasks[0], &status, &results) ==
          LUA_TK_PENDING);
  require(status == -1 && results == -1);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 500500);
  if (early_return) {
    /* The only release happened before this entrant could publish interest. */
    require(pthread_mutex_lock(&f->mutex) == 0);
    f->admission_release = 1;
    require(pthread_cond_broadcast(&f->changed) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  if (!cancel)
    require(collect(f, 1, LUA_OK) == 500500);
  require(f->native_entered == (cancel ? 1u : 2u));
  require(s->native_waiters.head == NULL && s->native == 0);
  finish(f);
}

static void wait_for_permit(struct fixture *f, unsigned id) {
  LuaScheduler *s = &f->scheduler;
  LuaExecution *execution = &f->tasks[id].fiber.execution;
  for (;;) {
    LuaWaiter *waiter;
    int found;
    require(pthread_mutex_lock(&s->native_mutex) == 0);
    for (waiter = s->native_waiters.head; waiter != NULL; waiter = waiter->next)
      if (waiter->execution == execution)
        break;
    found = waiter != NULL;
    require(pthread_mutex_unlock(&s->native_mutex) == 0);
    if (found)
      return;
    sched_yield();
  }
}

enum { FIFO_ADMISSION, CANCEL_GRANT, COMPETITIVE_ADMISSION };

static int hold_second_permit(lua_State *L) {
  struct fixture *f = fixture;
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->seen[5] = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->admission_release)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  lua_pushinteger(L, 66);
  return 1;
}

static int release_native_permit(lua_State *L) {
  LuaScheduledTask *task = luaQ_current()->context;
  /* A managed C poll ends reuse without yielding the task's slice. */
  task->polls = 2;
  luaQ_poll(L);
  return 0;
}

static void native_grant_stopped_world(struct fixture *f) {
  LuaScheduler *s = &f->scheduler;
  f->hold_grant_attach = 1;
  initlimits(f, 2, 1);
  lua_gc(f->main, LUA_GCSTOP);
  lua_pushcfunction(f->main, block_binding);
  lua_setglobal(f->main, "block_binding");
  managed(f->main, "release_native_permit", release_native_permit);
  submit(f, 0, "local n=block_binding(); release_native_permit(); return n", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "return block_binding()", 0);
  wait_for_permit(f, 1);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->grant_attach_seen)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 500500);

  /* Stop the world after admission grants the permit, before the binding
   * starts. The entrant must keep its grant while attachment parks. */
  lua_lock(f->main);
  luaW_stop(f->main);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->grant_attach_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(pthread_mutex_lock(&s->mutex) == 0);
  while (!f->tasks[1].published)
    require(pthread_cond_wait(&s->changed, &s->mutex) == 0);
  require(luaSC_nativeactive(s) == 1);
  require(pthread_mutex_unlock(&s->mutex) == 0);
  luaW_start(f->main);
  lua_unlock(f->main);

  require(collect(f, 1, LUA_OK) == 500500);
  require(f->native_entered == 2 && luaSC_nativeactive(s) == 0);
  finish(f);
}

static void native_fifo(struct fixture *f, int mode) {
  LuaScheduler *s = &f->scheduler;
  int cancel_grant = mode == CANCEL_GRANT;
  int competitive = mode == COMPETITIVE_ADMISSION;
  f->admission_fifo = 1;
  f->cancel_grant = cancel_grant;
  f->pause_hold = competitive;
  initlimits(f, 2, competitive ? 2 : 1);
  if (competitive) {
    lua_pushcfunction(f->main, hold_second_permit);
    lua_setglobal(f->main, "hold_second_permit");
    submit(f, 5, "return hold_second_permit()", 0);
    require(pthread_mutex_lock(&f->mutex) == 0);
    while (!f->seen[5])
      require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  lua_pushcfunction(f->main, block_binding);
  lua_setglobal(f->main, "block_binding");
  managed(f->main, "release_native_permit", release_native_permit);
  submit(f, 0,
         competitive ? "return block_binding(...)"
                     : "local result=block_binding(...); "
                       "release_native_permit(); return result",
         0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->native_entered == 0)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  for (unsigned i = 1; i <= 3; i++) {
    submit(f, i, "return block_binding(...)", (int)i);
    wait_for_permit(f, i);
  }
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (competitive && !f->pause_seen)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  if (cancel_grant) {
    while (!f->admission_paused)
      require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
    /* Cancel after selection and before the scheduler wake. The request
     * must finish even while the notifier is held at that boundary. */
    require(luaTK_requeststop(&f->tasks[1].root) == LUA_OK);
    require(pthread_mutex_lock(&f->mutex) == 0);
    require(f->seen[1] == 0);
    f->admission_release = 1;
    require(pthread_cond_broadcast(&f->changed) == 0);
  }
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 500500);
  if (competitive) {
    /* Task 1 was notified before saving its context. A running task can
     * claim that free permit, and returning it needs no second notice. */
    require(luaSC_nativeactive(s) == 1);
    submit(f, 4, "return block_binding(...)", 4);
    require(collect(f, 4, LUA_OK) == 500500 && f->seen[4] == 2);
    wait_for_permit(f, 2);
    wait_for_permit(f, 3);
    require(pthread_mutex_lock(&f->mutex) == 0);
    f->pause_release = 1;
    require(pthread_cond_broadcast(&f->changed) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  if (cancel_grant)
    (void)collect(f, 1, LUA_ERRRUN);
  else
    require(collect(f, 1, LUA_OK) == 500500);
  require(collect(f, 2, LUA_OK) == 500500);
  require(collect(f, 3, LUA_OK) == 500500);
  if (competitive) {
    require(pthread_mutex_lock(&f->mutex) == 0);
    f->admission_release = 1;
    require(pthread_cond_broadcast(&f->changed) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
    require(collect(f, 5, LUA_OK) == 66);
  }
  require(f->seen[0] == 1 &&
          f->seen[1] == (cancel_grant ? 0u : 2u + (unsigned)competitive));
  require(f->seen[2] == 3u + (unsigned)competitive - (unsigned)cancel_grant &&
          f->seen[3] == 4u + (unsigned)competitive - (unsigned)cancel_grant);
  require(s->native == 0 && s->native_waiters.head == NULL &&
          s->native_notices == 0 && s->native_peak == (competitive ? 2u : 1u));
  finish(f);
}

static int short_native_call(lua_State *L) {
  struct fixture *f = fixture;
  lua_Integer id = lua_tointeger(L, 1);
  require(id >= 0 && id < TASKS);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->seen[id] = ++f->native_entered;
  luaQ_wake(&f->changed, &f->waiters);
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->native_release)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  return 1;
}

static int wait_for_native_peer(lua_State *L) {
  struct fixture *f = fixture;
  LuaWorldScope depth = luaW_suspendapi();
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->seen[1])
    luaQ_wait(L, &f->mutex, &f->changed, &f->waiters);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  luaW_restoreapi(depth);
  lua_pushinteger(L, 77);
  return 1;
}

static int poll_for_native_peer(lua_State *L) {
  struct fixture *f = fixture;
  release_native_permit(L);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->seen[1])
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  lua_pushinteger(L, 77);
  return 1;
}

static void native_burst(struct fixture *f, int polling, unsigned slots) {
  LuaScheduler *s = &f->scheduler;
  initlimits(f, slots, 1);
  lua_gc(f->main, LUA_GCSTOP);
  managed(f->main, "wait_for_native_peer",
          polling ? poll_for_native_peer : wait_for_native_peer);
  lua_pushcfunction(f->main, short_native_call);
  lua_setglobal(f->main, "short_native_call");
  submit(f, 0,
         "local f=short_native_call; f(0); f(4); return wait_for_native_peer()",
         0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->seen[0])
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "return short_native_call(...)", 1);
  wait_for_permit(f, 1);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 77 && f->tasks[0].parks == !polling);
  require(collect(f, 1, LUA_OK) == 1);
  require(f->seen[0] == 1 && f->seen[4] == 2 && f->seen[1] == 3);
  require(s->native == 0 && s->native_waiters.head == NULL &&
          s->native_peak == 1);
  finish(f);
}

static void native_lease(struct fixture *f, int continuous, unsigned permits) {
  LuaScheduler *s = &f->scheduler;
  f->native_lease_test = continuous ? 1 : 2;
  f->poll_time_ns = 1;
  initlimits(f, 2, permits);
  lua_gc(f->main, LUA_GCSTOP);
  if (permits == 2) {
    lua_pushcfunction(f->main, hold_second_permit);
    lua_setglobal(f->main, "hold_second_permit");
    submit(f, 5, "return hold_second_permit()", 0);
    require(pthread_mutex_lock(&f->mutex) == 0);
    while (!f->seen[5])
      require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  lua_pushcfunction(f->main, short_native_call);
  lua_setglobal(f->main, "short_native_call");
  submit(f, 0,
         continuous ? "local f=short_native_call; for i=1,1500 do f(0) end; "
                      "return 42"
                    : "short_native_call(0); local n=0; for i=1,1500 do "
                      "n=n+i end; return n>0 and 42",
         0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->seen[0])
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "return short_native_call(...)", 1);
  wait_for_permit(f, 1);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 42 && f->native_lease_polls == 2);
  require(collect(f, 1, LUA_OK) == 1);
  if (permits == 2) {
    require(pthread_mutex_lock(&f->mutex) == 0);
    f->admission_release = 1;
    require(pthread_cond_broadcast(&f->changed) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
    require(collect(f, 5, LUA_OK) == 66);
  }
  require(s->native == 0 && s->native_waiters.head == NULL &&
          s->native_peak == permits);
  finish(f);
}

static int poll_after_native_waiter(lua_State *L) {
  struct fixture *f = fixture;
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->waiting = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->pause_release)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  return poll_for_native_peer(L);
}

static void late_native_waiter(struct fixture *f) {
  initlimits(f, 2, 2);
  lua_gc(f->main, LUA_GCSTOP);
  f->native_release = 1;
  managed(f->main, "poll_after_native_waiter", poll_after_native_waiter);
  lua_pushcfunction(f->main, hold_second_permit);
  lua_setglobal(f->main, "hold_second_permit");
  lua_pushcfunction(f->main, short_native_call);
  lua_setglobal(f->main, "short_native_call");
  submit(f, 5, "return hold_second_permit()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->seen[5])
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 0, "short_native_call(0); return poll_after_native_waiter()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->waiting)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);

  /* The first call returned before the new request. The other permit is
   * still occupied by a blocked call, so only the polling task can help. */
  require(luaSC_nativeactive(&f->scheduler) == 2);
  submit(f, 1, "return short_native_call(...)", 1);
  wait_for_permit(f, 1);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->pause_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 77);
  require(collect(f, 1, LUA_OK) == 1);
  require(luaSC_nativeactive(&f->scheduler) == 1);

  require(pthread_mutex_lock(&f->mutex) == 0);
  f->admission_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 5, LUA_OK) == 66);
  require(luaSC_nativeactive(&f->scheduler) == 0);
  finish(f);
}

static int callback_done(lua_State *L) {
  lua_pushboolean(
      L, __atomic_load_n(&fixture->native_callback_done, __ATOMIC_ACQUIRE));
  return 1;
}

static void callback_progress(struct fixture *f, int timed) {
  if (timed)
    f->poll_time_ns = 1; /* native return must progress with a frozen slice */
  init(f, 1);
  if (timed)
    f->scheduler.config.quantum_polls = 0;
  managed(f->main, "mark_running", mark_running);
  managed(f->main, "callback_done", callback_done);
  lua_pushcfunction(f->main, block_binding);
  lua_setglobal(f->main, "block_binding");
  submit(f, 0, "return block_binding()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->native_entered == 0)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1,
         "mark_running(); repeat local n=0; for i=1,100 do n=n+i end "
         "until callback_done(); return 42",
         0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->waiting == 0)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 1, LUA_OK) == 42);
  require(collect(f, 0, LUA_OK) == 500500);
  require(f->scheduler.reentry_waiting == 0);
  finish(f);
}

static int invoke_callback(lua_State *L) {
  struct fixture *f = fixture;
  int wait = lua_toboolean(L, 2);
  lua_settop(L, 1);
  if (wait) {
    require(pthread_mutex_lock(&f->mutex) == 0);
    f->native_entered++;
    require(pthread_cond_broadcast(&f->changed) == 0);
    while (!f->native_release)
      require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  lua_call(L, 0, 1);
  return 1;
}

static int inspect_waiting_callback(lua_State *L) {
  struct fixture *f = fixture;
  lua_State *target = f->tasks[1].root.thread;
  int reservation;
  mark_running(L);
  while (__atomic_load_n(&f->scheduler.reentry_waiting, __ATOMIC_ACQUIRE) != 1)
    sched_yield();
  /* The only slot is occupied. A callback waiting for it must leave its
   * stack available for root scanning. */
  reservation = luaX_tryreserve(target);
  require(reservation == 1);
  luaX_endreserve(target, reservation);
  return 0;
}

static void synchronous_callback(struct fixture *f) {
  init(f, 1);
  managed(f->main, "inspect_waiting_callback", inspect_waiting_callback);
  lua_pushcfunction(f->main, invoke_callback);
  lua_setglobal(f->main, "invoke_callback");
  const char *source = "return invoke_callback(function() "
                       "  local t = {}; for i = 1, 40 do t[i] = {i} end; "
                       "  collectgarbage('collect'); return t[40][1] + #t "
                       "end, ... ~= 0)";
  submit(f, 0, source, 0);
  require(collect(f, 0, LUA_OK) == 80);
  submit(f, 1, source, 1);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->native_entered == 0)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 2, "inspect_waiting_callback(); return 42", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->waiting == 0)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 2, LUA_OK) == 42);
  require(collect(f, 1, LUA_OK) == 80);
  finish(f);
}

static int hold_for_native_returns(lua_State *L) {
  mark_running(L);
  while (__atomic_load_n(&fixture->scheduler.reentry_waiting,
                         __ATOMIC_ACQUIRE) != 2)
    sched_yield();
  return 0;
}

static void simultaneous_reentry(struct fixture *f) {
  initlimits(f, 1, 2);
  managed(f->main, "hold_for_native_returns", hold_for_native_returns);
  lua_pushcfunction(f->main, block_binding);
  lua_setglobal(f->main, "block_binding");
  submit(f, 0, "return block_binding()", 0);
  submit(f, 1, "return block_binding()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->native_entered != 2)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);

  submit(f, 2, "hold_for_native_returns(); return 42", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->waiting)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 3, "return 43", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);

  require(collect(f, 0, LUA_OK) == 500500);
  require(collect(f, 1, LUA_OK) == 500500);
  require(collect(f, 2, LUA_OK) == 42);
  require(collect(f, 3, LUA_OK) == 43);
  require(f->scheduler.native_peak == 2 && f->scheduler.reentry_waiting == 0);
  finish(f);
}

static void *backing(void *context, void *block, size_t old, size_t size) {
  struct fixture *f = context;
  void *replacement;
  if (block == NULL)
    old = 0;
  if (size == 0) {
    free(block);
    f->live -= old;
    return NULL;
  }
  if (f->deny)
    return NULL;
  replacement = realloc(block, size);
  if (replacement != NULL)
    f->live = f->live - old + size;
  return replacement;
}

static void idle_notifications(struct fixture *f) {
  unsigned i;
  observe_idle = 1;
  idle_host = pthread_self();
  init(f, 4);
  require(pthread_mutex_lock(&f->idle_mutex) == 0);
  while (f->idle_entries < f->scheduler.created)
    require(pthread_cond_wait(&f->idle_changed, &f->idle_mutex) == 0);
  fprintf(stderr, "idle carriers on host notification channel: %u\n",
          f->host_channel_entries);
  require(f->host_channel_entries == 0);
  require(pthread_mutex_unlock(&f->idle_mutex) == 0);
  for (i = 0; i < TASKS; i++)
    submit(f, i, "local t={}; for i=1,300 do t[i]=i end; return t[300]", 0);
  for (i = 0; i < TASKS; i++)
    require(collect(f, i, LUA_OK) == 300);
  finish(f);
  observe_idle = 0;
}

static int current_carrier(lua_State *L) {
  LuaScheduledTask *task = luaQ_current()->context;
  lua_pushinteger(L, task->carrier - task->scheduler->carriers);
  return 1;
}

static void standby_carriers(struct fixture *f) {
  initlimits(f, 1, 3);
  lua_gc(f->main, LUA_GCSTOP);
  managed(f->main, "current_carrier", current_carrier);
  lua_pushcfunction(f->main, block_binding);
  lua_setglobal(f->main, "block_binding");

  submit(f, 0, "return current_carrier()", 0);
  require(collect(f, 0, LUA_OK) <= 1);
  for (unsigned i = 1; i <= 3; i++)
    submit(f, i, "return block_binding()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->native_entered != 3)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(luaSC_nativeactive(&f->scheduler) == 3);

  /* Only one of the four carrier threads can execute this task. */
  submit(f, 4, "current_carrier(); return 42", 0);
  require(collect(f, 4, LUA_OK) == 42);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  for (unsigned i = 1; i <= 3; i++)
    require(collect(f, i, LUA_OK) == 500500);
  require(luaSC_nativeactive(&f->scheduler) == 0);
  submit(f, 5, "return current_carrier()", 0);
  require(collect(f, 5, LUA_OK) <= 1);
  finish(f);
}

static void idle_affinity(struct fixture *f) {
  LuaScheduler *s = &f->scheduler;
  unsigned observed, i;
  observe_idle = 1;
  idle_host = pthread_self();
  init(f, 4);
  managed(f->main, "pinned_wait", pinned_wait);
  submit(f, 0, "return pinned_wait()", 0);
  require(pthread_mutex_lock(&s->mutex) == 0);
  while (!f->tasks[0].published)
    require(pthread_cond_wait(&s->changed, &s->mutex) == 0);
  require(pthread_mutex_unlock(&s->mutex) == 0);
  for (;;) {
    require(pthread_mutex_lock(&f->idle_mutex) == 0);
    observed = f->idle_entries;
    require(pthread_mutex_unlock(&f->idle_mutex) == 0);
    require(pthread_mutex_lock(&s->mutex) == 0);
    for (i = 0; i < s->config.carriers; i++)
      if (!s->carriers[i].sleeping)
        break;
    if (i == s->config.carriers) {
      LuaCarrier *required = f->tasks[0].carrier;
      LuaCarrier **link = &s->idle;
      /* Put every ineligible sleeper ahead of the required carrier. */
      while (*link != required)
        link = &(*link)->idle_next;
      *link = required->idle_next;
      required->idle_next = NULL;
      for (link = &s->idle; *link != NULL; link = &(*link)->idle_next) {
      }
      *link = required;
      require(s->idle != required && f->tasks[0].affinity == required);
      require(pthread_mutex_unlock(&s->mutex) == 0);
      break;
    }
    require(pthread_mutex_unlock(&s->mutex) == 0);
    require(pthread_mutex_lock(&f->idle_mutex) == 0);
    while (observed == f->idle_entries)
      require(pthread_cond_wait(&f->idle_changed, &f->idle_mutex) == 0);
    require(pthread_mutex_unlock(&f->idle_mutex) == 0);
  }
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  luaQ_wake(&f->changed, &f->waiters);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 77);
  finish(f);
  observe_idle = 0;
}

static void startup_failures(struct fixture *f) {
  LuaSchedulerConfig config = {2, 3, 1, 1, 1};
  LuaScheduler other = {0};
  unsigned i;
  fixture = f;
  f->main = lua_newstate(backing, f, 42);
  require(f->main != NULL);
  for (i = 0; i < 2 + config.slots; i++) {
    fail_mutex = (int)i;
    require(luaSC_init(f->main, &f->scheduler, &config) == EAGAIN);
    require(f->main->l_G->scheduler == NULL &&
            f->main->l_G->slot_caches == NULL);
  }
  for (i = 0; i < 2 + config.carriers; i++) {
    fail_condition = (int)i;
    require(luaSC_init(f->main, &f->scheduler, &config) == EAGAIN);
    require(f->main->l_G->scheduler == NULL &&
            f->main->l_G->slot_caches == NULL);
  }
  for (i = 0; i < 3; i++) {
    fail_carrier = (int)i;
    require(luaSC_init(f->main, &f->scheduler, &config) == EAGAIN);
    require(f->main->l_G->scheduler == NULL &&
            f->main->l_G->slot_caches == NULL);
  }
  f->deny = 1;
  config.slots = 16;
  config.carriers = 17;
  require(luaSC_init(f->main, &f->scheduler, &config) == ENOMEM);
  require(f->main->l_G->scheduler == NULL && f->main->l_G->slot_caches == NULL);
  f->deny = 0;
  config.slots = 1;
  config.carriers = 2;
  require(luaSC_init(f->main, &f->scheduler, &config) == 0);
  require(luaSC_init(f->main, &other, &config) == EBUSY);
  require(f->main->l_G->scheduler == &f->scheduler);
  {
    lua_State *L = lua_newthread(f->main);
    require(luaL_loadstring(L, "return 42") == LUA_OK);
    fail_mapping = 1;
    require(luaSC_submit(&f->scheduler, &f->tasks[0], L, 0) == ENOMEM);
    require(live_stacks == 0 && f->tasks[0].root.phase == LUA_TASK_UNUSED);
    fail_protection = 1;
    require(luaSC_submit(&f->scheduler, &f->tasks[0], L, 0) == ENOMEM);
    require(live_stacks == 0 && f->tasks[0].root.phase == LUA_TASK_UNUSED);
    lua_pop(f->main, 1);
  }
  submit(f, 0, "return 42", 0);
  require(collect(f, 0, LUA_OK) == 42);
  require(luaSC_destroy(f->main, &f->scheduler) == 0);
  lua_close(f->main);
  require(f->live == 0);
}

static void nursery_wait(lua_State *L, const char *name) {
  for (;;) {
    int ready;
    lua_getglobal(L, name);
    ready = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (ready)
      return;
    sched_yield();
  }
}

static int nursery_native(lua_State *L) {
  lua_pushvalue(L, 1);
  lua_setglobal(L, "nursery_native_root");
  return lua_getfield(L, 1, "answer") == LUA_TNUMBER;
}

static void nursery(struct fixture *f, int generational) {
  int status, results;
  init(f, 2);
  luaL_openlibs(f->main);
  lua_gc(f->main, generational ? LUA_GCGEN : LUA_GCINC);
  submit(f, 0,
         "local n=...; local name='nursery field '..n; "
         "local root={[name]=42}; root.self=root; name=nil; "
         "rawset(root,'values',{42}); "
         "for _,v in ipairs(rawget(root,'values')) do assert(v==42) end; "
         "for i=1,50000 do local row={i,i+1,kind='row'}; "
         "if i%4096==0 then collectgarbage('step',0) end; "
         "assert(row[2]==i+1) end; "
         "nursery_ready=true; while not nursery_release do end; "
         "return root.self['nursery field '..n]",
         17);
  nursery_wait(f->main, "nursery_ready");
  for (unsigned i = 0; i < 4; i++)
    lua_gc(f->main, LUA_GCSTEP, (size_t)0);
  lua_pushboolean(f->main, 1);
  lua_setglobal(f->main, "nursery_release");
  require(collect(f, 0, LUA_OK) == 42 && f->nursery_finished == 1);

  submit(f, 1,
         "local result={}; result.self=result; "
         "result.child={answer=42}; return result",
         0);
  luaSC_wait(&f->tasks[1]);
  require(luaSC_collect(f->main, &f->tasks[1], &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(lua_getfield(f->main, -1, "self") == LUA_TTABLE);
  require(lua_rawequal(f->main, -1, -2));
  require(lua_getfield(f->main, -1, "child") == LUA_TTABLE);
  require(lua_getfield(f->main, -1, "answer") == LUA_TNUMBER);
  require(lua_tointeger(f->main, -1) == 42);
  lua_pop(f->main, 4);

  submit(f, 2,
         "local root={child={answer=42}}; rawset(_G,'nursery_export',root); "
         "nursery_second_ready=true; while not nursery_second_release do end; "
         "return root.child.answer",
         0);
  nursery_wait(f->main, "nursery_second_ready");
  lua_gc(f->main, LUA_GCCOLLECT);
  require(lua_getglobal(f->main, "nursery_export") == LUA_TTABLE);
  require(lua_getfield(f->main, -1, "child") == LUA_TTABLE);
  require(lua_getfield(f->main, -1, "answer") == LUA_TNUMBER);
  require(lua_tointeger(f->main, -1) == 42);
  lua_pop(f->main, 3);
  lua_pushboolean(f->main, 1);
  lua_setglobal(f->main, "nursery_second_release");
  require(collect(f, 2, LUA_OK) == 42);

  submit(f, 3,
         "local root={answer=42}; "
         "nursery_third_ready=true; while not nursery_third_release do end; "
         "return root.answer",
         0);
  nursery_wait(f->main, "nursery_third_ready");
  lua_gc(f->main, LUA_GCCOLLECT);
  lua_pushboolean(f->main, 1);
  lua_setglobal(f->main, "nursery_third_release");
  require(collect(f, 3, LUA_OK) == 42);

  lua_pushcfunction(f->main, nursery_native);
  lua_setglobal(f->main, "nursery_native");
  submit(f, 4,
         "local root={answer=42}; local ok,value=pcall(nursery_native,root); "
         "assert(ok); return value",
         0);
  require(collect(f, 4, LUA_OK) == 42);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(lua_getglobal(f->main, "nursery_native_root") == LUA_TTABLE);
  require(lua_getfield(f->main, -1, "answer") == LUA_TNUMBER);
  require(lua_tointeger(f->main, -1) == 42);
  lua_pop(f->main, 2);
  finish(f);
}

static l_mem nursery_bytes(lua_State *L) {
  l_mem bytes;
  lua_lock(L);
  luaW_stop(L);
  bytes = gettotalbytes(G(L)) - getgcbytes(G(L)) -
          __atomic_load_n(&G(L)->GCretired, __ATOMIC_RELAXED);
  luaW_start(L);
  lua_unlock(L);
  return bytes;
}

static void nursery_pacing(struct fixture *f) {
  lua_State *L;
  init(f, 1);
  L = f->main;
  lua_gc(L, LUA_GCINC);
  require(luaL_dostring(L, "nursery_pending={}; "
                           "nursery_weak=setmetatable({nursery_pending}, "
                           "{__mode='v'})") == LUA_OK);
  lua_gc(L, LUA_GCCOLLECT);
  lua_lock(L);
  luaE_setdebt(G(L), 768 * 1024);
  __atomic_store_n(&G(L)->pacer.hard, getgcbytes(G(L)) + 1536 * 1024,
                   __ATOMIC_RELAXED);
  lua_unlock(L);
  lua_pushnil(L);
  lua_setglobal(L, "nursery_pending");

  submit(f, 0,
         "local row={}; for i=1,32768 do row[i]=i end; "
         "nursery_ready=true; while not nursery_release do end; "
         "for i=1,250000 do local scratch={i,i+1,kind='row'} end; "
         "if nursery_weak[1] ~= nil then return -1 end; "
         "return row[32768]",
         0);
  nursery_wait(L, "nursery_ready");
  require(nursery_bytes(L) == 128 * 1024);
  require(lua_getglobal(L, "nursery_weak") == LUA_TTABLE);
  require(lua_rawgeti(L, -1, 1) == LUA_TTABLE);
  lua_pop(L, 2);
  lua_pushboolean(L, 1);
  lua_setglobal(L, "nursery_release");
  require(collect(f, 0, LUA_OK) == 32768);
  require(nursery_bytes(L) == 0 && f->nursery_finished == 1);
  finish(f);
}

int main(int argc, char **argv) {
  struct fixture *f = calloc(1, sizeof(*f));
  require(f != NULL && argc == 2);
  if (strcmp(argv[1], "fairness") == 0)
    fairness(f, 1, 0);
  else if (strcmp(argv[1], "parallel") == 0)
    fairness(f, 2, 1);
  else if (strcmp(argv[1], "no_requests") == 0)
    no_requests(f);
  else if (strcmp(argv[1], "metadata_progress") == 0)
    metadata_progress(f, 1000000);
  else if (strcmp(argv[1], "timed_metadata_progress") == 0)
    metadata_progress(f, 0);
  else if (strcmp(argv[1], "timed_slice") == 0)
    timed_slice(f, 0);
  else if (strcmp(argv[1], "timed_continuation_slice") == 0)
    timed_slice(f, 1);
  else if (strcmp(argv[1], "early_incremental") == 0)
    wake_before_publication(f, 0);
  else if (strcmp(argv[1], "early_generational") == 0)
    wake_before_publication(f, 1);
  else if (strcmp(argv[1], "wake_waiters") == 0)
    wake_waiters(f);
  else if (strcmp(argv[1], "stopped_dispatch") == 0)
    stopped_dispatch(f);
  else if (strcmp(argv[1], "native") == 0)
    native_progress(f, 0);
  else if (strcmp(argv[1], "native_reentry") == 0)
    native_reentry(f);
  else if (strcmp(argv[1], "shutdown") == 0)
    native_progress(f, 1);
  else if (strcmp(argv[1], "parallel_native_compensation") == 0)
    parallel_native_compensation(f);
  else if (strcmp(argv[1], "native_return_compensation") == 0)
    native_return_compensation(f, 0);
  else if (strcmp(argv[1], "native_return_handoff") == 0)
    native_return_compensation(f, 1);
  else if (strcmp(argv[1], "native_leaf_return") == 0)
    native_leaf_return(f);
  else if (strcmp(argv[1], "native_recall_during_stack_borrow") == 0)
    native_recall_during_stack_borrow(f);
  else if (strcmp(argv[1], "yield") == 0)
    voluntary(f);
  else if (strcmp(argv[1], "saturation") == 0)
    native_saturation(f, 0, 0);
  else if (strcmp(argv[1], "cancel_admission") == 0)
    native_saturation(f, 1, 0);
  else if (strcmp(argv[1], "early_native_return") == 0)
    native_saturation(f, 0, 1);
  else if (strcmp(argv[1], "native_grant_stopped_world") == 0)
    native_grant_stopped_world(f);
  else if (strcmp(argv[1], "native_fifo") == 0)
    native_fifo(f, FIFO_ADMISSION);
  else if (strcmp(argv[1], "cancel_granted_native") == 0)
    native_fifo(f, CANCEL_GRANT);
  else if (strcmp(argv[1], "native_competitive") == 0)
    native_fifo(f, COMPETITIVE_ADMISSION);
  else if (strcmp(argv[1], "native_burst_parking") == 0)
    native_burst(f, 0, 2);
  else if (strcmp(argv[1], "native_burst_poll") == 0)
    native_burst(f, 1, 2);
  else if (strcmp(argv[1], "native_burst_single") == 0)
    native_burst(f, 0, 1);
  else if (strcmp(argv[1], "native_lease_expiry") == 0)
    native_lease(f, 1, 1);
  else if (strcmp(argv[1], "native_lease_inactive") == 0)
    native_lease(f, 0, 1);
  else if (strcmp(argv[1], "native_lease_full") == 0)
    native_lease(f, 1, 2);
  else if (strcmp(argv[1], "late_native_waiter") == 0)
    late_native_waiter(f);
  else if (strcmp(argv[1], "startup_failures") == 0)
    startup_failures(f);
  else if (strcmp(argv[1], "idle_notifications") == 0)
    idle_notifications(f);
  else if (strcmp(argv[1], "standby_carriers") == 0)
    standby_carriers(f);
  else if (strcmp(argv[1], "idle_affinity") == 0)
    idle_affinity(f);
  else if (strcmp(argv[1], "callback_progress") == 0)
    callback_progress(f, 0);
  else if (strcmp(argv[1], "timed_callback_progress") == 0)
    callback_progress(f, 1);
  else if (strcmp(argv[1], "synchronous_callback") == 0)
    synchronous_callback(f);
  else if (strcmp(argv[1], "simultaneous_reentry") == 0)
    simultaneous_reentry(f);
  else if (strcmp(argv[1], "parked") == 0)
    parked_wait(f, 0);
  else if (strcmp(argv[1], "cancel_parked") == 0)
    parked_wait(f, 1);
  else if (strcmp(argv[1], "nursery_incremental") == 0)
    nursery(f, 0);
  else if (strcmp(argv[1], "nursery_generational") == 0)
    nursery(f, 1);
  else if (strcmp(argv[1], "nursery_pacing") == 0)
    nursery_pacing(f);
  else
    abort();
  require(live_stacks == 0);
  free(f);
  return 0;
}
