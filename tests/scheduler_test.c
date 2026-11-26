#define _GNU_SOURCE
#include <errno.h>
#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lscheduler.h>
#include <ltable.h>
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
  unsigned full_stage;
  unsigned trace_mutator_waiting;
  int hold_full;
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
static int observe_trace_pause;
static pthread_t idle_host;

enum {
  FULL_OLD_READY = 1,
  FULL_PREPARED,
  FULL_NEW_READY,
  FULL_RESUME,
  FULL_COMPLETE
};

void __real_luaC_preparefull(lua_State *);
void __wrap_luaC_preparefull(lua_State *L) {
  struct fixture *f = fixture;
  __real_luaC_preparefull(L);
  if (f == NULL || !f->hold_full || L != f->main)
    return;
  require(pthread_mutex_lock(&f->mutex) == 0);
  if (f->full_stage == FULL_OLD_READY) {
    f->full_stage = FULL_PREPARED;
    require(pthread_cond_broadcast(&f->changed) == 0);
    while (f->full_stage < FULL_RESUME)
      require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  }
  require(pthread_mutex_unlock(&f->mutex) == 0);
}

void __real_luaC_endtask(lua_State *);
void __wrap_luaC_endtask(lua_State *L) {
  struct fixture *f = fixture;
  if (f != NULL && f->tasks[0].root.thread == L) {
    require(luaC_hastaskheap(L));
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
  if (observe_trace_pause && condition == &G(fixture->main)->world.rendezvous) {
    LuaExecution *execution = luaQ_current();
    require(execution == &fixture->tasks[1].fiber.execution);
    require(execution->managed_depth == 1 && execution->allocation_cache);
    __atomic_store_n(&fixture->waiting, 1, __ATOMIC_RELEASE);
  }
  if ((observe_trace_pause & LUA_WORLD_RETAIN_MUTATORS) &&
      condition == &G(fixture->main)->world.resumed) {
    LuaExecution *execution = luaQ_current();
    require(execution == &fixture->tasks[0].fiber.execution);
    require(execution->managed_depth == 1 && execution->allocation_cache);
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->trace_mutator_waiting = 1;
    require(pthread_cond_broadcast(&fixture->changed) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
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

static void wait_slice_peers(lua_State *L) {
  while (__atomic_load_n(&fixture->scheduler.runnable, __ATOMIC_ACQUIRE) <
         SLICE_PEERS)
    luaX_pollrequests(L);
}

static int poll_slice(lua_State *L) {
  struct fixture *f = fixture;
  uint64_t slice_ns = (uint64_t)luaL_checkinteger(L, 1);
  lua_lock(L);
  wait_slice_peers(L);
  /* Repeated polls keep the current task until its slice expires. */
  for (unsigned i = 0; i < 16; i++)
    luaQ_poll(L);
  __atomic_fetch_add(&f->poll_time_ns, slice_ns - 1, __ATOMIC_RELAXED);
  luaQ_poll(L);
  require(__atomic_load_n(&f->started, __ATOMIC_ACQUIRE) == 0);
  __atomic_fetch_add(&f->poll_time_ns, 1, __ATOMIC_RELAXED);
  luaQ_poll(L);
  /* Already queued peers run before the preempted task resumes. */
  for (unsigned i = 1; i <= SLICE_PEERS; i++)
    require(__atomic_load_n(&f->seen[i], __ATOMIC_ACQUIRE) == 1);
  lua_unlock(L);
  return 0;
}

static int poll_retained(lua_State *L) {
  struct fixture *f = fixture;
  int grew = lua_toboolean(L, 1);
  int first = lua_toboolean(L, 2);
  lua_lock(L);
  if (!grew) {
    wait_slice_peers(L);
    luaQ_poll(L);
  } else if (first) {
    __atomic_fetch_add(&f->poll_time_ns, 250000, __ATOMIC_RELAXED);
    luaQ_poll(L);
    require(__atomic_load_n(&f->started, __ATOMIC_ACQUIRE) == SLICE_PEERS);
  } else {
    __atomic_fetch_add(&f->poll_time_ns, 10000000, __ATOMIC_RELAXED);
    luaQ_poll(L);
    require(__atomic_load_n(&f->started, __ATOMIC_ACQUIRE) == 0);
    __atomic_fetch_add(&f->poll_time_ns, 30000000, __ATOMIC_RELAXED);
    luaQ_poll(L);
    require(__atomic_load_n(&f->seen[1], __ATOMIC_ACQUIRE) == 1);
    require(__atomic_load_n(&f->started, __ATOMIC_ACQUIRE) == 1);
    /* A resumed continuation can finish a read-only traversal. Upon expiry
     * without heap growth it returns to FIFO, even with the graph held. */
    luaQ_poll(L);
    __atomic_fetch_add(&f->poll_time_ns, 10000000, __ATOMIC_RELAXED);
    luaQ_poll(L);
    require(__atomic_load_n(&f->started, __ATOMIC_ACQUIRE) == 1);
    __atomic_fetch_add(&f->poll_time_ns, 30000000, __ATOMIC_RELAXED);
    luaQ_poll(L);
    require(__atomic_load_n(&f->started, __ATOMIC_ACQUIRE) == SLICE_PEERS);
  }
  lua_unlock(L);
  return 0;
}

static void timed_slice(struct fixture *f, int continuation, int retained) {
  const char *chunk = "mark_running(); poll_slice(...); return 10";
  f->poll_time_ns = 1;
  init(f, 1);
  f->scheduler.config.quantum_polls = 0;
  if (retained) {
    if (continuation)
      chunk = "coroutine.yield(); mark_running(); poll_retained(false); "
              "local rows={}; for i=1,8192 do rows[i]={i,i+1} end; "
              "poll_retained(true); return rows[8192][2]-8183";
    else
      chunk = "mark_running(); poll_retained(false); "
              "local rows={}; for i=1,8192 do rows[i]={i,i+1} end; "
              "poll_retained(true,true); return rows[8192][2]-8183";
    managed(f->main, "poll_retained", poll_retained);
  } else {
    if (continuation)
      chunk = "coroutine.yield(); mark_running(); poll_slice(...); return 10";
    lua_gc(f->main, LUA_GCSTOP);
    managed(f->main, "poll_slice", poll_slice);
  }
  managed(f->main, "started", started);
  managed(f->main, "mark_running", mark_running);
  submit(f, 0, chunk, continuation ? 10000000 : 250000);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->waiting == 0)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  for (unsigned i = 1; i <= SLICE_PEERS; i++)
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

static int retained_ready(lua_State *L) {
  struct fixture *f = fixture;
  lua_lock(L);
  luaQ_poll(L);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->waiting++;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  while (!__atomic_load_n(&f->native_release, __ATOMIC_ACQUIRE)) {
    luaX_pollrequests(L);
    sched_yield();
  }
  lua_unlock(L);
  return 0;
}

static void retained_fairness(struct fixture *f) {
  init(f, 2);
  f->scheduler.config.quantum_polls = 0;
  lua_gc(f->main, LUA_GCGEN);
  managed(f->main, "started", started);
  managed(f->main, "all_started", all_started);
  managed(f->main, "retained_ready", retained_ready);
  for (unsigned i = 0; i < 2; ++i)
    submit(f, i,
           "started(...); local rows={}; "
           "for i=1,8192 do rows[i]={i,i+1} end; retained_ready(); "
           "while not all_started() do coroutine.yield() end; "
           "return rows[8192][2]",
           (int)i);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->waiting != 2)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  /* The queued tasks must start while both resident graphs remain live. */
  for (unsigned i = 2; i < TASKS; ++i)
    submit(f, i, "started(...); return ...", (int)i);
  __atomic_store_n(&f->native_release, 1, __ATOMIC_RELEASE);
  for (unsigned i = 2; i < TASKS; ++i)
    require(collect(f, i, LUA_OK) == (lua_Integer)i);
  for (unsigned i = 0; i < 2; ++i)
    require(collect(f, i, LUA_OK) == 8193);
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

static int poll_at_trace_pause(lua_State *L) {
  struct fixture *f = fixture;
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_entered = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  while (!__atomic_load_n(&f->waiting, __ATOMIC_ACQUIRE))
    sched_yield();
  luaX_pollrequests(L);
  return 0;
}

static int close_trace_pause(lua_State *L) {
  luaW_collectenter(L);
  G(L)->world.stop_slots = (unsigned)observe_trace_pause;
  luaW_stop(L);
  luaW_heapleave(L);
  if (observe_trace_pause & LUA_WORLD_RETAIN_MUTATORS) {
    struct fixture *f = fixture;
    require(pthread_mutex_lock(&f->mutex) == 0);
    while (!f->trace_mutator_waiting)
      require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
  }
  luaW_start(L);
  return 0;
}

static void trace_pause(struct fixture *f, int retain_mutators) {
  observe_trace_pause = LUA_WORLD_RETAIN_COLLECTOR |
                        (retain_mutators ? LUA_WORLD_RETAIN_MUTATORS : 0);
  init(f, 2);
  lua_gc(f->main, LUA_GCSTOP);
  managed(f->main, "poll_at_trace_pause", poll_at_trace_pause);
  managed(f->main, "close_trace_pause", close_trace_pause);
  submit(f, 0, "poll_at_trace_pause(); return 42", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "close_trace_pause(); return 43", 0);
  require(collect(f, 0, LUA_OK) == 42);
  require(collect(f, 1, LUA_OK) == 43 && f->tasks[1].parks == 0);
  require(!retain_mutators || f->tasks[0].parks == 0);
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

static int hold_userdata(lua_State *L) {
  struct fixture *f = fixture;
  unsigned char *bytes = lua_newuserdatauv(L, 4096, 0);
  bytes[0] = 41;
  bytes[4095] = 1;
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_entered = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->native_release)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  lua_newuserdatauv(L, 4096, 0); /* leave a fresh batch for the task's exit */
  lua_pushinteger(L, bytes[0] + bytes[4095]);
  return 1;
}

static void native_accounting(struct fixture *f, int arm) {
  LuaAllocationDebt withdrawn;
  global_State *g;
  lua_State *owner;
  l_mem total, shared;
  init(f, 1);
  lua_gc(f->main, LUA_GCGEN);
  lua_gc(f->main, LUA_GCSTOP);
  lua_pushcfunction(f->main, hold_userdata);
  lua_setglobal(f->main, "hold_userdata");
  submit(f, 0, "return hold_userdata()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  owner = f->tasks[0].root.thread;
  g = G(f->main);
  withdrawn.pending = __atomic_load_n(&owner->withdrawn_debt, __ATOMIC_ACQUIRE);
  require(withdrawn.batch.total <= -4096 && withdrawn.batch.shared <= -4096);
  total = gettotalbytes(g) - withdrawn.batch.total;
  shared = getgcbytes(g) - withdrawn.batch.shared;
  if (arm) {
    uint64_t epoch = luaC_traceepoch(g);
    require(lua_gc(f->main, LUA_GCSTEP, (size_t)0) >= 0);
    require(luaC_traceepoch(g) != epoch && g->pacer.start == shared);
  } else {
    l_mem kib = lua_gc(f->main, LUA_GCCOUNT);
    require(kib * 1024 + lua_gc(f->main, LUA_GCCOUNTB) == total);
    require(getgcbytes(g) == shared);
  }
  require(__atomic_load_n(&owner->withdrawn_debt, __ATOMIC_ACQUIRE) == 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  luaSC_wait(&f->tasks[0]);
  require(__atomic_load_n(&owner->withdrawn_debt, __ATOMIC_ACQUIRE) == 0);
  require(collect(f, 0, LUA_OK) == 42);
  finish(f);
}

static void native_progress(struct fixture *f, int shutdown, int nested) {
  unsigned i;
  init(f, 1);
  lua_pushcfunction(f->main, block_binding); /* unchanged foreign binding */
  lua_setglobal(f->main, "block_binding");
  submit(f, 0,
         nested
             ? "local call=coroutine.wrap(function() "
               "local result=block_binding(); return result end); return call()"
             : "return block_binding()",
         0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  if (nested)
    require(lua_status(f->tasks[0].root.thread) == LUA_OK);
  for (i = 1; i < 5; i++)
    submit(f, i,
           nested && i == 1
               ? "collectgarbage('collect'); return 500"
               : "local t={}; for i=1,500 do t[i]=i end; return t[500]",
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
  if (nested)
    require(collect(f, 0, LUA_OK) == 500500);
  else
    (void)collect(f, 0, shutdown ? LUA_ERRRUN : LUA_OK);
  require(f->scheduler.native == 0);
  if (shutdown)
    require(luaSC_shutdown(&f->scheduler) == LUA_OK);
  finish(f);
}

static int native_stop_gate(lua_State *L) {
  struct fixture *f = fixture;
  lua_Integer index = luaL_checkinteger(L, 1);
  require(index == 0 || index == 1);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_entered++;
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!(f->native_release & (1u << index)))
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  return 1;
}

static void native_overlapping_stops(struct fixture *f) {
  initlimits(f, 2, 2);
  lua_pushcfunction(f->main, native_stop_gate);
  lua_setglobal(f->main, "native_stop_gate");
  for (unsigned i = 0; i < 2; i++)
    submit(f, i, "local value=native_stop_gate(...); return value", (int)i);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->native_entered != 2)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  for (unsigned i = 0; i < 2; i++) {
    require(luaTK_requeststop(&f->tasks[i].root) == LUA_OK);
    require(luaTK_requeststop(&f->tasks[i].root) == LUA_OK);
  }
  for (unsigned i = 0; i < 2; i++) {
    require(pthread_mutex_lock(&f->mutex) == 0);
    f->native_release |= 1u << i;
    require(pthread_cond_broadcast(&f->changed) == 0);
    require(pthread_mutex_unlock(&f->mutex) == 0);
    luaSC_wait(&f->tasks[i]);
    require(luaTK_hasstops(f->main) == (i == 0));
    (void)collect(f, i, LUA_ERRRUN);
    if (i == 0) {
      submit(f, 2, "local function answer() return 42 end; return answer()", 0);
      require(collect(f, 2, LUA_OK) == 42);
    }
  }
  require(!luaTK_hasstops(f->main));
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

static int native_index_method(lua_State *L) {
  const char *key = luaL_checkstring(L, 2);
  require(lua_type(L, 1) == LUA_TUSERDATA);
  if (strcmp(key, "blocked") == 0)
    blocked_leaf(L);
  else if (strcmp(key, "failure") == 0)
    return luaL_error(L, "index failure");
  else if (strcmp(key, "yielded") == 0) {
    lua_pushinteger(L, 42);
    return lua_yield(L, 1);
  }
  lua_pushinteger(L, 41);
  return 1;
}

static int native_index_api(lua_State *L) {
  lua_getfield(L, 1, "yielded");
  return 1;
}

static void native_index_dispatch(struct fixture *f) {
  init(f, 1);
  lua_newuserdatauv(f->main, 1, 0);
  lua_newtable(f->main);
  lua_pushcfunction(f->main, native_index_method);
  lua_setfield(f->main, -2, "__index");
  require(lua_setmetatable(f->main, -2));
  lua_setglobal(f->main, "index_target");
  lua_pushcfunction(f->main, native_index_api);
  lua_setglobal(f->main, "native_index_api");
  submit(
      f, 0,
      "local u=index_target; assert(u.value==41 and u.value==41); "
      "local ok,err=pcall(function() return u.failure end); "
      "assert(not ok and err:find('index failure',1,true)); "
      "local co=coroutine.wrap(function() return u.yielded end); "
      "assert(co()==42 and co(43)==43); "
      "ok,err=pcall(native_index_api,u); "
      "assert(not ok and err:find('yield across a C-call boundary',1,true)); "
      "local old=u.blocked; return old+u.replaced",
      0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1,
         "local mt=getmetatable(index_target); mt.__index=nil; "
         "for i=1,300 do mt['grow'..i]=i end; "
         "mt.__index={replaced=10}; collectgarbage('collect'); return 52",
         0);
  require(collect(f, 1, LUA_OK) == 52);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 51);
  finish(f);
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

static int make_argument(lua_State *L) {
  int *value = lua_newuserdatauv(L, sizeof(*value), 0);
  *value = 41;
  return 1;
}

static int observe_arguments(lua_State *L) {
  size_t len;
  int nargs = lua_gettop(L);
  const char *before = lua_tolstring(L, 2, &len);
  const int *value = lua_touserdata(L, 1);
  require(lua_type(L, 1) == LUA_TUSERDATA && value && *value == 41);
  require(before && len == 7 && memcmp(before, "old\0key", len) == 0);
  require(lua_tolstring(L, 1, NULL) == NULL);
  if (nargs > 2) {
    int isnum;
    require(lua_type(L, 3) == LUA_TNUMBER && lua_type(L, 8) == LUA_TBOOLEAN);
    require(lua_isinteger(L, 3) && lua_tointegerx(L, 3, &isnum) == -17 &&
            isnum);
    require(lua_tonumberx(L, 3, &isnum) == -17 && isnum);
    require(!lua_toboolean(L, 6) && !lua_toboolean(L, 7) &&
            lua_toboolean(L, 8));
    require(lua_checkstack(L, 3));
  }
  lua_pushinteger(L, 17);
  require(__atomic_load_n(&L->stack_gate.arguments[0], __ATOMIC_ACQUIRE) != 0);
  require(lua_tolstring(L, 2, &len) == before && len == 7);
  blocked_leaf(L);
  /* The old string is now at index 1 and still roots this borrowed pointer. */
  require(lua_type(L, 1) == LUA_TSTRING && lua_touserdata(L, 1) == NULL);
  require(memcmp(before, "old\0key", 7) == 0);
  require(lua_tolstring(L, 1, &len) == before && len == 7);
  if (nargs > 2) {
    require(!lua_isinteger(L, 3) && lua_tonumber(L, 3) == 4.5);
    require(lua_isinteger(L, 4) && lua_tointeger(L, 4) == LUA_MININTEGER);
    require(!lua_isinteger(L, 5) && lua_tonumber(L, 5) == 2.0);
  }
  lua_getglobal(L, "check_native_arguments");
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  require(lua_pcall(L, 2, 1, 0) == LUA_OK);
  return 1;
}

static void native_argument_observation(struct fixture *f, int wide) {
  lua_State *target;
  size_t len;
  const char *bytes;
  init(f, 1);
  lua_pushcfunction(f->main, warm_native_permit);
  lua_setglobal(f->main, "warm_native_permit");
  lua_pushcfunction(f->main, make_argument);
  lua_setglobal(f->main, "make_argument");
  lua_pushcfunction(f->main, observe_arguments);
  lua_setglobal(f->main, "observe_arguments");
  require(luaL_dostring(f->main, "function check_native_arguments(a,b) "
                                 "assert(a=='old\\0key' and b=='next\\0key'); "
                                 "return 51 end") == LUA_OK);
  submit(f, 0,
         wide ? "warm_native_permit(); return observe_arguments("
                "make_argument(),'old\\0key',-17,math.mininteger,2.0,false,nil,"
                "true)"
              : "warm_native_permit(); "
                "return observe_arguments(make_argument(),'old\\0key')",
         0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  target = f->tasks[0].root.thread;
  require(pthread_mutex_unlock(&f->mutex) == 0);
  bytes = lua_tolstring(target, 2, &len);
  require(bytes && len == 7 && memcmp(bytes, "old\0key", len) == 0);
  require(lua_type(target, 1) == LUA_TUSERDATA);
  lua_copy(target, 2, 1);
  require(lua_type(target, 1) == LUA_TSTRING);
  lua_pushlstring(target, "next\0key", 8);
  lua_replace(target, 2);
  if (wide) {
    lua_pushnumber(target, 4.5);
    lua_replace(target, 3);
  }
  submit(f, 1, "return 52", 0);
  require(collect(f, 1, LUA_OK) == 52);
  require(lua_gc(f->main, LUA_GCCOLLECT) == 0);
  require(lua_tolstring(target, 1, &len) == bytes && len == 7);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 51);
  finish(f);
}

static int retire_before_native(lua_State *L) {
  Table *table = (Table *)lua_topointer(L, 1);
  require(table != NULL);
  luaH_resize(L, table, 64, 0);
  require(L->storage_retired != NULL);
  return 0;
}

static void native_recall_during_stack_borrow(struct fixture *f, int root_offer,
                                              int retirement) {
  lua_State *target;
  int reservation;
  init(f, 1);
  /* Exercise the slot-owner offer when the stack has no slot encoding. */
  if (!root_offer)
    f->scheduler.slots[0].stack_offer = 0;
  lua_pushcfunction(f->main, warm_native_permit);
  lua_setglobal(f->main, "warm_native_permit");
  lua_pushcfunction(f->main, blocked_leaf);
  lua_setglobal(f->main, "blocked_leaf");
  if (retirement) {
    lua_gc(f->main, LUA_GCSTOP);
    lua_createtable(f->main, 32, 0);
    lua_pushinteger(f->main, 51);
    lua_rawseti(f->main, -2, 1);
    lua_setglobal(f->main, "resize_target");
    managed(f->main, "retire_before_native", retire_before_native);
    submit(f, 0,
           "warm_native_permit(); retire_before_native(resize_target); "
           "blocked_leaf(resize_target); return resize_target[1]",
           0);
  } else {
    submit(
        f, 0,
        "local value={answer=51}; warm_native_permit(); blocked_leaf(value); "
        "return value.answer",
        0);
  }
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  target = f->tasks[0].root.thread;
  require(pthread_mutex_unlock(&f->mutex) == 0);

  reservation = luaX_tryreserve(target);
  require(reservation == 1);
  require(target->storage_retired == NULL && target->storage_bytes == 0);
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

static void native_operation_done(void) {
  struct fixture *f = fixture;
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_entered++;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
}

static int read_native_metadata(lua_State *L) {
  blocked_leaf(L);
  require(lua_getmetatable(L, 1) && lua_rawequal(L, -1, 2));
  lua_pop(L, 1);
  require(lua_getiuservalue(L, 1, 1) == LUA_TTABLE);
  require(lua_getiuservalue(L, 1, 2) == LUA_TNONE && lua_isnil(L, -1));
  lua_pop(L, 1);
  native_operation_done();
  return 1;
}

static int wait_for_native_operation(lua_State *L) {
  struct fixture *f = fixture;
  mark_running(L);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->native_entered < 2)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  return 0;
}

static int wait_for_metadata_read(lua_State *L) {
  wait_for_native_operation(L);
  /* The native stack is now the only root of the result table. */
  lua_pushnil(L);
  require(lua_setiuservalue(L, 1, 1));
  require(lua_gc(L, LUA_GCCOLLECT) == 0);
  require(lua_getiuservalue(L, 1, 1) == LUA_TNIL);
  lua_pop(L, 1);
  return 0;
}

static void native_metadata_without_slot(struct fixture *f) {
  init(f, 1);
  lua_gc(f->main, LUA_GCSTOP);
  lua_newuserdatauv(f->main, 1, 1);
  lua_newtable(f->main);
  lua_setmetatable(f->main, -2);
  lua_newtable(f->main);
  lua_pushinteger(f->main, 51);
  lua_setfield(f->main, -2, "answer");
  require(lua_setiuservalue(f->main, -2, 1));
  lua_setglobal(f->main, "metadata_owner");
  lua_pushcfunction(f->main, read_native_metadata);
  lua_setglobal(f->main, "read_native_metadata");
  managed(f->main, "wait_for_metadata_read", wait_for_metadata_read);
  submit(f, 0,
         "return read_native_metadata(metadata_owner, "
         "getmetatable(metadata_owner)).answer",
         0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "wait_for_metadata_read(metadata_owner); return 52", 0);
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

static int read_native_registry(lua_State *L) {
  int ref = (int)luaL_checkinteger(L, 2);
  blocked_leaf(L);
  unsigned *payload = luaL_checkudata(L, 1, "luthr.registry-read");
  require(*payload == 42);
  require(lua_getfield(L, LUA_REGISTRYINDEX, "luthr.registry-read") ==
          LUA_TTABLE);
  require(lua_getfield(L, -1, "answer") == LUA_TNUMBER);
  luaL_unref(L, LUA_REGISTRYINDEX, ref);
  lua_pushinteger(L, 60);
  require(luaL_ref(L, LUA_REGISTRYINDEX) == ref);
  native_operation_done();
  return 1;
}

static void native_registry_without_slot(struct fixture *f) {
  init(f, 1);
  lua_gc(f->main, LUA_GCSTOP);
  luaL_newmetatable(f->main, "luthr.registry-read");
  lua_pushinteger(f->main, 51);
  lua_setfield(f->main, -2, "answer");
  unsigned *payload = lua_newuserdatauv(f->main, sizeof(*payload), 0);
  *payload = 42;
  lua_pushvalue(f->main, -2);
  lua_setmetatable(f->main, -2);
  lua_setglobal(f->main, "registry_object");
  lua_pop(f->main, 1);
  lua_pushcfunction(f->main, read_native_registry);
  lua_setglobal(f->main, "read_native_registry");
  managed(f->main, "wait_for_native_operation", wait_for_native_operation);
  lua_State *reader = lua_newthread(f->main);
  lua_pushinteger(reader, 59);
  int ref = luaL_ref(reader, LUA_REGISTRYINDEX);
  require(luaL_loadstring(
              reader, "return read_native_registry(registry_object, ...)") ==
          LUA_OK);
  lua_pushinteger(reader, ref);
  require(luaSC_submit(&f->scheduler, &f->tasks[0], reader, 1) == 0);
  lua_pop(f->main, 1);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "wait_for_native_operation(); return 52", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->waiting)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 51);
  require(collect(f, 1, LUA_OK) == 52);
  require(lua_rawgeti(f->main, LUA_REGISTRYINDEX, ref) == LUA_TNUMBER);
  require(lua_tointeger(f->main, -1) == 60);
  lua_pop(f->main, 1);
  luaL_unref(f->main, LUA_REGISTRYINDEX, ref);
  finish(f);
}

static int edit_native_array(lua_State *L) {
  blocked_leaf(L);
  const Table *table = lua_topointer(L, 1);
  require(table != NULL && luaC_tasktable(table));
  lua_pushvalue(L, 2);
  lua_rawseti(L, 1, 1);
  require(lua_rawgeti(L, 1, 1) == LUA_TTABLE);
  require(lua_rawgeti(L, -1, 1) == LUA_TNUMBER);
  native_operation_done();
  return 1;
}

static int create_native_array(lua_State *L) {
  blocked_leaf(L);
  lua_newtable(L);
  require(lua_rawgeti(L, -1, 1) == LUA_TNIL);
  lua_pop(L, 1);
  lua_createtable(L, 6, 0);
  lua_pushvalue(L, 1);
  lua_rawseti(L, -3, 1);
  lua_pushvalue(L, -2);
  lua_rawseti(L, -2, 1);
  native_operation_done();
  return 1;
}

static int edit_native_fields(lua_State *L) {
  static const char first[] = "first";
  static const char second[] = "second";
  lua_createtable(L, 0, 8);
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, first); /* warm the coroutine's C-string cache */
  lua_pushstring(L, second);
  lua_pop(L, 1);
  blocked_leaf(L);
  lua_pushvalue(L, 1);
  lua_setfield(L, -2, first);
  require(strcmp(lua_pushstring(L, second), second) == 0);
  lua_pushvalue(L, 1);
  lua_rawset(L, -3);
  native_operation_done();
  return 1;
}

static int collect_after_native_operation(lua_State *L) {
  wait_for_native_operation(L);
  require(lua_gc(L, LUA_GCCOLLECT) == 0);
  return 0;
}

static int inspect_native_stack(lua_State *L) {
  if (lua_gettop(L) == 0) {
    require(lua_checkstack(L, 80));
    return 0;
  }
  require(lua_type(L, 1) == LUA_TNIL && lua_type(L, 2) == LUA_TBOOLEAN);
  require(lua_type(L, 3) == LUA_TNUMBER && lua_isstring(L, 4));
  require(lua_type(L, 5) == LUA_TTABLE && lua_type(L, 6) == LUA_TFUNCTION);
  require(lua_type(L, 7) == LUA_TTHREAD && lua_isuserdata(L, 8));
  require(lua_type(L, 9) == LUA_TNUMBER && lua_type(L, 10) == LUA_TNONE);
  blocked_leaf(L);
  require(lua_checkstack(L, 48));
  for (int i = 0; i < 47; i++)
    lua_pushinteger(L, i);
  require(lua_gettop(L) == 56 && lua_tointeger(L, 56) == 46);
  require(lua_type(L, 1) == LUA_TNIL && lua_isuserdata(L, 8));
  lua_pushinteger(L, 51);
  native_operation_done();
  return 1;
}

static void native_operation_without_slot(struct fixture *f, lua_CFunction fn,
                                          const char *source, int gc_after) {
  init(f, 1);
  lua_pushcfunction(f->main, fn);
  lua_setglobal(f->main, "native_table_operation");
  managed(f->main, "wait_for_native_operation",
          gc_after ? collect_after_native_operation
                   : wait_for_native_operation);
  if (gc_after) {
    lua_gc(f->main, LUA_GCINC);
    lua_gc(f->main, LUA_GCPARAM, LUA_GCPPAUSE, 5000);
  }
  require(lua_gc(f->main, LUA_GCCOLLECT) == 0);
  submit(f, 0, source, 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  if (!gc_after)
    require(lua_gc(f->main, LUA_GCSTOP) == 0);
  /* The peer holds the only managed slot until the native operation ends. */
  submit(f, 1, "wait_for_native_operation(); return 52", 0);
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

static int native_noop(lua_State *L) {
  (void)L;
  return 0;
}

static int native_yield_value(lua_State *L) {
  lua_pushinteger(L, 43);
  return lua_yield(L, 1);
}

static int native_permit_cached(lua_State *L) {
  lua_pushboolean(L, luaSC_nativeactive(&fixture->scheduler) == 1);
  return 1;
}

static void native_lease_yield(struct fixture *f) {
  init(f, 1);
  lua_gc(f->main, LUA_GCSTOP);
  lua_pushcfunction(f->main, native_noop);
  lua_setglobal(f->main, "native_noop");
  lua_pushcfunction(f->main, native_yield_value);
  lua_setglobal(f->main, "native_yield_value");
  managed(f->main, "native_permit_cached", native_permit_cached);
  submit(f, 0,
         "local co=coroutine.wrap(function() native_noop(); "
         "coroutine.yield(42) end); local result=co(); "
         "assert(native_permit_cached(), 'managed yield released permit'); "
         "local native=coroutine.wrap(native_yield_value); "
         "assert(native()==43 and not native_permit_cached()); return result",
         0);
  require(collect(f, 0, LUA_OK) == 42);
  require(luaSC_nativeactive(&f->scheduler) == 0);
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
  lua_gc(f->main, LUA_GCSTOP);
  managed(f->main, "inspect_waiting_callback", inspect_waiting_callback);
  lua_pushcfunction(f->main, invoke_callback);
  lua_setglobal(f->main, "invoke_callback");
  const char *source = "local function scalar() return 4 end; "
                       "for i = 1, 8 do "
                       "  assert(invoke_callback(scalar, false) == 4) "
                       "end; return invoke_callback(function() "
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

static int nursery_inspect(lua_State *L) {
  const TValue *value = s2v(L->ci->func.p + 1);
  require(ttistable(value));
  lua_pushboolean(L, luaC_tasktable(hvalue(value)));
  lua_pushinteger(L, L->task_root_count);
  return 2;
}

static int nursery_marking(lua_State *L) {
  lua_pushboolean(L, L->task_mark_scan != 0);
  return 1;
}

static int nursery_layout_count(lua_State *L) {
  const TValue *value = s2v(L->ci->func.p + 1);
  unsigned count = 0;
  require(ttistable(value) && luaC_tasktable(hvalue(value)));
  require(luaH_recordkeys(hvalue(value)->node) != NULL);
  for (unsigned i = 0; i < L->task_root_count; i++)
    count += (L->task_roots[i] & LUA_TASK_ROOT_LAYOUT) != 0;
  lua_pushinteger(L, count);
  return 1;
}

static int nursery_resize(lua_State *L) {
  Table *table;
  lua_lock(L);
  table = hvalue(s2v(L->ci->func.p + 1));
  require(luaC_tasktable(table));
  luaH_resize(L, table, table->asize * 2, 8);
  lua_unlock(L);
  return 0;
}

static int nursery_roundtrip(lua_State *L) {
  struct fixture *f = fixture;
  size_t length;
  const char *bytes;
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  require(lua_pcall(L, 1, 2, 0) == LUA_OK);
  require(lua_toboolean(L, -2) && lua_tointeger(L, -1) == 42);
  require(lua_getfield(L, 2, "payload") == LUA_TSTRING);
  bytes = lua_tolstring(L, -1, &length);
  require(bytes != NULL && length == 168);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_entered = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->native_release)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(memcmp(bytes + length - 14, "private record", 14) == 0);
  lua_pushvalue(L, -1);
  lua_xmove(L, f->main, 1);
  lua_pushvalue(L, 2);
  lua_xmove(L, f->main, 1);
  lua_pushinteger(L, 42);
  return 1;
}

static int native_array(lua_State *L) {
  lua_Integer first = luaL_checkinteger(L, 1);
  int nested = lua_istable(L, 2);
  lua_newtable(L);
  for (int i = 1; i <= 6; i++) {
    if (nested && i == 1)
      lua_pushvalue(L, 2);
    else
      lua_pushinteger(L, first + i - 1);
    lua_rawseti(L, -2, i);
    if (nested) {
      require(lua_rawlen(L, -1) == (lua_Unsigned)i);
      if (i == 1) {
        require(lua_rawgeti(L, -1, i) == LUA_TTABLE);
        require(lua_rawequal(L, -1, 2));
      } else {
        require(lua_rawgeti(L, -1, i) == LUA_TNUMBER);
        require(lua_tointeger(L, -1) == first + i - 1);
      }
      lua_pop(L, 1);
    }
  }
  if (nested) {
    lua_pushnil(L);
    lua_rawseti(L, -2, 3);
    require(lua_rawgeti(L, -1, 3) == LUA_TNIL);
    lua_pop(L, 1);
    lua_pushinteger(L, first + 2);
    lua_rawseti(L, -2, 3);
  }
  return 1;
}

static void nursery_native_call(struct fixture *f, int generational) {
  init(f, 1);
  lua_gc(f->main, generational ? LUA_GCGEN : LUA_GCINC);
  managed(f->main, "nursery_inspect", nursery_inspect);
  lua_pushcfunction(f->main, nursery_roundtrip);
  lua_setglobal(f->main, "nursery_roundtrip");
  lua_pushcfunction(f->main, native_array);
  lua_setglobal(f->main, "native_array");
  submit(
      f, 0,
      "local array=native_array(41); assert(nursery_inspect(array)); "
      "local sample={('private record'):rep(12)}; "
      "local private,edges=nursery_inspect(sample); "
      "local row={child={answer=42,note=('child only'):rep(16)},"
      "           payload=('private record'):rep(12)}; "
      "assert(nursery_inspect(row)); "
      "local result=nursery_roundtrip(function(value) "
      "  return nursery_inspect(value),value.child.answer "
      "end,row); assert(private and edges==0 and array[2]==42); return result",
      0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  for (unsigned i = 0; i < 4; i++)
    require(lua_gc(f->main, LUA_GCSTEP, (size_t)0) >= 0);
  require(lua_gc(f->main, LUA_GCCOLLECT) == 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 42);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(lua_getfield(f->main, -1, "payload") == LUA_TSTRING);
  require(lua_rawlen(f->main, -1) == 168);
  require(lua_tostring(f->main, -1) == lua_tostring(f->main, -3));
  lua_pop(f->main, 1);
  require(lua_getfield(f->main, -1, "child") == LUA_TTABLE);
  require(lua_getfield(f->main, -1, "note") == LUA_TSTRING);
  require(lua_rawlen(f->main, -1) == 160);
  lua_pop(f->main, 1);
  require(lua_getfield(f->main, -1, "answer") == LUA_TNUMBER);
  require(lua_tointeger(f->main, -1) == 42);
  lua_pop(f->main, 4);
  finish(f);
}

static void nursery_native_allocation(struct fixture *f, int generational) {
  int status, results;
  init(f, 1);
  lua_gc(f->main, generational ? LUA_GCGEN : LUA_GCINC);
  managed(f->main, "nursery_inspect", nursery_inspect);
  lua_pushcfunction(f->main, native_array);
  lua_setglobal(f->main, "native_array");
  submit(f, 0,
         "local first=native_array(41,{41}); assert(nursery_inspect(first)); "
         "for i=1,30000 do "
         "  local row=native_array(i); "
         "  if i%100==0 then assert(nursery_inspect(row) and row[6]==i+5) end "
         "end; "
         "assert(nursery_inspect(first)); return first",
         0);
  luaSC_wait(&f->tasks[0]);
  require(luaSC_collect(f->main, &f->tasks[0], &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(lua_rawlen(f->main, -1) == 6);
  require(lua_rawgeti(f->main, -1, 1) == LUA_TTABLE);
  require(lua_rawgeti(f->main, -1, 1) == LUA_TNUMBER);
  require(lua_tointeger(f->main, -1) == 41);
  lua_pop(f->main, 2);
  require(lua_rawgeti(f->main, -1, 2) == LUA_TNUMBER);
  require(lua_tointeger(f->main, -1) == 42);
  lua_pop(f->main, 2);
  finish(f);
}

static int native_string_callback(lua_State *L) {
  char input[80];
  unsigned byte = (unsigned)luaL_checkinteger(L, 2) % 128;
  const char *copy;
  memset(input, 'x', sizeof(input));
  input[8] = '\0';
  input[9] = (char)byte;
  copy = lua_pushlstring(L, input, sizeof(input));
  memset(input, 'y', sizeof(input));
  lua_pushvalue(L, 1);
  lua_pushvalue(L, -2);
  lua_call(L, 1, 0);
  require(copy[0] == 'x' && copy[8] == '\0' && (unsigned char)copy[9] == byte &&
          copy[79] == 'x' && copy[80] == '\0');
  return 1;
}

static int native_publish_strings(lua_State *L) {
  enum { count = 2048 };
  char bytes[80];
  memset(bytes, 'z', sizeof(bytes));
  luaL_checkstack(L, count, "native strings");
  for (int i = 0; i < count; i++)
    lua_pushlstring(L, bytes, sizeof(bytes));
  lua_setglobal(L, "published_string");
  return 0;
}

static void nursery_native_strings(struct fixture *f, int generational) {
  init(f, 1);
  lua_gc(f->main, generational ? LUA_GCGEN : LUA_GCINC);
  lua_pushcfunction(f->main, native_string_callback);
  lua_setglobal(f->main, "copy_string");
  lua_pushcfunction(f->main, native_publish_strings);
  lua_setglobal(f->main, "publish_strings");
  submit(f, 0,
         "local function check(s) assert(#s==80 and s:byte(9)==0) end; "
         "local before=collectgarbage('count'); "
         "local saved=copy_string(function(s) check(s); "
         "collectgarbage('collect') end,42); "
         "for i=1,18000 do copy_string(check,i) end; "
         "assert(collectgarbage('count') < before+1024); "
         "publish_strings(); collectgarbage('collect'); "
         "local after=collectgarbage('count'); "
         "assert(after>0 and after<before+1024); "
         "assert(saved:byte(10)==42 and published_string==('z'):rep(80)); "
         "return 42",
         0);
  require(collect(f, 0, LUA_OK) == 42);
  finish(f);
}

static void nursery_edges(struct fixture *f, int generational) {
  int status, results;
  init(f, 2);
  luaL_openlibs(f->main);
  lua_gc(f->main, generational ? LUA_GCGEN : LUA_GCINC);
  managed(f->main, "nursery_inspect", nursery_inspect);
  require(luaL_dostring(f->main, "nursery_external={child={42}}; "
                                 "nursery_weak=setmetatable({nursery_external},"
                                 "{__mode='v'})") == LUA_OK);
  submit(f, 0,
         "local external=nursery_external; local root={external}; "
         "root[external]=42; nursery_external=nil; external=nil; "
         "local private,edges=nursery_inspect(root); "
         "nursery_ready=true; while not nursery_release do end; "
         "assert(private and edges==1); "
         "assert(root[1].child[1]==42 and root[root[1]]==42); "
         "private,edges=nursery_inspect(root); assert(private and edges==1); "
         "root[root[1]]=nil; root[1]=nil; "
         "for i=1,5000 do local row={i,i+1}; assert(row[1]==i) end; "
         "private,edges=nursery_inspect(root); assert(private and edges==0); "
         "for i=1,100 do "
         "  local row={('record '..i):rep(12)}; "
         "  assert(nursery_inspect(row) and #row[1]>=96) "
         "end; "
         "return 42",
         0);
  nursery_wait(f->main, "nursery_ready");
  for (unsigned i = 0; i < 4; i++)
    lua_gc(f->main, LUA_GCSTEP, (size_t)128 * 1024);
  require(luaL_dostring(f->main, "assert(nursery_weak[1].child[1]==42)") ==
          LUA_OK);
  lua_pushboolean(f->main, 1);
  lua_setglobal(f->main, "nursery_release");
  require(collect(f, 0, LUA_OK) == 42 && f->nursery_finished == 1);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(luaL_dostring(f->main, "assert(nursery_weak[1]==nil)") == LUA_OK);

  require(luaL_dostring(f->main, "nursery_shared={}; "
                                 "for i=1,192 do "
                                 "  nursery_shared[i]=('record '..i):rep(12) "
                                 "end") == LUA_OK);
  submit(f, 1,
         "local retained={}; assert(nursery_inspect(retained)); "
         "for i=1,192 do retained[i]=nursery_shared[i] end; "
         "local private,edges=nursery_inspect(retained); "
         "assert(private and edges==192); "
         "for i=1,96 do retained[i]=nil end; "
         "local own=('private'):rep(12); "
         "retained[193]=own; "
         "retained[own]=own; "
         "for i=1,3200 do local text=own..own; assert(#text==168) end; "
         "private,edges=nursery_inspect(retained); "
         "assert(private and edges==96 and retained[own]==own); "
         "return retained",
         0);
  luaSC_wait(&f->tasks[1]);
  require(luaSC_collect(f->main, &f->tasks[1], &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1);
  lua_gc(f->main, LUA_GCCOLLECT);
  for (unsigned i = 1; i <= 96; i++) {
    require(lua_geti(f->main, -1, i) == LUA_TNIL);
    lua_pop(f->main, 1);
  }
  for (unsigned i = 97; i <= 192; i++) {
    char prefix[16];
    int length = snprintf(prefix, sizeof(prefix), "record %u", i);
    require(length > 0 && (size_t)length < sizeof(prefix));
    require(lua_geti(f->main, -1, i) == LUA_TSTRING);
    require(lua_rawlen(f->main, -1) == (size_t)length * 12);
    require(strncmp(lua_tostring(f->main, -1), prefix, (size_t)length) == 0);
    lua_pop(f->main, 1);
  }
  require(lua_geti(f->main, -1, 193) == LUA_TSTRING);
  require(lua_rawlen(f->main, -1) == 84);
  lua_pushvalue(f->main, -1);
  require(lua_rawget(f->main, -3) == LUA_TSTRING);
  require(lua_rawequal(f->main, -2, -1));
  lua_pop(f->main, 2);
  lua_pop(f->main, 1);
  finish(f);
}

static void nursery_survivors(struct fixture *f, int generational) {
  int status, results;
  init(f, 2);
  luaL_openlibs(f->main);
  lua_gc(f->main, generational ? LUA_GCGEN : LUA_GCINC);
  managed(f->main, "nursery_inspect", nursery_inspect);
  require(luaL_dostring(f->main, "nursery_external={42}") == LUA_OK);
  submit(f, 0,
         "local root={nursery_external}; nursery_external=nil; "
         "for i=2,3072 do root[i]={i} end; "
         "local private,edges=nursery_inspect(root); "
         "assert(private and edges==1); "
         "local function attach(t) "
         "  local child={42}; "
         "  t[1]=nil; t[2]=child; t[3]=('keep'):rep(33); t[child]=17 "
         "end; "
         "attach(root); "
         "for i=3073,5000 do root[i]=i end; "
         "for i=1,40000 do local scratch={i,i+1} end; "
         "private,edges=nursery_inspect(root); assert(private and edges==0); "
         "assert(root[2][1]==42 and #root[3]==132 and root[5000]==5000); "
         "local latest={43}; root[4]=latest; root[5]=latest; root[latest]=23; "
         "latest=nil; "
         "for i=1,16384 do local scratch={i}; scratch[1]=scratch end; "
         "assert(root[4]==root[5] and root[4][1]==43 and root[root[4]]==23); "
         "return root",
         0);
  luaSC_wait(&f->tasks[0]);
  require(luaSC_collect(f->main, &f->tasks[0], &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1);
  lua_setglobal(f->main, "nursery_result");
  require(
      luaL_dostring(f->main,
                    "local t=nursery_result; local dead=t[3072]; "
                    "t[3072]=nil; nursery_finalized=0; "
                    "nursery_weak=setmetatable({dead,t[1536]},{__mode='v'}); "
                    "setmetatable(dead,{__gc=function(o) "
                    "assert(o[1]==3072); nursery_finalized=nursery_finalized+1 "
                    "end})") == LUA_OK);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(luaL_dostring(f->main,
                        "local t=nursery_result; "
                        "assert(nursery_weak[1]==nil and "
                        "nursery_weak[2]==t[1536] and nursery_finalized==1); "
                        "assert(t[1]==nil and t[2][1]==42 and "
                        "t[3]==('keep'):rep(33) and t[5000]==5000); "
                        "assert(t[t[2]]==17 and t[4]==t[5] and "
                        "t[4][1]==43 and t[t[4]]==23)") == LUA_OK);
  finish(f);
}

static void nursery_root_pressure(struct fixture *f, int generational) {
  lua_State *L;
  int status, results;
  init(f, 2);
  L = f->main;
  luaL_openlibs(L);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  managed(L, "nursery_inspect", nursery_inspect);
  submit(
      f, 0,
      "local id=...; local records={}; "
      "for i=1,1536 do records[i]={marker=i} end; "
      "local function make_record(n) "
      "  local child={n}; "
      "  return {{[child]={['key-'..n]=n}}, ('data-'..n):rep(24), child} "
      "end; "
      "records[1]=make_record(id); "
      "for i=1,38000 do local garbage={i,i+1} end; "
      "local private,edges=nursery_inspect(records); "
      "assert(private and edges>=2); "
      "for i=1,4096 do local garbage={['short-'..i]=i} end; "
      "private,edges=nursery_inspect(records); assert(private and edges<64); "
      "assert(records[1536].marker==1536); "
      "local row=records[1]; "
      "assert(row[1][row[3]]['key-'..id]==id and row[3][1]==id); "
      "assert(row[2]==('data-'..id):rep(24)); return row",
      42);
  luaSC_wait(&f->tasks[0]);
  require(luaSC_collect(L, &f->tasks[0], &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1);
  lua_setglobal(L, "nursery_result");
  lua_gc(L, LUA_GCCOLLECT);
  require(luaL_dostring(L, "local r=nursery_result; "
                           "assert(r[1][r[3]]['key-42']==42 and r[3][1]==42); "
                           "assert(r[2]==('data-42'):rep(24))") == LUA_OK);
  finish(f);
}

static void nursery_retained_roots(struct fixture *f, int generational) {
  lua_State *L;
  init(f, 2);
  L = f->main;
  luaL_openlibs(L);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  managed(L, "nursery_inspect", nursery_inspect);
  require(luaL_dostring(L, "nursery_labels={}; for i=1,24000 do "
                           "nursery_labels[i]=('label-%05d'):format(i) end") ==
          LUA_OK);
  submit(f, 0,
         "local labels=nursery_labels; nursery_labels=nil; "
         "local rows={}; for i=1,24000 do "
         "  rows[i]={sequence=i,label=labels[i]} end; labels=nil; "
         "nursery_ready=true; while not nursery_release do end; "
         "local private,edges=nursery_inspect(rows); "
         "assert(private and edges>=24000); "
         "for i=1,24000 do local row=rows[i]; "
         "  assert(row.sequence==i and row.label==('label-%05d'):format(i)) "
         "end; "
         "nursery_export=rows; nursery_export=nil; rows=nil; "
         "for i=1,20 do "
         "  for j=1,1024 do end; "
         /* Intern through table insertion before counting outgoing edges. */
         "  local label=next({[('next-%05d'):format(i)]=true}); "
         "  local suffix=next({[label..'x']=true}); local row={label,suffix}; "
         "  private,edges=nursery_inspect(row); assert(private and edges==2); "
         "  nursery_export=row; nursery_export=nil; "
         "  assert(row[1]==label and row[2]==label..'x') "
         "end; return 42",
         0);
  nursery_wait(L, "nursery_ready");
  for (unsigned i = 0; i < 4; i++)
    lua_gc(L, LUA_GCSTEP, (size_t)256 * 1024);
  lua_pushboolean(L, 1);
  lua_setglobal(L, "nursery_release");
  require(collect(f, 0, LUA_OK) == 42);
  finish(f);
}

static void nursery_parked_roots(struct fixture *f, int generational) {
  lua_State *L;
  uint64_t epoch;
  init(f, 2);
  L = f->main;
  luaL_openlibs(L);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  managed(L, "nursery_inspect", nursery_inspect);
  lua_pushcfunction(L, blocked_leaf);
  lua_setglobal(L, "nursery_block");
  require(luaL_dostring(L, "nursery_labels={}; "
                           "nursery_weak=setmetatable({},{__mode='v'}); "
                           "for i=1,4096 do local value={i}; "
                           "nursery_weak[i]=value; nursery_labels[i]=value "
                           "end") == LUA_OK);
  submit(f, 0,
         "local labels=nursery_labels; nursery_labels=nil; "
         "local rows={}; for i=1,4096 do "
         "  rows[i]={label=labels[i]} end; labels=nil; "
         "nursery_block(); "
         "local private,edges=nursery_inspect(rows); "
         "assert(private and edges>=4096); "
         "for i=1,4096 do assert(rows[i].label[1]==i) end; return 42",
         0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->native_entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  epoch = luaC_traceepoch(G(L));
  /* Charge explicit work so the idle-heap shortcut cannot skip the cycle. */
  for (unsigned i = 0;
       i < 512 && (luaC_traceepoch(G(L)) == epoch || luaC_tracing(L)); i++)
    require(lua_gc(L, LUA_GCSTEP, (size_t)64 * 1024 * 1024) >= 0);
  require(luaC_traceepoch(G(L)) != epoch);
  require(!luaC_tracing(L));
  require(
      luaL_dostring(L, "for i=1,4096 do assert(nursery_weak[i][1]==i) end") ==
      LUA_OK);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 42);
  lua_gc(L, LUA_GCCOLLECT);
  require(luaL_dostring(L, "assert(next(nursery_weak)==nil)") == LUA_OK);
  finish(f);
}

static void nursery_shared_store(struct fixture *f, int generational) {
  lua_State *L;
  init(f, 2);
  L = f->main;
  luaL_openlibs(L);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  managed(L, "nursery_inspect", nursery_inspect);
  require(luaL_dostring(L, "sink={}; redirects={}; "
                           "proxy=setmetatable({}, {__newindex=redirects})") ==
          LUA_OK);
  submit(f, 0,
         "local receiver, target = sink, proxy; "
         "local keep = {answer=42}; assert(nursery_inspect(keep)); "
         "for i=1,8 do "
         "  receiver.named={number=i}; "
         "  receiver[1]={i}; "
         "  receiver[i+1]={number=i, spare=false}; "
         "  keep.child={number=i}; assert(nursery_inspect(keep.child)); "
         "end; "
         "local answer=keep.answer; target.changed={answer=answer}; "
         "assert(target.changed==nil and nursery_inspect(keep)); "
         "return keep.child.number",
         0);
  require(collect(f, 0, LUA_OK) == 8);
  lua_gc(L, LUA_GCCOLLECT);
  require(luaL_dostring(L,
                        "assert(sink.named.number==8 and sink[1][1]==8); "
                        "for i=1,8 do "
                        "  assert(sink[i+1].number==i and "
                        "         sink[i+1].spare==false) "
                        "end; assert(redirects.changed.answer==42)") == LUA_OK);
  finish(f);
}

static void nursery_layout_roots(struct fixture *f, int generational) {
  lua_State *L;
  int status, results;
  init(f, 2);
  L = f->main;
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  managed(L, "nursery_layout_count", nursery_layout_count);
  managed(L, "nursery_marking", nursery_marking);
  require(luaL_dostring(
              L, "layout_keep=assert(load('return {answer=42,spare=17}')); "
                 "layout_makers={}; for i=1,64 do "
                 "  layout_makers[i]=assert(load('return {field' "
                 "    ..i..'=1,spare=17}')) end") == LUA_OK);
  submit(f, 0,
         "local record=layout_keep(); layout_keep=nil; "
         "assert(nursery_layout_count(record)==1); "
         "local rows={}; for i=1,24000 do rows[i]={i} end; "
         "for i=1,64 do layout_makers[i](); "
         "  for j=1,1024 do end; while nursery_marking() do end "
         "end; layout_makers=nil; "
         "local count=nursery_layout_count(record); "
         "assert(count>=1 and count<32 and record.answer==42); "
         "assert(rows[24000][1]==24000); layout_ready=true; "
         "while not layout_release do end; "
         "assert(record.answer==42 and record.spare==17); "
         "record.answer={51}; return record",
         0);
  nursery_wait(L, "layout_ready");
  for (unsigned i = 0; i < 4; i++)
    lua_gc(L, LUA_GCSTEP, (size_t)256 * 1024);
  lua_pushboolean(L, 1);
  lua_setglobal(L, "layout_release");
  luaSC_wait(&f->tasks[0]);
  require(luaSC_collect(L, &f->tasks[0], &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_getfield(L, -1, "answer") == LUA_TTABLE);
  require(lua_geti(L, -1, 1) == LUA_TNUMBER && lua_tointeger(L, -1) == 51);
  finish(f);
}

static void nursery_mark(struct fixture *f, int generational, int publish) {
  init(f, 2);
  lua_gc(f->main, generational ? LUA_GCGEN : LUA_GCINC);
  managed(f->main, "nursery_inspect", nursery_inspect);
  managed(f->main, "nursery_marking", nursery_marking);
  managed(f->main, "nursery_resize", nursery_resize);
  require(luaL_dostring(f->main, "nursery_external={answer=42}") == LUA_OK);
  submit(f, 0,
         "local publish=...; local graph={}; local last; "
         "for i=1,131072 do graph[i]={i}; "
         "  if i%64==0 and nursery_marking() then last=i; break end end; "
         "assert(last and nursery_inspect(graph)); "
         "local stackonly={89}; "
         "graph[1][2]={57}; graph[1][3]=nursery_external; "
         "nursery_external=nil; graph[2]=nil; nursery_resize(graph); "
         "assert(nursery_marking()); "
         "if publish==1 then collectgarbage('collect'); "
         "  assert(not nursery_marking() and not nursery_inspect(graph)); "
         "else for j=1,128 do "
         "    if not nursery_marking() then break end; "
         "    local n=0; for i=1,1024 do n=n+i end; "
         "  end; assert(not nursery_marking() and nursery_inspect(graph)); "
         "end; "
         "for i=1,12000 do local row={i,i+1}; assert(row[2]==i+1) end; "
         "assert(graph[2]==nil and graph[last][1]==last); "
         "assert(graph[1][3].answer==42 and stackonly[1]==89); "
         "return graph[1][2][1]",
         publish);
  require(collect(f, 0, LUA_OK) == 57);
  finish(f);
}

static void nursery_full(struct fixture *f, int generational) {
  lua_State *L;
  init(f, 3);
  L = f->main;
  luaL_openlibs(L);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  managed(L, "nursery_inspect", nursery_inspect);
  require(luaL_dostring(L, "ready, done = {}, {}") == LUA_OK);
  submit(f, 0,
         "local graph={}; graph.self=graph; "
         "for i=1,65536 do graph[i]={key=i} end; "
         "graph.label=('local root'):rep(16); "
         "assert(nursery_inspect(graph)); ready[0]=true; "
         "while not (done[1] and done[2]) do end; "
         "assert(not nursery_inspect(graph)); "
         "assert(graph.self==graph and graph.label==('local root'):rep(16)); "
         "assert(graph[1].key==1 and graph[32768].key==32768); "
         "return graph[65536].key",
         0);
  for (unsigned id = 1; id <= 2; id++)
    submit(f, id,
           "local id=...; local graph={answer=42}; "
           "assert(nursery_inspect(graph)); ready[id]=true; "
           "while not start_full do end; collectgarbage('collect'); "
           "assert(not nursery_inspect(graph)); done[id]=true; "
           "return graph.answer",
           (int)id);
  require(luaL_dostring(L, "while not (ready[0] and ready[1] and ready[2]) "
                           "do end; start_full=true") == LUA_OK);
  require(collect(f, 0, LUA_OK) == 65536);
  require(collect(f, 1, LUA_OK) == 42);
  require(collect(f, 2, LUA_OK) == 42);
  finish(f);
}

static int full_stage(lua_State *L) {
  struct fixture *f = fixture;
  unsigned next = (unsigned)lua_tointeger(L, 1);
  unsigned stage;
  require(pthread_mutex_lock(&f->mutex) == 0);
  if (next != 0) {
    require(next == f->full_stage + 1);
    f->full_stage = next;
    require(pthread_cond_broadcast(&f->changed) == 0);
  }
  stage = f->full_stage;
  require(pthread_mutex_unlock(&f->mutex) == 0);
  lua_pushinteger(L, stage);
  return 1;
}

static void *collect_full(void *arg) {
  require(lua_gc(arg, LUA_GCCOLLECT) == 0);
  return NULL;
}

static void nursery_full_cutoff(struct fixture *f, int generational) {
  pthread_t first, second;
  lua_State *L;
  init(f, 1);
  L = f->main;
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  f->metadata_peer = lua_newthread(L);
  managed(L, "nursery_inspect", nursery_inspect);
  managed(L, "full_stage", full_stage);
  f->hold_full = 1;
  submit(
      f, 0,
      "local old={answer=40}; assert(nursery_inspect(old)); full_stage(1); "
      "while full_stage()<2 do end; assert(not nursery_inspect(old)); "
      "for i=1,4096 do end; "
      "local young={answer=2}; assert(nursery_inspect(young)); full_stage(3); "
      "while full_stage()<5 do end; assert(not nursery_inspect(young)); "
      "return old.answer+young.answer",
      0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->full_stage < FULL_OLD_READY)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(pthread_create(&first, NULL, collect_full, L) == 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->full_stage < FULL_NEW_READY)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(pthread_create(&second, NULL, collect_full, f->metadata_peer) == 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->metadata_queued)
    luaQ_wait(L, &f->mutex, &f->changed, &f->waiters);
  f->full_stage = FULL_RESUME;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(pthread_join(first, NULL) == 0);
  require(pthread_join(second, NULL) == 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->full_stage = FULL_COMPLETE;
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(collect(f, 0, LUA_OK) == 42);
  lua_pop(L, 1);
  finish(f);
}

static void nursery(struct fixture *f, int generational) {
  int status, results;
  init(f, 2);
  luaL_openlibs(f->main);
  lua_gc(f->main, generational ? LUA_GCGEN : LUA_GCINC);
  lua_pushcfunction(f->main, nursery_native);
  lua_setglobal(f->main, "nursery_native");
  submit(f, 0,
         "local n=...; local name='nursery field '..n; "
         "local root={[name]=42}; root.self=root; name=nil; "
         "local function read(k) return root.self[k] end; "
         "rawset(root,'values',{42}); "
         "for _,v in ipairs(rawget(root,'values')) do assert(v==42) end; "
         "for i=1,50000 do local row={i,i+1,kind='row'}; "
         "if i%4096==0 then "
         "  local wide={}; for j=1,512 do wide[j]=j end; "
         "  assert(wide[512]==512); collectgarbage('step',0) "
         "end; "
         "assert(row[2]==i+1) end; "
         "nursery_ready=true; while not nursery_release do end; "
         "local result=read('nursery field '..n); root=nil; "
         "assert(nursery_native({answer=result})==42); "
         "for i=1,5000 do local row={i,i+1}; assert(row[2]==i+1) end; "
         "return result",
         17);
  nursery_wait(f->main, "nursery_ready");
  for (unsigned i = 0; i < 4; i++)
    lua_gc(f->main, LUA_GCSTEP, (size_t)0);
  lua_pushboolean(f->main, 1);
  lua_setglobal(f->main, "nursery_release");
  require(collect(f, 0, LUA_OK) == 42 && f->nursery_finished == 1);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(luaL_dostring(f->main, "assert(nursery_native_root.answer == 42)") ==
          LUA_OK);

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

  submit(f, 5,
         "local root={child={answer=42}}; "
         "nursery_reader=function() return root.child.answer end; "
         "nursery_capture_ready=true; "
         "while not nursery_capture_release do end; "
         "return root.child.answer",
         0);
  nursery_wait(f->main, "nursery_capture_ready");
  lua_gc(f->main, LUA_GCCOLLECT);
  require(lua_getglobal(f->main, "nursery_reader") == LUA_TFUNCTION);
  require(lua_pcall(f->main, 0, 1, 0) == LUA_OK);
  require(lua_tointeger(f->main, -1) == 42);
  lua_pop(f->main, 1);
  lua_pushboolean(f->main, 1);
  lua_setglobal(f->main, "nursery_capture_release");
  require(collect(f, 5, LUA_OK) == 42);

  submit(f, 6,
         "local root={child={answer=42}}; "
         "return function() return root.child.answer end",
         0);
  luaSC_wait(&f->tasks[6]);
  require(luaSC_collect(f->main, &f->tasks[6], &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1);
  lua_gc(f->main, LUA_GCCOLLECT);
  require(lua_pcall(f->main, 0, 1, 0) == LUA_OK);
  require(lua_tointeger(f->main, -1) == 42);
  lua_pop(f->main, 1);
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
  require(nursery_bytes(L) >= 256 * 1024);
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
  else if (strcmp(argv[1], "retained_fairness") == 0)
    retained_fairness(f);
  else if (strcmp(argv[1], "no_requests") == 0)
    no_requests(f);
  else if (strcmp(argv[1], "metadata_progress") == 0)
    metadata_progress(f, 1000000);
  else if (strcmp(argv[1], "timed_metadata_progress") == 0)
    metadata_progress(f, 0);
  else if (strcmp(argv[1], "timed_slice") == 0)
    timed_slice(f, 0, 0);
  else if (strcmp(argv[1], "timed_continuation_slice") == 0)
    timed_slice(f, 1, 0);
  else if (strcmp(argv[1], "timed_retained_slice") == 0)
    timed_slice(f, 1, 1);
  else if (strcmp(argv[1], "timed_retained_start") == 0)
    timed_slice(f, 0, 1);
  else if (strcmp(argv[1], "early_incremental") == 0)
    wake_before_publication(f, 0);
  else if (strcmp(argv[1], "early_generational") == 0)
    wake_before_publication(f, 1);
  else if (strcmp(argv[1], "wake_waiters") == 0)
    wake_waiters(f);
  else if (strcmp(argv[1], "stopped_dispatch") == 0)
    stopped_dispatch(f);
  else if (strcmp(argv[1], "trace_pause_retains_collector") == 0)
    trace_pause(f, 0);
  else if (strcmp(argv[1], "trace_pause_retains_mutators") == 0)
    trace_pause(f, 1);
  else if (strcmp(argv[1], "native") == 0)
    native_progress(f, 0, 0);
  else if (strcmp(argv[1], "native_reentry") == 0)
    native_reentry(f);
  else if (strcmp(argv[1], "native_coroutine_progress") == 0)
    native_progress(f, 0, 1);
  else if (strcmp(argv[1], "shutdown") == 0)
    native_progress(f, 1, 0);
  else if (strcmp(argv[1], "parallel_native_compensation") == 0)
    parallel_native_compensation(f);
  else if (strcmp(argv[1], "native_overlapping_stops") == 0)
    native_overlapping_stops(f);
  else if (strcmp(argv[1], "native_return_compensation") == 0)
    native_return_compensation(f, 0);
  else if (strcmp(argv[1], "native_return_handoff") == 0)
    native_return_compensation(f, 1);
  else if (strcmp(argv[1], "native_leaf_return") == 0)
    native_leaf_return(f);
  else if (strcmp(argv[1], "native_argument_observation") == 0)
    native_argument_observation(f, 0);
  else if (strcmp(argv[1], "native_argument_types") == 0)
    native_argument_observation(f, 1);
  else if (strcmp(argv[1], "native_index_dispatch") == 0)
    native_index_dispatch(f);
  else if (strcmp(argv[1], "native_recall_during_stack_borrow") == 0)
    native_recall_during_stack_borrow(f, 1, 0);
  else if (strcmp(argv[1], "native_recall_without_stack_offer") == 0)
    native_recall_during_stack_borrow(f, 0, 0);
  else if (strcmp(argv[1], "native_recall_with_retirement") == 0)
    native_recall_during_stack_borrow(f, 1, 1);
  else if (strcmp(argv[1], "native_metadata_without_slot") == 0)
    native_metadata_without_slot(f);
  else if (strcmp(argv[1], "native_registry_without_slot") == 0)
    native_registry_without_slot(f);
  else if (strcmp(argv[1], "native_stack_without_slot") == 0)
    native_operation_without_slot(
        f, inspect_native_stack,
        "local noop = function() end; local co = coroutine.create(noop); "
        "native_table_operation(); "
        "return native_table_operation(nil, true, 2, 'key', {}, noop, co, "
        "io.stdout, 9)",
        0);
  else if (strcmp(argv[1], "native_array_without_slot") == 0)
    native_operation_without_slot(
        f, edit_native_array, "return native_table_operation({0}, {51})", 0);
  else if (strcmp(argv[1], "native_allocation_without_slot") == 0)
    native_operation_without_slot(
        f, create_native_array,
        "return native_table_operation({51, 1, 2, 3, 4, 5})[1][1][1]", 1);
  else if (strcmp(argv[1], "native_named_fields_without_slot") == 0)
    native_operation_without_slot(
        f, edit_native_fields,
        "local row = native_table_operation({51}); "
        "assert(rawequal(row.first, row.second)); return row.first[1]",
        1);
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
  else if (strcmp(argv[1], "native_lease_yield") == 0)
    native_lease_yield(f);
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
  else if (strcmp(argv[1], "nursery_edges_incremental") == 0)
    nursery_edges(f, 0);
  else if (strcmp(argv[1], "nursery_edges_generational") == 0)
    nursery_edges(f, 1);
  else if (strcmp(argv[1], "nursery_survivors_incremental") == 0)
    nursery_survivors(f, 0);
  else if (strcmp(argv[1], "nursery_survivors_generational") == 0)
    nursery_survivors(f, 1);
  else if (strcmp(argv[1], "nursery_root_pressure_incremental") == 0)
    nursery_root_pressure(f, 0);
  else if (strcmp(argv[1], "nursery_root_pressure_generational") == 0)
    nursery_root_pressure(f, 1);
  else if (strcmp(argv[1], "nursery_retained_roots_incremental") == 0)
    nursery_retained_roots(f, 0);
  else if (strcmp(argv[1], "nursery_retained_roots_generational") == 0)
    nursery_retained_roots(f, 1);
  else if (strcmp(argv[1], "nursery_parked_roots_incremental") == 0)
    nursery_parked_roots(f, 0);
  else if (strcmp(argv[1], "nursery_parked_roots_generational") == 0)
    nursery_parked_roots(f, 1);
  else if (strcmp(argv[1], "nursery_layout_roots_incremental") == 0)
    nursery_layout_roots(f, 0);
  else if (strcmp(argv[1], "nursery_layout_roots_generational") == 0)
    nursery_layout_roots(f, 1);
  else if (strcmp(argv[1], "nursery_shared_store_incremental") == 0)
    nursery_shared_store(f, 0);
  else if (strcmp(argv[1], "nursery_shared_store_generational") == 0)
    nursery_shared_store(f, 1);
  else if (strcmp(argv[1], "nursery_native_incremental") == 0)
    nursery_native_call(f, 0);
  else if (strcmp(argv[1], "nursery_native_generational") == 0)
    nursery_native_call(f, 1);
  else if (strcmp(argv[1], "nursery_native_allocation_incremental") == 0)
    nursery_native_allocation(f, 0);
  else if (strcmp(argv[1], "nursery_native_allocation_generational") == 0)
    nursery_native_allocation(f, 1);
  else if (strcmp(argv[1], "nursery_native_strings_incremental") == 0)
    nursery_native_strings(f, 0);
  else if (strcmp(argv[1], "nursery_native_strings_generational") == 0)
    nursery_native_strings(f, 1);
  else if (strcmp(argv[1], "nursery_full_incremental") == 0)
    nursery_full(f, 0);
  else if (strcmp(argv[1], "nursery_mark_incremental") == 0)
    nursery_mark(f, 0, 0);
  else if (strcmp(argv[1], "nursery_mark_generational") == 0)
    nursery_mark(f, 1, 0);
  else if (strcmp(argv[1], "nursery_mark_publish_incremental") == 0)
    nursery_mark(f, 0, 1);
  else if (strcmp(argv[1], "nursery_mark_publish_generational") == 0)
    nursery_mark(f, 1, 1);
  else if (strcmp(argv[1], "nursery_full_generational") == 0)
    nursery_full(f, 1);
  else if (strcmp(argv[1], "nursery_full_cutoff_incremental") == 0)
    nursery_full_cutoff(f, 0);
  else if (strcmp(argv[1], "nursery_full_cutoff_generational") == 0)
    nursery_full_cutoff(f, 1);
  else if (strcmp(argv[1], "native_accounting_count") == 0)
    native_accounting(f, 0);
  else if (strcmp(argv[1], "native_accounting_arm") == 0)
    native_accounting(f, 1);
  else
    abort();
  require(live_stacks == 0);
  free(f);
  return 0;
}
