#define _GNU_SOURCE
#include <errno.h>
#include <lapi.h>
#include <lauxlib.h>
#include <lscheduler.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

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
  unsigned pause_hold, pause_seen, pause_release;
  unsigned finalized;
  unsigned native_callback_done;
  unsigned idle_entries, host_channel_entries;
  lua_State *metadata_peer;
  unsigned metadata_held, metadata_queued, metadata_released;
  size_t live;
  int inspect_polls, shutting_down, parallel_check, deny;
};
static struct fixture *fixture;
static __thread int inspecting;
static __thread int measuring_reentry;
static __thread unsigned reentry_locks;
static unsigned poll_locks, poll_calls;
static int fail_mutex = -1, fail_condition = -1, fail_carrier = -1;
static int fail_mapping, fail_protection;
static unsigned live_stacks;
static int observe_idle;
static pthread_t idle_host;

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
  if (inspecting)
    __atomic_fetch_add(&poll_locks, 1, __ATOMIC_RELAXED);
  if (measuring_reentry && mutex == &fixture->scheduler.mutex)
    reentry_locks++;
  return __real_pthread_mutex_lock(mutex);
}
void __real_luaX_poll(lua_State *);
void __wrap_luaX_poll(lua_State *L) {
  if (fixture->inspect_polls && luaQ_current() != NULL) {
    inspecting = 1;
    __atomic_fetch_add(&poll_calls, 1, __ATOMIC_RELAXED);
    __real_luaX_poll(L);
    inspecting = 0;
  } else
    __real_luaX_poll(L);
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
  if ((f->pause_hold || task->root.thread == f->metadata_peer) &&
      task->waiter != NULL) {
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

static void init(struct fixture *f, unsigned slots) {
  LuaSchedulerConfig config = {slots, slots + 1, 1, 1, 1};
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
  require(f->scheduler.created == slots + 1);
  if (slots == 2) {
    f->parallel_check = 1;
    require(pthread_barrier_init(&f->parallel, NULL, 2) == 0);
  }
}

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

static void metadata_progress(struct fixture *f) {
  lua_State *owner, *peer;
  init(f, 1);
  f->scheduler.config.quantum_polls = 1000000;
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
    int status = pthread_barrier_wait(&fixture->parallel);
    require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
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
  lua_pushinteger(L, 77);
  return 1;
}

static int local_api(lua_State *L) {
  pthread_t carrier = pthread_self();
  lua_Integer sum = 0;
  unsigned i;
  reentry_locks = 0;
  measuring_reentry = 1;
  for (i = 0; i < 2000; i++) {
    lua_pushinteger(L, (lua_Integer)i);
    sum += lua_tointeger(L, -1);
    lua_settop(L, 0);
  }
  measuring_reentry = 0;
  require(sum == 1999000 && pthread_equal(pthread_self(), carrier));
  require(reentry_locks == 0);
  lua_pushinteger(L, sum);
  return 1;
}

static void native_reentry(struct fixture *f) {
  init(f, 1);
  lua_pushcfunction(f->main, local_api);
  lua_setglobal(f->main, "local_api");
  submit(f, 0, "return local_api()", 0);
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

static int block_binding(lua_State *L) {
  struct fixture *f = fixture;
  pthread_t carrier = pthread_self();
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->native_entered++;
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

static void native_saturation(struct fixture *f, int cancel) {
  LuaScheduler *s;
  int status = -1, results = -1;
  init(f, 1);
  s = &f->scheduler;
  lua_pushcfunction(f->main, block_binding);
  lua_setglobal(f->main, "block_binding");
  submit(f, 0, "return block_binding()", 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (f->native_entered == 0)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  submit(f, 1, "return block_binding()", 0);
  require(pthread_mutex_lock(&s->native_mutex) == 0);
  while (s->native_waiters == NULL) {
    require(pthread_mutex_unlock(&s->native_mutex) == 0);
    sched_yield();
    require(pthread_mutex_lock(&s->native_mutex) == 0);
  }
  require(s->native == 1 && f->native_entered == 1);
  require(pthread_mutex_unlock(&s->native_mutex) == 0);
  submit(f, 2, "return 42", 0);
  require(collect(f, 2, LUA_OK) == 42);
  if (cancel) {
    require(luaTK_requeststop(&f->tasks[1].root) == LUA_OK);
    (void)collect(f, 1, LUA_ERRRUN);
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
  if (!cancel)
    require(collect(f, 1, LUA_OK) == 500500);
  require(f->native_entered == (cancel ? 1u : 2u));
  finish(f);
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

static int callback_done(lua_State *L) {
  lua_pushboolean(
      L, __atomic_load_n(&fixture->native_callback_done, __ATOMIC_ACQUIRE));
  return 1;
}

static void callback_progress(struct fixture *f) {
  init(f, 1);
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
  for (i = 0; i < 4; i++) {
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
    metadata_progress(f);
  else if (strcmp(argv[1], "early_incremental") == 0)
    wake_before_publication(f, 0);
  else if (strcmp(argv[1], "early_generational") == 0)
    wake_before_publication(f, 1);
  else if (strcmp(argv[1], "native") == 0)
    native_progress(f, 0);
  else if (strcmp(argv[1], "native_reentry") == 0)
    native_reentry(f);
  else if (strcmp(argv[1], "shutdown") == 0)
    native_progress(f, 1);
  else if (strcmp(argv[1], "yield") == 0)
    voluntary(f);
  else if (strcmp(argv[1], "saturation") == 0)
    native_saturation(f, 0);
  else if (strcmp(argv[1], "cancel_admission") == 0)
    native_saturation(f, 1);
  else if (strcmp(argv[1], "startup_failures") == 0)
    startup_failures(f);
  else if (strcmp(argv[1], "idle_notifications") == 0)
    idle_notifications(f);
  else if (strcmp(argv[1], "idle_affinity") == 0)
    idle_affinity(f);
  else if (strcmp(argv[1], "callback_progress") == 0)
    callback_progress(f);
  else if (strcmp(argv[1], "parked") == 0)
    parked_wait(f, 0);
  else if (strcmp(argv[1], "cancel_parked") == 0)
    parked_wait(f, 1);
  else
    abort();
  require(live_stacks == 0);
  free(f);
  return 0;
}
