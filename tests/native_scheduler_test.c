#define _GNU_SOURCE
#include "private/context.h"
#include <laccess.h>
#include <lauxlib.h>
#include <lcallback.h>
#include <ldo.h>
#include <lgc.h>
#include <lpipe.h>
#include <ltable.h>
#include <ltask.h>
#include <lualib.h>
#include <luthrum/lua_runtime.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

enum {
  MANAGED = 1,
  NATIVE = 2,
  WORKERS = MANAGED + NATIVE,
  TASKS = 6,
  MAX_WORKERS = 4,
  MAX_TASKS = 8,
  MAX_HELPERS = 11
};
enum reason { READY, NATIVE_WAIT, RESOURCE_WAIT, DONE };
enum wait_kind { OBJECT_WAIT, GATE_WAIT, GC_WAIT, EARLY_WAIT };
struct pool;
struct task {
  struct pool *pool;
  struct test_fiber fiber;
  LuaTaskRoot root;
  lua_State *state;
  struct task *next;
  enum reason reason;
  int permit, native, pinned, finished, status, results, id;
  int worker, target;
  unsigned parks, migrations, pinned_waits;
  long last_carrier;
  LuaWaiter *waiter;
  int notified, published, wait_pinned;
  unsigned resource_waits, pinned_resource_waits, early_wakes;
  unsigned parallel_seen, await_waits;
  unsigned helper_polls[MAX_HELPERS], helper_migrations[MAX_HELPERS],
      helper_pinned[MAX_HELPERS];
  int helper_hold, helper_held;
  int parallel_cpu;
  void (*run)(struct task *);
  void (*before_wait)(struct task *);
};
struct worker {
  struct pool *pool;
  struct test_context context;
  unsigned dispatches;
  int bind_cpu, cpu;
};
struct pool {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  struct task tasks[MAX_TASKS];
  struct worker workers[MAX_WORKERS];
  pthread_barrier_t parallel;
  struct task *requests, *requests_tail, *ready, *ready_tail;
  struct task *native_wait, *native_tail;
  lua_State *main;
  luthrum_lua_runtime *runtime;
  unsigned managed, native, managed_peak, native_peak, native_waits;
  unsigned managed_limit, parallel_arrivals, await_winners[2], await_losers[2];
  unsigned await_closed, await_ready[2];
  void *await_payloads[2];
  int awaiting;
  lua_CFunction helper_work[MAX_HELPERS];
  int helper_count;
  unsigned done, entered, callback_done, release[2], finalized;
  int callback_request, stop, budget, trace;
  unsigned observed;
  Table *shared;
  enum wait_kind wait_kind;
  int force_migrate, stage, native_resource;
  size_t stream_live, stream_peak;
  unsigned generated, consumed;
  int stream_foreign;
  unsigned char stream_seen[1024];
};
static __thread int native_marker;
static struct pool *fixture;

static void check(int code) {
  if (code != 0)
    abort();
}
#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)
static void lock(struct pool *pool) { check(pthread_mutex_lock(&pool->mutex)); }
static void unlock(struct pool *pool) {
  check(pthread_mutex_unlock(&pool->mutex));
}
static void signal_change(struct pool *pool) {
  check(pthread_cond_broadcast(&pool->changed));
}
static void wait_change(struct pool *pool) {
  check(pthread_cond_wait(&pool->changed, &pool->mutex));
}
static void append(struct task **head, struct task **tail, struct task *task) {
  require(task->next == NULL);
  if (*tail != NULL)
    (*tail)->next = task;
  else
    *head = task;
  *tail = task;
}
static struct task *take(struct task **head, struct task **tail) {
  struct task *task = *head;
  require(task != NULL);
  *head = task->next;
  if (*head == NULL)
    *tail = NULL;
  task->next = NULL;
  return task;
}
static void grant(struct pool *pool) {
  unsigned limit = pool->managed_limit == 0 ? MANAGED : pool->managed_limit;
  while (pool->managed < limit && pool->requests != NULL) {
    struct task *task = take(&pool->requests, &pool->requests_tail);
    require(!task->permit);
    task->permit = 1;
    pool->managed++;
    if (pool->managed > pool->managed_peak)
      pool->managed_peak = pool->managed;
    if (!task->pinned)
      append(&pool->ready, &pool->ready_tail, task);
  }
  signal_change(pool);
}
static void release_permit(struct task *task) {
  struct pool *pool = task->pool;
  require(task->permit && pool->managed > 0);
  task->permit = 0;
  pool->managed--;
  grant(pool);
}
static void park(struct task *task, enum reason reason) {
  long previous = syscall(SYS_gettid);
  task->reason = reason;
  if (task->pool->force_migrate) {
    lock(task->pool);
    if (!task->pool->awaiting)
      task->target = 1 - task->worker;
    else if (task->id >= 2 && task->pool->parallel_arrivals == 2)
      task->target = task->worker == 2 ? 3 : 2;
    unlock(task->pool);
  }
  task->parks++;
  test_fiber_pause(&task->fiber);
  luaD_rebindexecution(&task->fiber.execution);
  if (previous != syscall(SYS_gettid))
    task->migrations++;
  task->last_carrier = syscall(SYS_gettid);
}
static void managed_enter(LuaExecution *execution, lua_State *state) {
  struct task *task = execution->context;
  struct pool *pool = task->pool;
  (void)state;
  lock(pool);
  if (!task->permit) {
    if (execution->native_depth != 0) {
      /* A nested callback waits on its physical carrier, preserving C/TLS. */
      task->pinned = 1;
      task->pinned_waits++;
      append(&pool->requests, &pool->requests_tail, task);
      grant(pool);
      while (!task->permit)
        wait_change(pool);
      task->pinned = 0;
    } else {
      unlock(pool);
      park(task, READY);
      lock(pool);
    }
  }
  require(task->permit);
  unlock(pool);
}
static void managed_leave(LuaExecution *execution) {
  struct task *task = execution->context;
  lock(task->pool);
  release_permit(task);
  unlock(task->pool);
}
static void native_enter(LuaExecution *execution, lua_State *state) {
  struct task *task = execution->context;
  struct pool *pool = task->pool;
  LuaWorldScope scope = {0, 0};
  int managed = execution->managed_depth != 0;
  if (execution->native_depth != 0) {
    require(task->native);
    return;
  }
  lock(pool);
  if (pool->native == NATIVE) {
    pool->native_waits++;
    unlock(pool);
    if (managed)
      scope = luaW_detach(state);
    park(task, NATIVE_WAIT);
    require(task->native);
    if (managed)
      luaW_attach(state, scope);
    else {
      lock(pool);
      release_permit(task);
      unlock(pool);
    }
    return;
  }
  require(!task->native);
  task->native = 1;
  pool->native++;
  if (pool->native > pool->native_peak)
    pool->native_peak = pool->native;
  unlock(pool);
}
static void native_leave(LuaExecution *execution, lua_State *state) {
  struct task *task = execution->context;
  struct pool *pool = task->pool;
  (void)state;
  if (execution->native_depth != 0)
    return;
  lock(pool);
  if (task->native) {
    require(pool->native > 0);
    task->native = 0;
    pool->native--;
    if (pool->native_wait != NULL) {
      struct task *waiting = take(&pool->native_wait, &pool->native_tail);
      require(!waiting->native);
      waiting->native = 1;
      pool->native++;
      append(&pool->requests, &pool->requests_tail, waiting);
      grant(pool);
    }
  }
  unlock(pool);
}
static void native_restore(LuaExecution *execution, lua_State *state,
                           unsigned previous) {
  (void)previous;
  native_leave(execution, state);
}
static void poll(LuaExecution *execution, lua_State *state) {
  struct task *task = execution->context;
  long carrier = syscall(SYS_gettid);
  int helper = -1, index, collect = 0;
  if (!isLua(state->ci) && ttisCclosure(s2v(state->ci->func.p))) {
    lua_CFunction function = clCvalue(s2v(state->ci->func.p))->f;
    for (index = 0; index < task->pool->helper_count; ++index)
      if (function == task->pool->helper_work[index]) {
        helper = index;
        collect = task->helper_polls[index]++ == 0;
        if (execution->native_depth != 0)
          task->helper_pinned[index]++;
        break;
      }
  }
  LuaWorldScope scope = luaW_detach(state);
  if (helper == 0 && collect && (task->id == 2 || task->id == 3))
    task->helper_hold = 1;
  if (execution->native_depth == 0)
    park(task, READY);
  if (helper >= 0 && carrier != syscall(SYS_gettid))
    task->helper_migrations[helper]++;
  if (collect)
    (void)lua_gc(state, LUA_GCCOLLECT); /* Retained helper arguments/buffers. */
  luaW_attach(state, scope);
}
static void resource_wait(LuaExecution *execution, pthread_mutex_t *mutex,
                          LuaWaiter *waiter) {
  struct task *task = execution->context;
  struct pool *pool = task->pool;
  lock(pool);
  require(task->waiter == NULL && !task->permit);
  task->waiter = waiter;
  if (pool->awaiting && mutex == &G(pool->main)->world.mutex &&
      (pool->tasks[2].root.waiters == waiter ||
       pool->tasks[3].root.waiters == waiter))
    task->await_waits++;
  task->notified = task->published = 0;
  task->resource_waits++;
  task->wait_pinned = execution->native_depth != 0;
  check(pthread_mutex_unlock(mutex));
  signal_change(pool);
  if (task->wait_pinned) {
    task->pinned_resource_waits++;
    while (!task->notified)
      wait_change(pool);
    task->waiter = NULL;
    unlock(pool);
  } else {
    unlock(pool);
    if (task->before_wait != NULL)
      task->before_wait(task);
    park(task, RESOURCE_WAIT);
    lock(pool);
    require(task->notified);
    task->waiter = NULL;
    unlock(pool);
  }
}
static void resource_wake(LuaExecution *execution, LuaWaiter *waiter) {
  struct task *task = execution->context;
  struct pool *pool = task->pool;
  lock(pool);
  require(task->waiter == waiter && !task->notified);
  task->notified = 1;
  if (!task->wait_pinned) {
    if (task->published) {
      task->published = 0;
      append(&pool->requests, &pool->requests_tail, task);
      grant(pool);
    } else
      task->early_wakes++;
  }
  signal_change(pool);
  unlock(pool);
}
static void task_entry(void *argument) {
  struct task *task = argument;
  if (task->run != NULL)
    task->run(task);
  else {
    task->status =
        luaTK_resume(&task->root, task->id < 2 ? 2 : 0, &task->results);
    if (task->id == 4 && task->status == LUA_YIELD)
      task->status = luaTK_resume(&task->root, 0, &task->results);
  }
  require(task->fiber.execution.managed_depth == 0);
  require(task->fiber.execution.native_depth == 0);
  require(task->fiber.execution.protected == NULL);
  require(task->fiber.native.coroutine_scopes == NULL);
  require(task->state == NULL ||
          (task->state->owned_operations == 0 &&
           task->state->resume_active == 0 && task->state->close_active == 0));
  /* Collection may remove the last root immediately after finish publishes.
   * No state access is permitted below this point without another Lua root. */
  if (task->root.global != NULL)
    require(luaTK_finish(&task->root, &task->status, &task->results) == LUA_OK);
  lock(task->pool);
  if (task->permit)
    release_permit(task); /* A private probe may finish without a Lua entry. */
  require(!task->native && task->waiter == NULL);
  unlock(task->pool);
  task->reason = DONE;
  test_fiber_pause(&task->fiber);
  abort();
}
static void *worker_entry(void *argument) {
  struct worker *worker = argument;
  struct pool *pool = worker->pool;
  if (worker->bind_cpu) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    CPU_SET(worker->cpu, &allowed);
    check(pthread_setaffinity_np(pthread_self(), sizeof(allowed), &allowed));
  }
  for (;;) {
    struct task *task = NULL;
    lock(pool);
    while (!pool->stop && task == NULL) {
      struct task **link = &pool->ready, *previous = NULL;
      while (*link != NULL) {
        if ((*link)->target < 0 ||
            (*link)->target == (int)(worker - pool->workers)) {
          task = *link;
          *link = task->next;
          if (pool->ready_tail == task)
            pool->ready_tail = previous;
          task->next = NULL;
          break;
        }
        previous = *link;
        link = &(*link)->next;
      }
      if (task == NULL)
        wait_change(pool);
    }
    if (pool->stop) {
      unlock(pool);
      return NULL;
    }
    task->worker = (int)(worker - pool->workers);
    unlock(pool);
    worker->dispatches++;
    test_fiber_resume(&task->fiber, &worker->context);
    require(luaQ_current() == NULL);
    lock(pool);
    /* Publish only after saving the complete outgoing context. */
    if (task->reason == DONE) {
      task->finished = 1;
      pool->done++;
    } else if (task->reason == NATIVE_WAIT) {
      if (pool->native < NATIVE) {
        task->native = 1;
        pool->native++;
        append(&pool->requests, &pool->requests_tail, task);
      } else
        append(&pool->native_wait, &pool->native_tail, task);
    } else if (task->reason == RESOURCE_WAIT) {
      if (task->notified)
        append(&pool->requests, &pool->requests_tail, task);
      else
        task->published = 1;
    } else if (task->helper_hold) {
      task->helper_hold = 0;
      task->helper_held = 1;
    } else
      append(&pool->requests, &pool->requests_tail, task);
    grant(pool);
    unlock(pool);
  }
}

/* Ordinary C bindings: no scheduler calls or explicit application yields. */
static int block_binding(lua_State *state) {
  struct pool *pool = fixture;
  int id = (int)lua_tointeger(state, 1);
  unsigned *payload = lua_touserdata(state, 2);
  long carrier = syscall(SYS_gettid);
  require(id >= 0 && id < 2 && payload != NULL && *payload == 12345U);
  native_marker = id + 1;
  lock(pool);
  pool->entered++;
  signal_change(pool);
  while (!pool->release[id]) {
    if (id == 0 && pool->callback_request && !pool->callback_done) {
      lua_State *child;
      int results;
      unlock(pool);
      /* A foreign activation on the parent also pins the child's VM work. */
      child = lua_newthread(state);
      require(
          luaL_loadstring(
              child,
              pool->awaiting
                  ? "if audit_utf8 then audit_utf8() end; "
                    "if audit_patterns then audit_patterns(0) end; "
                    "if audit_strings then audit_strings(0) end; "
                    "if audit_dump then audit_dump(0) end; "
                    "if audit_load then audit_load(0) end; "
                    "if audit_package then audit_package(0) end; "
                    "return await_checked(2)"
                  : "local n=0; for i=1,4000 do n=n+i end; "
                    "local bytes=string.dump(function() return 42 end); "
                    "local f=assert(load(bytes)); if f()~=42 then return -2 "
                    "end; "
                    "warn('nested warning'); "
                    "local ok=pcall(function() error('nested expected') end); "
                    "if ok then return -1 end; return n") == LUA_OK);
      require(lua_resume(child, NULL, 0, &results) == LUA_OK);
      require(results == 1 &&
              lua_tointeger(child, -1) == (pool->awaiting ? 42 : 8002000));
      lua_pop(state, 1);
      require(syscall(SYS_gettid) == carrier && native_marker == id + 1);
      lock(pool);
      pool->callback_done = 1;
      signal_change(pool);
    } else
      wait_change(pool);
  }
  unlock(pool);
  require(syscall(SYS_gettid) == carrier && native_marker == id + 1);
  require(*payload == 12345U);
  lua_pushinteger(state, id + 10);
  return 1;
}
static int failing_binding(lua_State *state) {
  return luaL_error(state, "admitted expected");
}
static int continuation(lua_State *state, int status, lua_KContext context) {
  require(status == LUA_YIELD && context == 123);
  lua_pushinteger(state, 42);
  return 1;
}
static int yielding_binding(lua_State *state) {
  return lua_yieldk(state, 0, 123, continuation);
}
static int warmup(lua_State *state) { return lua_yield(state, 0); }
static void failing_hook(lua_State *state, lua_Debug *record) {
  require(record->event == LUA_HOOKCOUNT);
  require(luaQ_current()->native_depth != 0);
  luaL_error(state, "hook expected");
}
static int finalize(lua_State *state) {
  unsigned *payload = lua_touserdata(state, 1);
  require(payload != NULL && *payload == 12345U);
  fixture->finalized++;
  return 0;
}
static int open_fixture(void *opaque) {
  fixture->main = opaque;
  return 0;
}
static void observe(void *context,
                    const luthrum_lua_runtime_trace_event *event) {
  struct pool *pool = context;
  LuaExecution *execution = luaQ_current();
  require(event->line > 0);
  if (execution != NULL) {
    struct task *task = execution->context;
    require(execution->native_depth != 0 && task->native);
    /* This callback is ordinary native work, including inside another binding.
     */
    lock(pool);
    pool->observed++;
    unlock(pool);
  }
}
static void submit(struct pool *pool, int id) {
  lock(pool);
  append(&pool->requests, &pool->requests_tail, &pool->tasks[id]);
  grant(pool);
  unlock(pool);
}
static void init_execution(struct task *task) {
  task->fiber.execution.context = task;
  task->fiber.execution.managed_enter = managed_enter;
  task->fiber.execution.managed_leave = managed_leave;
  task->fiber.execution.native_enter = native_enter;
  task->fiber.execution.native_leave = native_leave;
  task->fiber.execution.native_restore = native_restore;
  task->fiber.execution.poll = poll;
  task->fiber.execution.wait = resource_wait;
  task->fiber.execution.wake = resource_wake;
}
static void init_task(struct pool *pool, struct task *task, int id,
                      void (*run)(struct task *)) {
  task->pool = pool;
  task->id = id;
  task->target = -1;
  task->run = run;
  check(test_fiber_create(&task->fiber, task_entry, task));
  init_execution(task);
  if (run == NULL)
    require(luaTK_admit(task->state, &task->root, &task->fiber.execution) ==
            LUA_OK);
}
static void collect_task(struct task *task) {
  if (task->root.global != NULL) {
    int status, results;
    require(luaTK_collect(task->pool->main, &task->root, &status, &results) ==
            LUA_OK);
    require(status == task->status && results == task->results);
    lua_pop(task->pool->main, results);
  }
}
static void resource_body(lua_State *state, void *argument) {
  struct task *task = argument;
  struct pool *pool = task->pool;
  LuaObjectScope scope;
  TValue value;
  if (pool->wait_kind == GC_WAIT) {
    luaC_fullgc(state, 0);
    luaD_throw(state,
               LUA_ERRMEM); /* moved protected receiver must still work */
  }
  luaA_begin(state, obj2gco(pool->shared), &scope);
  if (task->id == 0) {
    luaW_heapenter(state); /* Retain object ownership while metadata is busy. */
    setivalue(&value, 11);
    luaH_setint(state, pool->shared, 1, &value);
    luaW_heapleave(state);
  } else {
    require(luaH_getint(state, pool->shared, 1, &value) == LUA_VNUMINT);
    require(ivalue(&value) == 11);
    setivalue(&value, 22);
    luaH_setint(state, pool->shared, 1, &value);
  }
  luaA_end(state, &scope);
}
static void run_resource_task(struct task *task) {
  struct pool *pool = task->pool;
  if ((task->id == 2 || (task->id == 1 && pool->wait_kind == EARLY_WAIT)) &&
      pool->wait_kind != GC_WAIT) {
    task->status = lua_resume(task->state, NULL, 0, &task->results);
    return;
  }
  if (task->id == 2 && pool->wait_kind == GC_WAIT) {
    LuaWorld *world = &G(pool->main)->world;
    check(pthread_mutex_lock(&world->mutex));
    require(world->stopped && world->heap_depth != 0);
    require(!luaQ_isowner(&world->heap_owner));
    require(!luaQ_isowner(&world->collector));
    check(pthread_mutex_unlock(&world->mutex));
    task->status = LUA_OK;
    return;
  }
  if (task->id == 1 && pool->wait_kind == GATE_WAIT) {
    lua_pushinteger(pool->tasks[0].state, 55);
    task->results = lua_gettop(pool->tasks[0].state);
    task->status = LUA_OK;
    return;
  }
  if (task->id == 1 && pool->wait_kind == GC_WAIT) {
    task->status = lua_resume(task->state, NULL, 0, &task->results);
    return;
  }
  luaX_lockstate(task->state);
  task->status = luaD_rawrunprotected(task->state, resource_body, task);
  luaX_unlockstate(task->state);
}
static void before_wait(struct task *task) {
  struct pool *pool = task->pool;
  lock(pool);
  if (pool->stage == 0) {
    pool->stage = 1;
    signal_change(pool);
    while (pool->stage != 2)
      wait_change(pool);
  }
  unlock(pool);
}
static int exercise_resource_wait(const char *name) {
  struct pool *pool = calloc(1, sizeof(*pool));
  pthread_t workers[2];
  uintptr_t stacks[2];
  int id, count, carriers;
  require(pool != NULL);
  fixture = pool;
  pool->wait_kind = strstr(name, "early") != NULL  ? EARLY_WAIT
                    : strstr(name, "gate") != NULL ? GATE_WAIT
                    : strstr(name, "gc") != NULL   ? GC_WAIT
                                                   : OBJECT_WAIT;
  pool->force_migrate = strstr(name, "migrate") != NULL;
  carriers = pool->force_migrate ? 2 : 1;
  count = pool->wait_kind == EARLY_WAIT ? 2 : 3;
  check(pthread_mutex_init(&pool->mutex, NULL));
  check(pthread_cond_init(&pool->changed, NULL));
  pool->main = luaL_newstate();
  require(pool->main != NULL);
  lua_pushcfunction(pool->main, warmup);
  lua_setglobal(pool->main, "warm");
  if (strstr(name, "generational") != NULL)
    (void)lua_gc(pool->main, LUA_GCGEN);
  (void)lua_gc(pool->main, LUA_GCSTOP);
  if (pool->wait_kind != GC_WAIT) {
    unsigned *payload;
    lua_newtable(pool->main);
    pool->shared = (Table *)lua_topointer(pool->main, -1);
    lua_pushinteger(pool->main, 0);
    lua_rawseti(pool->main, -2, 1);
    payload = lua_newuserdatauv(pool->main, sizeof(*payload), 0);
    *payload = 12345U;
    lua_newtable(pool->main);
    lua_pushcfunction(pool->main, finalize);
    lua_setfield(pool->main, -2, "__gc");
    lua_setmetatable(pool->main, -2);
    lua_rawseti(pool->main, -2, 2);
  }
  for (id = 0; id < count; ++id) {
    struct task *task = &pool->tasks[id];
    task->state = lua_newthread(pool->main);
    if (id == count - 1 || (id == 1 && pool->wait_kind == GC_WAIT)) {
      require(luaL_loadstring(
                  task->state,
                  "warm(); local n=0; for i=1,1000 do n=n+i end; return n") ==
              LUA_OK);
      require(lua_resume(task->state, NULL, 0, &task->results) == LUA_YIELD);
    }
    init_task(pool, task, id, run_resource_task);
    task->state->poll_remaining = 1;
  }
  if (pool->wait_kind == EARLY_WAIT)
    pool->tasks[0].before_wait = before_wait;
  if (pool->wait_kind != GC_WAIT)
    lua_remove(pool->main,
               1); /* Only retained operation scopes keep the table. */
  luaX_lockstate(pool->main);
  if (pool->wait_kind != GC_WAIT)
    luaW_heapenter(pool->main);
  for (id = 0; id < carriers; ++id) {
    pool->workers[id].pool = pool;
    check(pthread_create(&workers[id], NULL, worker_entry, &pool->workers[id]));
  }
  submit(pool, 0);
  lock(pool);
  if (pool->wait_kind == EARLY_WAIT) {
    while (pool->stage != 1)
      wait_change(pool);
    unlock(pool);
    luaW_heapleave(pool->main);
    luaX_unlockstate(pool->main);
    /* Wake and collect while registration exists but context is not published.
     */
    (void)lua_gc(pool->main, LUA_GCCOLLECT);
    require(pool->finalized == 0);
    lock(pool);
    require(pool->tasks[0].early_wakes != 0);
    pool->stage = 2;
    signal_change(pool);
    while (!pool->tasks[0].finished)
      wait_change(pool);
    unlock(pool);
    submit(pool, 1);
  } else {
    while (!pool->tasks[0].published)
      wait_change(pool);
    unlock(pool);
    submit(pool, 1);
    lock(pool);
    while (!pool->tasks[1].published)
      wait_change(pool);
    require(!pool->tasks[0].finished && !pool->tasks[1].finished);
    unlock(pool);
    submit(pool, 2);
    lock(pool);
    while (!pool->tasks[2].finished)
      wait_change(pool);
    require(!pool->tasks[0].finished && !pool->tasks[1].finished);
    unlock(pool);
    if (pool->wait_kind != GC_WAIT) {
      stacks[0] = (uintptr_t)pool->tasks[0].state->stack.p;
      stacks[1] = (uintptr_t)pool->tasks[1].state->stack.p;
      luaC_fullgc(pool->main, 0);
      require(pool->finalized == 0);
      require(stacks[0] == (uintptr_t)pool->tasks[0].state->stack.p);
      if (pool->wait_kind == OBJECT_WAIT)
        require(stacks[1] == (uintptr_t)pool->tasks[1].state->stack.p);
      luaW_heapleave(pool->main);
    }
    luaX_unlockstate(pool->main);
  }
  lock(pool);
  while (pool->done != (unsigned)count)
    wait_change(pool);
  pool->stop = 1;
  signal_change(pool);
  unlock(pool);
  for (id = 0; id < carriers; ++id)
    check(pthread_join(workers[id], NULL));
  require(pool->managed == 0 && pool->native == 0);
  require(pool->tasks[0].resource_waits != 0);
  if (pool->force_migrate)
    require(pool->tasks[0].migrations != 0);
  for (id = 0; id < count; ++id) {
    struct task *task = &pool->tasks[id];
    require(task->status ==
            (id == 0 && pool->wait_kind == GC_WAIT ? LUA_ERRMEM : LUA_OK));
    require(task->state->native_depth == 0 && task->state->world_depth == 0);
    require(task->state->heap_waiting == 0 && task->state->object_waiting == 0);
    require(task->state->native_roots == NULL &&
            task->state->object_scopes == NULL);
    require(task->waiter == NULL);
    if (pool->wait_kind == GATE_WAIT && id == 1)
      require(task->results == 1);
    if (pool->wait_kind != GC_WAIT && id == count - 1)
      require(task->results == 1 && lua_tointeger(task->state, -1) == 500500);
    collect_task(task);
    (void)lua_closethread(task->state, NULL);
    lua_settop(task->state, 0);
    test_fiber_destroy(&task->fiber);
  }
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  require(pool->finalized == (pool->wait_kind == GC_WAIT ? 0U : 1U));
  lua_close(pool->main);
  check(pthread_cond_destroy(&pool->changed));
  check(pthread_mutex_destroy(&pool->mutex));
  printf("resource wait %s: bounded carriers reused, roots and ownership "
         "preserved\n",
         name);
  free(pool);
  return 0;
}
struct foreign_actor {
  struct task task;
  LuaCallback *registration;
  int argument, result, status, done;
  char error[256];
};
struct callback_fixture {
  struct pool pool;
  LuaCallback registration, nested;
  struct foreign_actor actors[2];
  int entered, release;
};
static struct callback_fixture *callback_fixture;
static int callback_self_close(lua_State *state) {
  return lua_closethread(state, state);
}
static int bind_callback_capture(lua_State *state) {
  int index;
  luaX_lockstate(state);
  luaD_checkstack(state, 1);
  setobj2s(state, state->top.p++, &callback_fixture->registration.function);
  luaX_unlockstate(state);
  for (index = 1;; ++index) {
    const char *name = lua_getupvalue(state, -1, index);
    require(name != NULL);
    lua_pop(state, 1);
    if (strcmp(name, "cell") == 0)
      break;
  }
  lua_upvaluejoin(state, -1, index, 1, 1);
  lua_pop(state, 1);
  return 0;
}

static void callback_marshal(lua_State *state, void *context) {
  struct foreign_actor *actor = context;
  lua_pushinteger(state, actor->argument);
  lua_call(state, 1, 1);
  actor->result = (int)lua_tointeger(state, -1);
}
static void init_foreign(struct foreign_actor *actor, struct pool *pool,
                         LuaCallback *registration, int argument) {
  memset(actor, 0, sizeof(*actor));
  actor->task.pool = pool;
  actor->task.native = 1; /* Provider already owns this native carrier. */
  actor->task.target = -1;
  actor->argument = argument;
  actor->registration = registration;
  init_execution(&actor->task);
}
static void *foreign_entry(void *context) {
  struct foreign_actor *actor = context;
  struct pool *pool = actor->task.pool;
  long carrier = syscall(SYS_gettid);
  native_marker = actor->argument + 100;
  actor->status =
      luaCB_call(actor->registration, &actor->task.fiber.execution,
                 callback_marshal, actor, actor->error, sizeof(actor->error));
  require(carrier == syscall(SYS_gettid));
  require(native_marker == actor->argument + 100);
  require(luaQ_current() == NULL);
  require(actor->task.fiber.execution.managed_depth == 0 &&
          actor->task.fiber.execution.native_depth == 0 &&
          actor->task.fiber.execution.protected == NULL);
  require(!actor->task.permit && actor->task.next == NULL);
  if (actor->status == LUA_OK)
    require(actor->task.pinned_waits != 0);
  lock(pool);
  actor->done = 1;
  signal_change(pool);
  unlock(pool);
  return NULL;
}
static int callback_gate(lua_State *state) {
  struct callback_fixture *test = callback_fixture;
  struct pool *pool = &test->pool;
  unsigned *payload = lua_touserdata(state, 1);
  int id = (int)lua_tointeger(state, 2);
  long carrier = syscall(SYS_gettid);
  int marker = native_marker;
  require(payload != NULL && *payload == 12345U);
  if (id == 0) {
    lock(pool);
    test->entered++;
    signal_change(pool);
    while (!test->release)
      wait_change(pool);
    unlock(pool);
  }
  require(carrier == syscall(SYS_gettid) && marker == native_marker);
  require(*payload == 12345U);
  return 0;
}
static int nested_callback(lua_State *state) {
  struct foreign_actor actor;
  struct pool *pool = &callback_fixture->pool;
  init_foreign(&actor, pool, &callback_fixture->registration, 6);
  require(luaCB_call(actor.registration, NULL, callback_marshal, &actor,
                     actor.error, sizeof(actor.error)) == LUA_OK);
  lua_pushinteger(state, actor.result);
  return 1;
}
static int nested_foreign_callback(lua_State *state) {
  struct foreign_actor actor;
  pthread_t provider;
  struct pool *pool = &callback_fixture->pool;
  init_foreign(&actor, pool, &callback_fixture->nested, 6);
  require(pthread_create(&provider, NULL, foreign_entry, &actor) == 0);
  require(pthread_join(provider, NULL) == 0);
  require(actor.status == LUA_OK);
  lua_pushinteger(state, actor.result);
  return 1;
}
static int exercise_callback(const char *name) {
  static const unsigned char capture[] = "require('fixture')";
  static const char function[] =
      "local token=...; local shared=shared; local cell=0; return function(id) "
      "cell=cell+1; "
      "local child=coroutine.create(function() return 17 end); "
      "assert(coroutine.resume(child)); shared.child=child; "
      "local n=0; for i=1,1000 do n=n+i end; shared[id]=n; "
      "callback_gate(token,id); "
      "if shared.finished then shared.observed=shared.finished end; "
      "if id==2 then return nested_callback() end; "
      "if id==7 then return nested_foreign_callback() end; return n+id end";
  struct callback_fixture *test = calloc(1, sizeof(*test));
  struct pool *pool;
  pthread_t providers[2], workers[WORKERS];
  struct foreign_actor probe;
  int id, revoke = strstr(name, "revoke") != NULL;
  int open_capture = strstr(name, "open") != NULL;
  require(test != NULL);
  callback_fixture = test;
  fixture = pool = &test->pool;
  check(pthread_mutex_init(&pool->mutex, NULL));
  check(pthread_cond_init(&pool->changed, NULL));
  require(luthrum_lua_runtime_new(&pool->runtime) == LUTHRUM_LUA_RUNTIME_OK);
  require(luthrum_lua_runtime_openlibs(pool->runtime) ==
          LUTHRUM_LUA_RUNTIME_OK);
  require(luthrum_lua_runtime_register_c_module(pool->runtime, "fixture",
                                                open_fixture) ==
          LUTHRUM_LUA_RUNTIME_OK);
  require(luthrum_lua_runtime_run_buffer(pool->runtime, capture,
                                         sizeof(capture) - 1, "capture", 0,
                                         NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  if (strstr(name, "budget") != NULL) {
    require(luthrum_lua_runtime_set_instruction_limit(pool->runtime, 100000) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_run_buffer(pool->runtime, capture,
                                           sizeof(capture) - 1, "budget", 0,
                                           NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  }
  lua_pushcfunction(pool->main, block_binding);
  lua_setglobal(pool->main, "block");
  lua_pushcfunction(pool->main, warmup);
  lua_setglobal(pool->main, "warm");
  lua_pushcfunction(pool->main, callback_gate);
  lua_setglobal(pool->main, "callback_gate");
  lua_pushcfunction(pool->main, nested_callback);
  lua_setglobal(pool->main, "nested_callback");
  lua_pushcfunction(pool->main, nested_foreign_callback);
  lua_setglobal(pool->main, "nested_foreign_callback");
  lua_pushcfunction(pool->main, callback_self_close);
  lua_setglobal(pool->main, "self_close");
  lua_pushcfunction(pool->main, bind_callback_capture);
  lua_setglobal(pool->main, "bind_capture");
  lua_newtable(pool->main);
  lua_setglobal(pool->main, "shared");
  {
    unsigned *payload = lua_newuserdatauv(pool->main, sizeof(*payload), 0);
    *payload = 12345U;
    lua_newtable(pool->main);
    lua_pushcfunction(pool->main, finalize);
    lua_setfield(pool->main, -2, "__gc");
    lua_setmetatable(pool->main, -2);
    require(luaL_loadstring(pool->main, function) == LUA_OK);
    lua_pushvalue(pool->main, -2);
    require(lua_pcall(pool->main, 1, 1, 0) == LUA_OK);
    require(luaCB_init(pool->main, &test->registration, 2) == LUA_OK);
    require(luaCB_init(pool->main, &test->nested, 0) == LUA_OK);
    lua_settop(pool->main,
               0); /* Only registered roots retain the closure/token. */
  }
  if (strstr(name, "generational") != NULL)
    (void)lua_gc(pool->main, LUA_GCGEN);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  require(pool->finalized == 0);
  for (id = 0; id < 4; ++id) {
    struct task *task = &pool->tasks[id];
    task->state = lua_newthread(pool->main);
    require(luaL_loadstring(
                task->state,
                id == 0 && open_capture
                    ? "local cell=0; bind_capture(function() return cell end); "
                      "local result=block(...); return result,cell"
                : id < 2  ? "return block(...)"
                : id == 3 ? "self_close()"
                          : "warm(); local n=0; for i=1,1000 do n=n+i end; "
                            "shared.finished=n; return n") == LUA_OK);
    if (id < 2) {
      unsigned *payload;
      lua_pushinteger(task->state, id);
      payload = lua_newuserdatauv(task->state, sizeof(*payload), 0);
      *payload = 12345U;
      lua_newtable(task->state);
      lua_pushcfunction(task->state, finalize);
      lua_setfield(task->state, -2, "__gc");
      lua_setmetatable(task->state, -2);
    } else if (id == 2)
      require(lua_resume(task->state, NULL, 0, &task->results) == LUA_YIELD);
    init_task(pool, task, id, NULL);
  }
  for (id = 0; id < WORKERS; ++id) {
    pool->workers[id].pool = pool;
    check(pthread_create(&workers[id], NULL, worker_entry, &pool->workers[id]));
  }
  submit(pool, 0);
  submit(pool, 1);
  lock(pool);
  while (pool->entered != 2)
    wait_change(pool);
  require(pool->native == NATIVE);
  unlock(pool);
  for (id = 0; id < 2; ++id)
    init_foreign(&test->actors[id], pool, &test->registration, id);
  check(pthread_create(&providers[0], NULL, foreign_entry, &test->actors[0]));
  lock(pool);
  while (!test->entered)
    wait_change(pool);
  unlock(pool);
  check(pthread_create(&providers[1], NULL, foreign_entry, &test->actors[1]));
  check(pthread_mutex_lock(&test->registration.mutex));
  while (test->registration.waiters == 0)
    check(pthread_cond_wait(&test->registration.changed,
                            &test->registration.mutex));
  require(test->registration.entries == 2 && test->registration.depth == 1);
  check(pthread_mutex_unlock(&test->registration.mutex));
  init_foreign(&probe, pool, &test->registration, 8);
  require(luaCB_call(probe.registration, &probe.task.fiber.execution,
                     callback_marshal, &probe, probe.error,
                     sizeof(probe.error)) == LUA_CB_CAPACITY);
  submit(pool, 2);
  lock(pool);
  while (!pool->tasks[2].finished)
    wait_change(pool);
  require(!test->actors[0].done && !test->actors[1].done &&
          pool->native == NATIVE);
  unlock(pool);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  require(pool->finalized == 0);
  if (revoke) {
    luaCB_revoke(&test->registration);
    require(luaCB_destroy(pool->main, &test->registration) == LUA_CB_PENDING);
    check(pthread_join(providers[1], NULL));
    require(test->actors[1].status == LUA_CB_REVOKED);
    require(luaCB_call(probe.registration, &probe.task.fiber.execution,
                       callback_marshal, &probe, probe.error,
                       sizeof(probe.error)) == LUA_CB_REVOKED);
    (void)lua_gc(pool->main, LUA_GCCOLLECT);
    require(pool->finalized == 0);
  }
  lock(pool);
  test->release = 1;
  signal_change(pool);
  unlock(pool);
  check(pthread_join(providers[0], NULL));
  require(test->actors[0].status == LUA_OK && test->actors[0].result == 500500);
  if (!revoke) {
    check(pthread_join(providers[1], NULL));
    require(test->actors[1].status == LUA_OK &&
            test->actors[1].result == 500501);
    for (id = 0; id < 2; ++id) {
      init_foreign(&probe, pool, &test->registration, id == 0 ? 2 : 7);
      (void)foreign_entry(
          &probe); /* Host thread is also a provider-owned caller. */
      require(probe.status == LUA_OK && probe.result == 500506);
    }
    luaCB_revoke(&test->registration);
  }
  require(test->registration.thread->luthrum_stack_context == NULL);
  require(luaCB_destroy(pool->main, &test->registration) == LUA_OK);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  require(pool->finalized ==
          0); /* The second registration still retains token. */
  luaCB_revoke(&test->nested);
  require(luaCB_destroy(pool->main, &test->nested) == LUA_OK);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  require(pool->finalized == 1);
  lua_getglobal(pool->main, "shared");
  lua_getfield(pool->main, -1, "child");
  require(lua_tothread(pool->main, -1)->luthrum_stack_context == NULL);
  lua_pop(pool->main, 1);
  lua_getfield(pool->main, -1, "observed");
  require(lua_tointeger(pool->main, -1) == 500500);
  lua_pop(pool->main, 2);
  lock(pool);
  require(pool->native == NATIVE && pool->managed == 0);
  pool->release[0] = pool->release[1] = 1;
  signal_change(pool);
  while (pool->done != 3)
    wait_change(pool);
  unlock(pool);
  submit(pool, 3); /* A successful self-close must retire its native lease. */
  lock(pool);
  while (pool->done != 4)
    wait_change(pool);
  pool->stop = 1;
  signal_change(pool);
  unlock(pool);
  for (id = 0; id < WORKERS; ++id)
    check(pthread_join(workers[id], NULL));
  require(pool->native == 0 && pool->managed == 0);
  for (id = 0; id < 4; ++id) {
    require(pool->tasks[id].status == LUA_OK);
    if (id == 0 && open_capture)
      require(pool->tasks[id].results == 2 &&
              lua_tointeger(pool->tasks[id].state, -1) == 6);
    collect_task(&pool->tasks[id]);
    (void)lua_closethread(pool->tasks[id].state, NULL);
    lua_settop(pool->tasks[id].state, 0);
    test_fiber_destroy(&pool->tasks[id].fiber);
  }
  lua_settop(pool->main, 0);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  require(pool->finalized == 3);
  luthrum_lua_runtime_free(pool->runtime);
  check(pthread_cond_destroy(&pool->changed));
  check(pthread_mutex_destroy(&pool->mutex));
  printf("registered foreign callbacks %s: bounded admission and protected "
         "nested entry\n",
         name);
  free(test);
  return 0;
}
enum { STREAM_CHUNKS = 1024, STREAM_HEAP_LIMIT = 128 * 1024 };
static void *stream_allocate(void *context, void *old, size_t old_size,
                             size_t size) {
  struct pool *pool = context;
  void *pointer;
  if (old == NULL)
    old_size = 0;
  if (size == 0) {
    pool->stream_live -= old_size;
    free(old);
    return NULL;
  }
  if (size > STREAM_HEAP_LIMIT - (pool->stream_live - old_size))
    return NULL;
  pointer = malloc(size);
  if (pointer != NULL) {
    if (old != NULL) {
      memcpy(pointer, old, old_size < size ? old_size : size);
      free(old);
    }
    pool->stream_live = pool->stream_live - old_size + size;
    if (pool->stream_peak < pool->stream_live)
      pool->stream_peak = pool->stream_live;
  }
  return pointer;
}
static int make_chunk(lua_State *state) {
  char chunk[LUA_PIPE_CHUNK_MAX];
  unsigned id = (unsigned)lua_tointeger(state, 1), i;
  require((luaQ_current()->native_depth != 0) == fixture->stream_foreign);
  require(id >= 1 && id <= STREAM_CHUNKS);
  chunk[0] = (char)(id & 255);
  chunk[1] = (char)(id >> 8);
  for (i = 2; i < sizeof(chunk); ++i)
    chunk[i] = (char)((id + i) & 255);
  lock(fixture);
  fixture->generated++;
  signal_change(fixture);
  unlock(fixture);
  lua_pushlstring(state, chunk, sizeof(chunk));
  return 1;
}
static int check_chunk(lua_State *state) {
  size_t size;
  const unsigned char *chunk =
      (const unsigned char *)lua_tolstring(state, 1, &size);
  unsigned i, id;
  require(luaQ_current()->native_depth == 0);
  require(chunk != NULL && size == LUA_PIPE_CHUNK_MAX);
  id = chunk[0] + ((unsigned)chunk[1] << 8);
  require(id >= 1 && id <= STREAM_CHUNKS);
  for (i = 2; i < size; ++i)
    require(chunk[i] == ((id + i) & 255));
  lock(fixture);
  require(!fixture->stream_seen[id - 1]);
  fixture->stream_seen[id - 1] = 1;
  fixture->consumed++;
  signal_change(fixture);
  unlock(fixture);
  lua_pushboolean(state, id == 1);
  return 1;
}
static int check_managed(lua_State *state) {
  (void)state;
  require(luaQ_current() != NULL && luaQ_current()->native_depth == 0);
  return 0;
}
static const char *control_script(lua_State *state, const char *body,
                                  int close_path) {
  const char *source;
  if (close_path)
    source = lua_pushfstring(
        state,
        "local result; local co=coroutine.create(function() "
        "local token <close> =setmetatable({}, {__close=function() "
        "audit_base(); result=(function() %s end)() end}); "
        "coroutine.yield('ready') end); "
        "local ok,value=coroutine.resume(co); assert(ok and value=='ready'); "
        "check_managed(); assert(coroutine.close(co)); return result",
        body);
  else
    source = lua_pushfstring(
        state,
        "return coroutine.wrap(function() check_managed(); "
        "local co=coroutine.create(function() "
        "local ok,result=pcall(function() "
        "coroutine.yield('ready'); local ok,value=xpcall(function() "
        "local pass,n=pcall(function() audit_base(); "
        "return (function() %s end)() end); assert(pass,n); "
        "error({result=n}) end, function(err) check_managed(); return err "
        "end); "
        "assert(not ok and type(value)=='table'); return value.result end); "
        "assert(ok,result); return result end); "
        "local ok,value=coroutine.resume(co); assert(ok and value=='ready'); "
        "ok,value=coroutine.resume(co); assert(ok,value); "
        "assert(coroutine.status(co)=='dead'); assert(coroutine.close(co)); "
        "return value end)()",
        body);
  return source;
}
static void run_stream_task(struct task *task) {
  task->status =
      luaTK_resume(&task->root, task->id < 2 ? 2 : 0, &task->results);
}
static void *stream_foreign_entry(void *context) {
  struct foreign_actor *actor = context;
  struct pool *pool = actor->task.pool;
  long carrier = syscall(SYS_gettid);
  native_marker = 200;
  actor->status =
      luaCB_call(actor->registration, &actor->task.fiber.execution,
                 callback_marshal, actor, actor->error, sizeof(actor->error));
  require(carrier == syscall(SYS_gettid) && native_marker == 200);
  require(luaQ_current() == NULL && !actor->task.permit &&
          actor->task.waiter == NULL);
  require(actor->task.fiber.execution.protected == NULL &&
          actor->task.fiber.execution.native_depth == 0 &&
          actor->task.fiber.execution.managed_depth == 0);
  lock(pool);
  actor->done = 1;
  signal_change(pool);
  unlock(pool);
  return NULL;
}
static void end_pipe(lua_State *state, int finalizing) {
  lua_getglobal(state, "p");
  if (finalizing) {
    require(lua_getmetatable(state, -1));
    lua_getfield(state, -1, "__gc");
    lua_pushvalue(state, -3);
    lua_call(state, 1, 0);
    lua_pop(state, 2);
  } else {
    lua_getfield(state, -1, "close");
    lua_pushvalue(state, -2);
    lua_call(state, 1, 0);
    lua_pop(state, 1);
  }
}
static int exercise_stream(const char *name) {
  static const char producer[] =
      "p:send(make_chunk(1)); ack:recv(); "
      "for i=2,1024 do p:send(make_chunk(i)) end; p:close(); return 1024";
  static const char consumer[] =
      "local n=0; while true do local chunk=p:recv(); "
      "if chunk==nil then break end; "
      "if check_chunk(chunk) then checkpoint:recv(); ack:send('ack') end; "
      "n=n+1 end; return n";
  struct pool *pool = calloc(1, sizeof(*pool));
  struct foreign_actor actor;
  LuaCallback registration;
  pthread_t workers[WORKERS], provider;
  int id, native = strstr(name, "native") != NULL;
  int foreign = strstr(name, "foreign") != NULL;
  int abort_send = strstr(name, "send") != NULL;
  int abort_recv = strstr(name, "recv") != NULL;
  int aborting = abort_send || abort_recv;
  int finalizing = strstr(name, "close") == NULL;
  int control = strstr(name, "control") != NULL;
  int facade = strstr(name, "facade") != NULL;
  int closing_control = strstr(name, "closing") != NULL;
  int carriers, consumers, expected_done;
  unsigned sum = 0;
  require(pool != NULL);
  fixture = pool;
  pool->stream_foreign = foreign;
  pool->force_migrate = strstr(name, "migrate") != NULL;
  native |= foreign;
  carriers = native ? WORKERS : pool->force_migrate ? 2 : 1;
  consumers = native && !foreign ? 1 : 3;
  expected_done = abort_send ? 1 : (foreign ? 0 : 1) + consumers;
  if (abort_recv)
    expected_done--;
  if (native)
    expected_done += 2;
  check(pthread_mutex_init(&pool->mutex, NULL));
  check(pthread_cond_init(&pool->changed, NULL));
  if (facade) {
    static const unsigned char capture[] = "require('fixture')";
    require(luthrum_lua_runtime_new_with_limit(
                &pool->runtime, STREAM_HEAP_LIMIT) == LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_openlibs(pool->runtime) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_register_c_module(pool->runtime, "fixture",
                                                  open_fixture) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_set_instruction_limit(pool->runtime, 2000000) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_run_buffer(
                pool->runtime, capture, sizeof(capture) - 1, "stream-control",
                0, NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  } else
    pool->main = lua_newstate(stream_allocate, pool, 0);
  require(pool->main != NULL);
  if (control && !facade)
    luaL_openlibs(pool->main);
  luaQ_pushmanaged(pool->main, check_managed, 0);
  lua_setglobal(pool->main, "check_managed");
  if (control) {
    require(
        luaL_loadstring(
            pool->main,
            "function audit_base() check_managed(); local t={11,22}; "
            "rawset(t,'x',tonumber('17')); assert(rawget(t,'x')==17); "
            "assert(rawlen(t)==2 and rawequal(t,t)); "
            "assert(select('#',1,2,3)==3 and select(-1,1,2,3)==3); "
            "assert(tonumber('1a',16)==26 and next(t)~=nil); "
            "local sum=0; for _,v in ipairs(t) do sum=sum+v end; "
            "assert(sum==33); "
            "local proxy=setmetatable({}, {__metatable='protected', "
            "__tostring=function() check_managed(); return 'owned' end, "
            "__pairs=function() check_managed(); return next,t,nil end}); "
            "assert(tostring(proxy)=='owned' and "
            "getmetatable(proxy)=='protected'); "
            "sum=0; for _,v in pairs(proxy) do sum=sum+v end; assert(sum==50); "
            "sum=0; for _,v in pairs(t) do sum=sum+v end; assert(sum==50); "
            "local co,main=coroutine.running(); assert(type(co)=='thread' and "
            "not main); "
            "assert(coroutine.status(co)=='running'); "
            "assert(type(coroutine.isyieldable())=='boolean'); "
            "assert(type(collectgarbage('count'))=='number'); "
            "assert(type(collectgarbage('step',0))=='boolean'); end") ==
        LUA_OK);
    require(lua_pcall(pool->main, 0, 0, 0) == LUA_OK);
  }
  lua_pushcfunction(pool->main, block_binding);
  lua_setglobal(pool->main, "block");
  luaQ_pushmanaged(pool->main, make_chunk, 0);
  lua_setglobal(pool->main, "make_chunk");
  luaQ_pushmanaged(pool->main, check_chunk, 0);
  lua_setglobal(pool->main, "check_chunk");
  luaP_newpipe(pool->main, 1);
  lua_setglobal(pool->main, "p");
  luaP_newpipe(pool->main, 1);
  lua_setglobal(pool->main, "ack");
  luaP_newpipe(pool->main, 1);
  lua_setglobal(pool->main, "checkpoint");
  if (strstr(name, "generational") != NULL)
    (void)lua_gc(pool->main, LUA_GCGEN);
  for (id = 0; id < TASKS; ++id) {
    struct task *task = &pool->tasks[id];
    const char *script;
    if ((id < 2 && !native) || (id == 2 && (foreign || abort_recv)) ||
        (id >= 3 && (abort_send || id >= 3 + consumers)))
      continue;
    script = id < 2 ? "return block(...)"
             : id == 2 && abort_send
                 ? "p:send(make_chunk(1)); p:send(make_chunk(2)); return 0"
             : id == 2    ? producer
             : abort_recv ? "return p:recv()"
                          : consumer;
    task->state = lua_newthread(pool->main);
    if (control && id >= 2)
      script = control_script(pool->main, script, closing_control);
    require(luaL_loadstring(task->state, script) == LUA_OK);
    if (control && id >= 2)
      lua_pop(pool->main, 1);
    if (id < 2) {
      unsigned *payload;
      lua_pushinteger(task->state, id);
      payload = lua_newuserdatauv(task->state, sizeof(*payload), 0);
      *payload = 12345U;
    }
    init_task(pool, task, id, run_stream_task);
    require(luaTK_admit(task->state, &task->root, &task->fiber.execution) ==
            LUA_OK);
  }
  if (foreign) {
    char script[256];
    require(snprintf(script, sizeof(script), "return function() %s end",
                     producer) < (int)sizeof(script));
    require(luaL_loadstring(pool->main, script) == LUA_OK);
    require(lua_pcall(pool->main, 0, 1, 0) == LUA_OK);
    require(luaCB_init(pool->main, &registration, 0) == LUA_OK);
    lua_pop(pool->main, 1);
    init_foreign(&actor, pool, &registration, 0);
  }
  for (id = 0; id < carriers; ++id) {
    pool->workers[id].pool = pool;
    check(pthread_create(&workers[id], NULL, worker_entry, &pool->workers[id]));
  }
  if (native) {
    submit(pool, 0);
    submit(pool, 1);
    lock(pool);
    while (pool->entered != 2)
      wait_change(pool);
    require(pool->native == NATIVE);
    unlock(pool);
  }
  if (!abort_send) {
    for (id = 3; id < 3 + consumers; ++id) {
      submit(pool, id);
      lock(pool);
      while (!pool->tasks[id].published)
        wait_change(pool);
      unlock(pool);
    }
  }
  if (!abort_recv) {
    if (foreign)
      check(pthread_create(&provider, NULL, stream_foreign_entry, &actor));
    else
      submit(pool, 2);
    lock(pool);
    if (aborting) {
      while (!pool->tasks[2].published)
        wait_change(pool);
    } else {
      int parked;
      do {
        parked = 0;
        for (id = 3; id < 3 + consumers; ++id)
          parked += pool->tasks[id].published != 0;
        if (pool->consumed == 1 && parked == consumers &&
            (foreign ? actor.task.pinned_resource_waits != 0
                     : pool->tasks[2].published))
          break;
        wait_change(pool);
      } while (1);
      require(pool->generated == 1);
    }
    unlock(pool);
  }
  /* The producer cannot obtain the remaining input until an actual downstream
     consumer has acknowledged the first chunk and this collection completes. */
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  if (aborting)
    end_pipe(pool->main, finalizing);
  else {
    lua_getglobal(pool->main, "checkpoint");
    lua_getfield(pool->main, -1, "send");
    lua_pushvalue(pool->main, -2);
    lua_pushliteral(pool->main, "go");
    lua_call(pool->main, 2, 0);
    lua_pop(pool->main, 1);
  }
  if (foreign) {
    check(pthread_join(provider, NULL));
    require(actor.status == LUA_OK && actor.result == STREAM_CHUNKS);
    require(actor.task.pinned_resource_waits != 0 && actor.task.parks == 0);
    luaCB_revoke(&registration);
    require(luaCB_destroy(pool->main, &registration) == LUA_OK);
  }
  lock(pool);
  while (pool->done != (unsigned)(expected_done - (native ? 2 : 0)))
    wait_change(pool);
  if (native) {
    require(pool->native == NATIVE && !pool->tasks[0].finished &&
            !pool->tasks[1].finished);
    pool->release[0] = pool->release[1] = 1;
    signal_change(pool);
    while (pool->done != (unsigned)expected_done)
      wait_change(pool);
  }
  pool->stop = 1;
  signal_change(pool);
  unlock(pool);
  for (id = 0; id < carriers; ++id)
    check(pthread_join(workers[id], NULL));
  require(pool->managed == 0 && pool->native == 0 && pool->native_waits == 0);
  if (!native)
    require(pool->native_peak == 0);
  if (!aborting)
    require(pool->generated == STREAM_CHUNKS &&
            pool->consumed == STREAM_CHUNKS);
  for (id = 0; id < TASKS; ++id) {
    struct task *task = &pool->tasks[id];
    if (task->state == NULL)
      continue;
    if (task->status != LUA_OK && !(abort_send && id == 2))
      fprintf(stderr, "stream task %d: %s\n", id,
              lua_tostring(task->state, -1));
    require(task->status == (abort_send && id == 2 ? LUA_ERRRUN : LUA_OK));
    require(task->fiber.execution.protected == NULL &&
            task->state->native_depth == 0 && task->state->world_depth == 0 &&
            task->state->native_roots == NULL &&
            task->state->object_scopes == NULL);
    if (id >= 3 && !aborting)
      sum += (unsigned)lua_tointeger(task->state, -1);
    if (id >= 2) {
      require(task->resource_waits != 0 && task->pinned_resource_waits == 0);
      if (pool->force_migrate)
        require(task->migrations != 0);
    }
    collect_task(task);
    (void)lua_closethread(task->state, NULL);
    test_fiber_destroy(&task->fiber);
  }
  if (!aborting)
    require(sum == STREAM_CHUNKS);
  require(luaL_loadstring(
              pool->main,
              aborting ? "local t=p:stats(); return t.buffered_bytes, t.ready"
                       : "local t=p:stats(); return t.sent_bytes, "
                         "t.received_bytes, t.peak_bytes, t.buffered_bytes") ==
          LUA_OK);
  require(lua_pcall(pool->main, 0, aborting ? 2 : 4, 0) == LUA_OK);
  if (aborting) {
    require(lua_tointeger(pool->main, -2) ==
            (abort_send && !finalizing ? 1024 : 0));
    require(lua_toboolean(pool->main, -1) == !finalizing);
  } else {
    require(lua_tointeger(pool->main, -4) ==
            STREAM_CHUNKS * LUA_PIPE_CHUNK_MAX);
    require(lua_tointeger(pool->main, -3) ==
            STREAM_CHUNKS * LUA_PIPE_CHUNK_MAX);
    require(lua_tointeger(pool->main, -2) <= LUA_PIPE_CHUNK_MAX);
    require(lua_tointeger(pool->main, -1) == 0);
  }
  if (facade)
    luthrum_lua_runtime_free(pool->runtime);
  else
    lua_close(pool->main);
  require(pool->stream_live == 0 && pool->stream_peak <= STREAM_HEAP_LIMIT);
  check(pthread_cond_destroy(&pool->changed));
  check(pthread_mutex_destroy(&pool->mutex));
  printf("chunk stream %s: %u chunks, %zu Lua-byte %s, %d owned carriers\n",
         name, pool->consumed,
         facade ? (size_t)STREAM_HEAP_LIMIT : pool->stream_peak,
         facade ? "cap" : "peak", carriers);
  free(pool);
  return 0;
}
static void parallel_poll(lua_State *state, void *context) {
  struct task *task = context;
  struct pool *pool = task->pool;
  unsigned arrived;
  int status;
  if (task->parallel_seen)
    return;
  task->parallel_seen = 1;
  task->parallel_cpu = sched_getcpu();
  require(luaQ_current() == &task->fiber.execution &&
          task->fiber.execution.managed_depth != 0);
  lock(pool);
  require(pool->managed_limit == 2);
  arrived = ++pool->parallel_arrivals;
  if (!pool->force_migrate)
    task->target = -1;
  signal_change(pool);
  unlock(pool);
  if (arrived == 2) {
    LuaWorld *world = &G(state)->world;
    check(pthread_mutex_lock(&world->mutex));
    require(world->active == 2 && !world->stopped);
    check(pthread_mutex_unlock(&world->mutex));
  }
  status = pthread_barrier_wait(&pool->parallel);
  require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}
static int await_task(lua_State *state) {
  int id = (int)luaL_checkinteger(state, 1);
  int status = -999, results = -999, code, base = lua_gettop(state);
  require(id == 2 || id == 3);
  code = luaTK_await(state, &fixture->tasks[id].root, &status, &results);
  lua_pushinteger(state, code);
  lua_insert(state, base + 1);
  if (code == LUA_OK) {
    lua_pushinteger(state, status);
    lua_insert(state, base + 2);
    return results + 2;
  }
  require(code == LUA_TK_COLLECTED && status == -999 && results == -999);
  return 1;
}
static int reject_self_wait(lua_State *state) {
  LuaExecution *execution = luaQ_current();
  LuaTaskRoot *task = __atomic_load_n(&execution->task, __ATOMIC_ACQUIRE);
  int status = 111, results = 222;
  require(task != NULL);
  require(luaTK_await(state, task, &status, &results) == LUA_TK_INVALID);
  require(status == 111 && results == 222);
  return 0;
}
static int await_report(lua_State *state) {
  struct pool *pool = fixture;
  int id = (int)luaL_checkinteger(state, 1) - 2;
  int won = lua_toboolean(state, 2);
  require(id >= 0 && id < 2);
  if (won) {
    unsigned *token = lua_touserdata(state, 3);
    require(token == pool->await_payloads[id] && *token == 12345U);
  }
  lock(pool);
  if (won)
    pool->await_winners[id]++;
  else
    pool->await_losers[id]++;
  signal_change(pool);
  unlock(pool);
  return 0;
}
static int await_ready(lua_State *state) {
  int id = (int)luaL_checkinteger(state, 1) - 2;
  require(id >= 0 && id < 2);
  lock(fixture);
  fixture->await_ready[id]++;
  signal_change(fixture);
  unlock(fixture);
  return 0;
}
static int await_cleanup(lua_State *state) {
  unsigned *token = lua_touserdata(state, 1);
  require(token != NULL && *token == 12345U);
  lock(fixture);
  fixture->await_closed++;
  unlock(fixture);
  return 0;
}
static int await_finalize(lua_State *state) {
  unsigned *token = lua_touserdata(state, 1);
  require(token != NULL && *token == 12345U);
  *token = 0;
  lock(fixture);
  fixture->finalized++;
  unlock(fixture);
  return 0;
}
static void await_before_wait(struct task *task) {
  if (task->await_waits != 0)
    before_wait(task);
}
static void release_producer(struct pool *pool, const char *name) {
  lua_getglobal(pool->main, name);
  lua_getfield(pool->main, -1, "send");
  lua_insert(pool->main, -2);
  lua_pushliteral(pool->main, "release");
  require(lua_pcall(pool->main, 2, 0, 0) == LUA_OK);
}
static void run_await_driver(struct task *task) {
  int status = -999, results = 0;
  int code =
      luaTK_await(task->state, &task->pool->tasks[2].root, &status, &results);
  require(code == LUA_OK || code == LUA_TK_COLLECTED);
  if (code == LUA_TK_COLLECTED)
    require(status == -999 && results == 0);
  else
    require(status == LUA_OK && results == 3);
  lua_pushinteger(task->state, code);
  lua_insert(task->state,
             2); /* The unentered task function stays at index 1. */
  task->status = luaTK_resume(&task->root, results + 1, &task->results);
}
static void install_library_audit(lua_State *state) {
  static const char source[] =
      "function audit_libraries(id) check_managed(); "
      "assert(math.abs(-3)==3 and math.acos(1)==0 and math.asin(0)==0); "
      "assert(math.atan(0,1)==0 and math.ceil(1.2)==2 and math.floor(1.8)==1); "
      "assert(math.cos(0)==1 and math.sin(0)==0 and math.tan(0)==0); "
      "assert(math.deg(0)==0 and math.rad(0)==0 and math.exp(0)==1); "
      "assert(math.tointeger('12')==12 and math.tointeger(1.5)==nil); "
      "assert(math.fmod(17,5)==2 and math.fmod(17.5,5)==2.5); "
      "local m,e=math.frexp(8); assert(m==0.5 and e==4 and "
      "math.ldexp(m,e)==8); "
      "assert(math.ult(1,-1) and math.log(8,2)==3 and math.log(100,10)==2); "
      "m,e=math.modf(-3.25); assert(m==-3 and e==-0.25 and math.sqrt(81)==9); "
      "assert(math.type(7)=='integer' and math.type(7.5)=='float' and "
      "math.type('7')==nil); "
      "local x,y=math.randomseed(id,id+1); assert(x==id and y==id+1); "
      "for i=1,32 do x=math.random(); assert(x>=0 and x<1); "
      "x=math.random(1,7); assert(x>=1 and x<=7); math.random(0) end; "
      "math.randomseed(); local waited=false; "
      "local mt={__lt=function(a,b) check_managed(); "
      "if id==2 and not waited then waited=true; assert(await_checked(3)==42) "
      "end; "
      "return a.n<b.n end}; "
      "local a,b=setmetatable({n=1},mt),setmetatable({n=2},mt); "
      "local t=table.pack(4,1,3,2); assert(t.n==4); "
      "table.sort(t,function(x,y) check_managed(); "
      "assert(math.min(a,b)==a and math.max(a,b)==b); return x<y end); "
      "assert(table.concat(t,',')=='1,2,3,4'); "
      "assert(table.remove(t,2)==2); table.insert(t,2,8); "
      "local c=table.create(4,2); assert(table.move(t,1,4,1,c)==c); "
      "local p,q,r,s=table.unpack(c); assert(p==1 and q==8 and r==3 and s==4); "
      "table.move(c,1,3,2); assert(table.concat(c,':')=='1:1:8:3'); "
      "assert(select('#',table.unpack(table.pack(1,nil,3),1,3))==3); "
      "local backing={3,1,2}; local proxy=setmetatable({}, { "
      "__len=function() check_managed(); return #backing end, "
      "__index=function(_,k) check_managed(); return backing[k] end, "
      "__newindex=function(_,k,v) check_managed(); backing[k]=v end}); "
      "table.sort(proxy); assert(table.concat(proxy,',')=='1,2,3'); "
      "local ok=pcall(table.sort,{3,2,1},function() check_managed(); "
      "error('sort expected') end); "
      "assert(not ok); ok=pcall(math.min,setmetatable({}, {__lt=function() "
      "check_managed(); error('min expected') end}),{}); assert(not ok); "
      "local chunks={}; local text='0123456789abcdef0123456789abcdef'; "
      "for i=1,400 do chunks[i]=text end; "
      "assert(#table.concat(chunks)==12800); "
      "chunks[401]={}; ok=pcall(table.concat,chunks); assert(not ok); "
      "collectgarbage('collect'); check_managed() end";
  luaQ_pushmanaged(state, check_managed, 0);
  lua_setglobal(state, "check_managed");
  require(luaL_loadstring(state, source) == LUA_OK);
  require(lua_pcall(state, 0, 0, 0) == LUA_OK);
}
static void *utf8_allocate(void *context, void *old, size_t old_size,
                           size_t size) {
  struct pool *pool = context;
  void *memory;
  if (old == NULL)
    old_size = 0;
  if (size == 0) {
    pool->stream_live -= old_size;
    free(old);
    return NULL;
  }
  memory = malloc(size);
  if (memory != NULL) {
    if (old != NULL) {
      memcpy(memory, old, old_size < size ? old_size : size);
      free(old);
    }
    pool->stream_live = pool->stream_live - old_size + size;
  }
  return memory;
}
static void *utf8_host_allocate(void *context, size_t size) {
  return utf8_allocate(context, NULL, 0, size);
}
static void utf8_host_free(void *context, void *memory, size_t size) {
  (void)utf8_allocate(context, memory, size, 0);
}
static int helper_ticks(lua_State *state) {
  struct task *task = luaQ_current()->context;
  lua_Integer helper = luaL_checkinteger(state, 1);
  require(helper >= 1 && helper <= task->pool->helper_count);
  lua_pushinteger(state, task->helper_polls[helper - 1]);
  return 1;
}
static int helper_managed(lua_State *state) {
  LuaExecution *execution = luaQ_current();
  struct task *task;
  (void)state;
  require(execution != NULL);
  task = execution->context;
  require(task->id == 0 ? execution->native_depth != 0 && task->native
                        : execution->native_depth == 0);
  return 0;
}
static void install_utf8_audit(struct pool *pool) {
  static const char *const names[] = {"len", "offset", "char", "codepoint"};
  static const char source[] =
      "utf8_ascii=string.rep('a',4096); "
      "utf8_bad=utf8_ascii..string.char(255); "
      "utf8_cont=string.char(194)..string.rep(string.char(128),4096)..'z'; "
      "utf8_chars={}; utf8_badchars={}; for i=1,4096 do "
      "utf8_chars[i]=97; utf8_badchars[i]=97 end; "
      "utf8_badchars[4097]=-1; "
      "function audit_utf8() "
      "local function work(k,f,...) local before=helper_ticks(k); "
      "local t=table.pack(f(...)); assert(helper_ticks(k)>before); "
      "return table.unpack(t,1,t.n) end; "
      "assert(work(1,utf8.len,utf8_ascii)==4096); "
      "local a,b=work(2,utf8.offset,utf8_ascii,2049); "
      "assert(a==2049 and b==2049); "
      "a,b=work(2,utf8.offset,utf8_ascii,-2049); "
      "assert(a==2048 and b==2048); "
      "a,b=work(2,utf8.offset,utf8_cont,0,4096); assert(a==1 and b==4097); "
      "a,b=work(2,utf8.offset,utf8_cont,2); assert(a==4098 and b==4098); "
      "a,b=work(2,utf8.offset,utf8_cont,-2); assert(a==1 and b==4097); "
      "a,b=work(2,utf8.offset,utf8_cont,1); assert(a==1 and b==4097); "
      "assert(work(3,utf8.char,table.unpack(utf8_chars))==utf8_ascii); "
      "local t=table.pack(work(4,utf8.codepoint,utf8_ascii,1,-1)); "
      "assert(t.n==4096); for i=1,t.n do assert(t[i]==97) end; "
      "for _,lax in ipairs({false,true}) do "
      "local f=utf8.codes('a',lax); "
      "a,b=work(lax and 6 or 5,f,utf8_cont,1); "
      "assert(a==4098 and b==122) end; "
      "a,b=utf8.len(utf8_cont); assert(a==nil and b==3); "
      "assert(not pcall(utf8.codepoint,utf8_cont,1,-1)); "
      "assert(not pcall(utf8.codepoint,utf8_bad,1,-1)); "
      "assert(not pcall(utf8.char,table.unpack(utf8_badchars))); "
      "assert(not pcall(utf8.char,-1)); "
      "assert(not pcall(utf8.len,false)); end";
  lua_State *state = pool->main;
  int index;
  pool->helper_count = 6;
  luaQ_pushmanaged(state, helper_ticks, 0);
  lua_setglobal(state, "helper_ticks");
  lua_getglobal(state, "utf8");
  for (index = 0; index < 4; ++index) {
    lua_getfield(state, -1, names[index]);
    pool->helper_work[index] = lua_tocfunction(state, -1);
    require(pool->helper_work[index] != NULL);
    lua_pop(state, 1);
  }
  for (index = 4; index < 6; ++index) {
    lua_getfield(state, -1, "codes");
    lua_pushliteral(state, "a");
    lua_pushboolean(state, index == 5);
    require(lua_pcall(state, 2, 3, 0) == LUA_OK);
    pool->helper_work[index] = lua_tocfunction(state, -3);
    require(pool->helper_work[index] != NULL);
    lua_pop(state, 3);
  }
  lua_pop(state, 1);
  require(luaL_loadstring(state, source) == LUA_OK);
  require(lua_pcall(state, 0, 0, 0) == LUA_OK);
}
static void install_pattern_audit(struct pool *pool) {
  static const char *const names[] = {"find", "match", "gsub"};
  static const char source[] =
      "pattern_a=string.rep('a',4096); pattern_text=pattern_a..'z'; "
      "pattern_class='['..string.rep('x',2048)..'a]'; "
      "pattern_literal=string.rep('x',2048)..'z'; "
      "pattern_balanced='('..pattern_a..')'; "
      "pattern_pair=pattern_a..pattern_a; "
      "pattern_changed='b'..string.rep('a',4095)..'z'; "
      "function audit_patterns(id) check_managed(); "
      "local function work(k,f,...) local before=helper_ticks(k); "
      "local t=table.pack(f(...)); assert(helper_ticks(k)>before); "
      "return table.unpack(t,1,t.n) end; "
      "local a,b=work(1,string.find,pattern_text,'z',1,true); "
      "assert(a==4097 and b==4097); "
      "assert(work(1,string.find,pattern_text,pattern_literal)==nil); "
      "assert(work(2,string.match,pattern_text,'a+')==pattern_a); "
      "assert(work(2,string.match,pattern_balanced,'%b()')==pattern_balanced); "
      "assert(work(2,string.match,'a',pattern_class)=='a'); "
      "assert(work(2,string.match,pattern_pair,'('..pattern_a..')%1')==pattern_"
      "a); "
      "assert(work(2,string.match,pattern_text,'a-z')==pattern_text); "
      "assert(work(2,string.match,pattern_text,'a*az')==pattern_text); "
      "local out,n=work(3,string.gsub,pattern_a,'a+',pattern_a); "
      "assert(out==pattern_a and n==1); "
      "out,n=work(3,string.gsub,pattern_text,'^a','b'); "
      "assert(out==pattern_changed and n==1); "
      "out,n=work(3,string.gsub,pattern_a,'()a+',pattern_a..'%1'); "
      "assert(out==pattern_a..'1' and n==1); "
      "out,n=work(3,string.gsub,pattern_a,'a+',function(s) check_managed(); "
      "assert(s==pattern_a); if id==2 then assert(await_checked(3)==42) end; "
      "collectgarbage('collect'); return pattern_a end); "
      "assert(out==pattern_a and n==1); "
      "out,n=work(3,string.gsub,pattern_a,'a+',setmetatable({}, { "
      "__index=function(_,s) check_managed(); assert(s==pattern_a); "
      "collectgarbage('collect'); return false end})); "
      "assert(out==pattern_a and n==1); "
      "local f=string.gmatch(pattern_text,'()z'); "
      "assert(work(4,f)==4097 and f()==nil); "
      "assert(not pcall(string.gsub,pattern_a,'a+',function() "
      "check_managed(); collectgarbage('collect'); error('replacement "
      "expected') "
      "end)); check_managed() end";
  lua_State *state = pool->main;
  int index;
  pool->helper_count = 4;
  luaQ_pushmanaged(state, helper_ticks, 0);
  lua_setglobal(state, "helper_ticks");
  luaQ_pushmanaged(state, helper_managed, 0);
  lua_setglobal(state, "check_managed");
  lua_getglobal(state, "string");
  for (index = 0; index < 3; ++index) {
    lua_getfield(state, -1, names[index]);
    pool->helper_work[index] = lua_tocfunction(state, -1);
    require(pool->helper_work[index] != NULL);
    lua_pop(state, 1);
  }
  lua_getfield(state, -1, "gmatch");
  lua_pushliteral(state, "a");
  lua_pushliteral(state, "a");
  require(lua_pcall(state, 2, 1, 0) == LUA_OK);
  pool->helper_work[3] = lua_tocfunction(state, -1);
  require(pool->helper_work[3] != NULL);
  lua_pop(state, 2);
  require(luaL_loadstring(state, source) == LUA_OK);
  require(lua_pcall(state, 0, 0, 0) == LUA_OK);
}
static void install_string_audit(struct pool *pool) {
  static const char *const names[] = {"sub",  "reverse",  "lower", "upper",
                                      "rep",  "byte",     "char",  "format",
                                      "pack", "packsize", "unpack"};
  static const char source[] =
      "string_a=string.rep('a',4096); string_A=string.rep('A',4096); "
      "string_R=string.rep('R',4096); string_z=string_a..'z'; "
      "string_pad=string.pack('c4096','a'); string_numbers={}; "
      "for i=1,4096 do string_numbers[i]=97 end; "
      "string_fmt=string.rep('x',4096); string_spaces=string.rep(' ',4096); "
      "function audit_strings(id) check_managed(); "
      "local function work(k,f,...) local before=helper_ticks(k); "
      "local t=table.pack(f(...)); assert(helper_ticks(k)>before); "
      "return table.unpack(t,1,t.n) end; "
      "assert(string.len(string_a)==4096); "
      "assert(work(1,string.sub,string_a,1)==string_a); "
      "assert(work(2,string.reverse,string_z)=='z'..string_a); "
      "assert(work(3,string.lower,string_A)==string_a); "
      "assert(work(4,string.upper,string_a)==string_A); "
      "assert(work(5,string.rep,string_a,2)==string_a..string_a); "
      "assert(work(5,string.rep,'',3,string_a)==string_a..string_a); "
      "local t=table.pack(work(6,string.byte,string_a,1,-1)); "
      "assert(t.n==4096); for i=1,t.n do assert(t[i]==97) end; "
      "assert(work(7,string.char,table.unpack(string_numbers))==string_a); "
      "assert(work(8,string.format,string_R)==string_R); "
      "assert(work(8,string.format,'%q',string_a)=='\"'..string_a..'\"'); "
      "assert(work(8,string.format,'%.2s',string_a)=='aa'); "
      "local obj=setmetatable({}, {__tostring=function() check_managed(); "
      "if id==2 then assert(await_checked(3)==42) end; "
      "collectgarbage('collect'); return string_a end}); "
      "assert(work(8,string.format,'%s',obj)==string_a); "
      "assert(work(9,string.pack,'c4096','a')==string_pad); "
      "assert(work(9,string.pack,'c4096',string_a)==string_a); "
      "assert(work(9,string.pack,'z',string_a)==string_a..string.char(0)); "
      "assert(work(9,string.pack,'s4',string_a)==string.pack('<I4',4096).."
      "string_a); "
      "assert(work(9,string.pack,string_spaces)==''); "
      "assert(work(10,string.packsize,string_fmt)==4096); "
      "local a,b=work(11,string.unpack,'c4096',string_a); "
      "assert(a==string_a and b==4097); "
      "a,b=work(11,string.unpack,'z',string_a..string.char(0)); "
      "assert(a==string_a and b==4098); "
      "assert(work(11,string.unpack,string_spaces,'')==1); "
      "assert(not pcall(string.format,'%s',setmetatable({}, { "
      "__tostring=function() check_managed(); string.reverse(string_a); "
      "error('format expected') end}))); "
      "assert(not pcall(string.pack,'z',string_a..string.char(0))); "
      "assert(not pcall(string.unpack,'z',string_a)); check_managed() end";
  lua_State *state = pool->main;
  unsigned index;
  pool->helper_count = sizeof(names) / sizeof(names[0]);
  require(pool->helper_count <= MAX_HELPERS);
  luaQ_pushmanaged(state, helper_ticks, 0);
  lua_setglobal(state, "helper_ticks");
  luaQ_pushmanaged(state, helper_managed, 0);
  lua_setglobal(state, "check_managed");
  lua_getglobal(state, "string");
  for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
    lua_getfield(state, -1, names[index]);
    pool->helper_work[index] = lua_tocfunction(state, -1);
    require(pool->helper_work[index] != NULL);
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  require(luaL_loadstring(state, source) == LUA_OK);
  require(lua_pcall(state, 0, 0, 0) == LUA_OK);
}
static void install_dump_audit(struct pool *pool) {
  static const char source[] =
      "dump_target=assert(load('return function() local n=0; '.."
      "string.rep('n=n+1;',256)..'return n,\"'..string.rep('R',8194)..'\" "
      "end'))(); "
      "dump_full=string.dump(dump_target,false); "
      "dump_strip=string.dump(dump_target,true); "
      "function audit_dump(id) check_managed(); "
      "local before=helper_ticks(1); local b=string.dump(dump_target,false); "
      "assert(helper_ticks(1)>before and b==dump_full); "
      "before=helper_ticks(1); b=string.dump(dump_target,true); "
      "assert(helper_ticks(1)>before and b==dump_strip); "
      "assert('21'+21==42 and '5'-2==3 and '6'*7==42 and '9'%4==1); "
      "assert('2'^3==8 and '9'/2==4.5 and '9'//2==4 and -'3'==-3); "
      "assert('0x20'+10==42 and '1.5'+2==3.5); "
      "local waited=false; local obj; local function apply(a,b) "
      "check_managed(); "
      "assert(a=='left' and b==obj); if id==2 and not waited then waited=true; "
      "assert(await_checked(3)==42) end; collectgarbage('collect'); return 42 "
      "end; "
      "obj=setmetatable({}, {__add=apply,__sub=apply,__mul=apply,__mod=apply, "
      "__pow=apply,__div=apply,__idiv=apply,__unm=apply}); "
      "local mt=getmetatable(''); "
      "for _,k in "
      "ipairs({'__add','__sub','__mul','__mod','__pow','__div','__idiv','__unm'"
      "}) do "
      "assert(mt[k]('left',obj)==42) end; "
      "assert(not pcall(mt.__add,'left',setmetatable({}, {__add=function() "
      "check_managed(); assert(string.dump(dump_target)==dump_full); "
      "error('arithmetic expected') end}))); "
      "assert(not pcall(string.dump,print)); check_managed() end";
  lua_State *state = pool->main;
  pool->helper_count = 1;
  luaQ_pushmanaged(state, helper_ticks, 0);
  lua_setglobal(state, "helper_ticks");
  luaQ_pushmanaged(state, helper_managed, 0);
  lua_setglobal(state, "check_managed");
  lua_getglobal(state, "string");
  lua_getfield(state, -1, "dump");
  pool->helper_work[0] = lua_tocfunction(state, -1);
  require(pool->helper_work[0] != NULL);
  lua_pop(state, 2);
  require(luaL_loadstring(state, source) == LUA_OK);
  require(lua_pcall(state, 0, 0, 0) == LUA_OK);
}
static void install_load_audit(struct pool *pool) {
  static const char source[] =
      "load_prefix='--[=['..string.rep('a',4096)..']=]\\n'; "
      "load_text=load_prefix..'return marker'; "
      "load_literal='return [=['..string.rep('R',4096)..']=]'; "
      "load_ident=string.rep('n',4096); "
      "load_names='local '..load_ident..'=42; return '..load_ident; "
      "load_binary=string.dump(assert(load('return function() return 42,\"'.."
      "string.rep('R',4096)..'\" end'))()); "
      "function audit_load(id) check_managed(); "
      "local function work(source,name,mode,env) local before=helper_ticks(1); "
      "local f,e=load(source,name,mode,env); assert(helper_ticks(1)>before); "
      "assert(f,e); return f end; "
      "assert(work(load_text,'=text','t',{marker=42})()==42); "
      "assert(work(load_literal,'=literal','t')()==string.rep('R',4096)); "
      "assert(work(load_names,'=names','t')()==42); "
      "local a,b=work(load_binary,'=binary','b')(); assert(a==42 and "
      "b==string.rep('R',4096)); "
      "local calls=0; local f=work(function() check_managed(); calls=calls+1; "
      "if calls==1 then if id==2 then assert(await_checked(3)==42) end; "
      "assert(assert(load('return 7'))()==7); collectgarbage('collect'); "
      "return load_text end end,'=reader','t',{marker=42}); "
      "assert(calls==2 and f()==42); "
      "local n=0; f=work(function() n=n+1; if n<=4 then return load_prefix "
      "elseif n==5 then return 'return 42' end end,'=fragments','t'); "
      "assert(f()==42); "
      "local bad,e=load(load_prefix..'return {','=bad'); assert(bad==nil and "
      "type(e)=='string'); "
      "bad,e=load(function() check_managed(); "
      "assert(string.len(load_binary)>4096); "
      "error('reader expected') end); assert(bad==nil and type(e)=='string'); "
      "check_managed() end";
  lua_State *state = pool->main;
  pool->helper_count = 1;
  luaQ_pushmanaged(state, helper_ticks, 0);
  lua_setglobal(state, "helper_ticks");
  luaQ_pushmanaged(state, helper_managed, 0);
  lua_setglobal(state, "check_managed");
  lua_getglobal(state, "load");
  pool->helper_work[0] = lua_tocfunction(state, -1);
  require(pool->helper_work[0] != NULL);
  lua_pop(state, 1);
  require(luaL_loadstring(state, source) == LUA_OK);
  require(lua_pcall(state, 0, 0, 0) == LUA_OK);
}
static void install_package_audit(struct pool *pool) {
  static const char source[] =
      "local preload=package.searchers[1]; "
      "setmetatable(package.preload,{__index=function(t,n) check_managed(); "
      "if n:sub(1,5)=='meta_' then return function() check_managed(); "
      "collectgarbage('collect'); return 42 end end end}); "
      "local missing=string.rep('missing ',512); "
      "package.searchers={function(n) check_managed(); return missing end, "
      "preload, function(n) check_managed(); "
      "if n:sub(1,7)=='custom_' then local data={answer=42}; "
      "return function(name,d) check_managed(); assert(name==n and d==data); "
      "collectgarbage('collect'); return d end,data end end}; "
      "function audit_package(id) check_managed(); "
      "local key='owned_'..id; local nested='nested_'..id; "
      "package.preload[nested]=function(n,d) check_managed(); "
      "assert(n==nested and d==':preload:'); return 7 end; "
      "package.preload[key]=function(n,d) check_managed(); "
      "assert(n==key and d==':preload:'); assert(require(nested)==7); "
      "collectgarbage('collect'); "
      "if id==2 then assert(await_checked(3)==42) end; "
      "return {answer=42} end; "
      "local before=helper_ticks(1); local m,d=require(key); "
      "assert(helper_ticks(1)>before and m.answer==42 and d==':preload:'); "
      "assert(require(key)==m and select('#',require(key))==1); "
      "local custom='custom_'..id; local a,b=require(custom); "
      "assert(a==b and a.answer==42); "
      "assert(require('meta_'..id)==42); "
      "local token={}; local bad='bad_'..id; "
      "package.preload[bad]=function() check_managed(); error(token) end; "
      "local ok,e=pcall(require,bad); assert(not ok and e==token); "
      "package.preload[bad]=function() check_managed(); return 42 end; "
      "assert(require(bad)==42); "
      "ok,e=pcall(require,'absent_'..id); "
      "assert(not ok and type(e)=='string'); check_managed() end";
  lua_State *state = pool->main;
  pool->helper_count = 1;
  luaQ_pushmanaged(state, helper_ticks, 0);
  lua_setglobal(state, "helper_ticks");
  luaQ_pushmanaged(state, helper_managed, 0);
  lua_setglobal(state, "check_managed");
  lua_getglobal(state, "require");
  pool->helper_work[0] = lua_tocfunction(state, -1);
  require(pool->helper_work[0] != NULL);
  lua_pop(state, 1);
  require(luaL_loadstring(state, source) == LUA_OK);
  require(lua_pcall(state, 0, 0, 0) == LUA_OK);
}
static int exercise_task_wait(const char *name) {
  struct pool *pool = calloc(1, sizeof(*pool));
  pthread_t workers[MAX_WORKERS];
  cpu_set_t available;
  int cpu, cpus[2] = {-1, -1};
  int id, facade = strstr(name, "facade") != NULL;
  int early = strstr(name, "early") != NULL;
  int callback = strstr(name, "callback") != NULL;
  int driver = strstr(name, "driver") != NULL;
  int error = strstr(name, "error") != NULL;
  int library = strstr(name, "library") != NULL;
  int utf8 = strstr(name, "utf8") != NULL;
  int pattern = strstr(name, "pattern") != NULL;
  int strings = strstr(name, "strings") != NULL;
  int dump = strstr(name, "dump") != NULL;
  int load = strstr(name, "load") != NULL;
  int package = strstr(name, "package") != NULL;
  int helpers = utf8 || pattern || strings || dump || load || package;
  int movealloc = strstr(name, "movealloc") != NULL;
  static const char producer[] =
      "local token,id=...; return function() "
      "local r <close> =setmetatable({}, {__close=function() cleanup(token) "
      "end}); "
      "local n=0; for i=1,2000 do local t={i}; n=n+t[1] end; "
      "shared[id]=n; producer_ready(id); "
      "if audit_libraries then audit_libraries(id) end; "
      "if audit_utf8 then audit_utf8() end; "
      "if audit_patterns then audit_patterns(id) end; "
      "if audit_strings then audit_strings(id) end; "
      "if audit_dump then audit_dump(id) end; "
      "if audit_load then audit_load(id) end; "
      "if audit_package then audit_package(id) end; "
      "if id==2 then gate2:recv() else gate3:recv() end; "
      "if id==3 and failing then error(token) end; "
      "return token,function() return shared end,42 end";
  require(pool != NULL);
  fixture = pool;
  pool->awaiting = 1;
  pool->managed_limit = 2;
  pool->force_migrate = strstr(name, "migrate") != NULL;
  check(sched_getaffinity(0, sizeof(available), &available));
  for (cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &available)) {
      if (cpus[0] < 0)
        cpus[0] = cpu;
      else {
        cpus[1] = cpu;
        break;
      }
    }
  require(cpus[0] >= 0);
  if (cpus[1] < 0)
    cpus[1] = cpus[0];
  check(pthread_mutex_init(&pool->mutex, NULL));
  check(pthread_cond_init(&pool->changed, NULL));
  check(pthread_barrier_init(&pool->parallel, NULL, 2));
  if (facade) {
    static const unsigned char capture[] = "require('fixture')";
    if (movealloc) {
      luthrum_lua_runtime_allocator_config config = {0};
      config.user = pool;
      config.alloc_fn = utf8_host_allocate;
      config.realloc_fn = utf8_allocate;
      config.free_fn = utf8_host_free;
      require(luthrum_lua_runtime_new_with_allocator(&pool->runtime, &config) ==
              LUTHRUM_LUA_RUNTIME_OK);
    } else
      require(luthrum_lua_runtime_new(&pool->runtime) ==
              LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_openlibs(pool->runtime) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_set_instruction_limit(pool->runtime, 2000000) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_register_c_module(pool->runtime, "fixture",
                                                  open_fixture) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_run_buffer(pool->runtime, capture,
                                           sizeof(capture) - 1, "task-wait", 0,
                                           NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  } else {
    pool->main =
        movealloc ? lua_newstate(utf8_allocate, pool, 0) : luaL_newstate();
    require(pool->main != NULL);
    luaL_openlibs(pool->main);
  }
  lua_pushcfunction(pool->main, block_binding);
  lua_setglobal(pool->main, "block");
  luaQ_pushmanaged(pool->main, await_task, 0);
  lua_setglobal(pool->main, "await_task");
  luaQ_pushmanaged(pool->main, reject_self_wait, 0);
  lua_setglobal(pool->main, "reject_self_wait");
  luaQ_pushmanaged(pool->main, await_report, 0);
  lua_setglobal(pool->main, "report");
  luaQ_pushmanaged(pool->main, await_ready, 0);
  lua_setglobal(pool->main, "producer_ready");
  luaQ_pushmanaged(pool->main, await_cleanup, 0);
  lua_setglobal(pool->main, "cleanup");
  lua_pushboolean(pool->main, error);
  lua_setglobal(pool->main, "failing");
  luaP_newpipe(pool->main, 1);
  lua_setglobal(pool->main, "gate2");
  luaP_newpipe(pool->main, 1);
  lua_setglobal(pool->main, "gate3");
  require(
      luaL_loadstring(
          pool->main,
          "shared={}; function await_checked(id) reject_self_wait(); "
          "local code,status,token,fn,n=await_task(id); "
          "if code==-3 then report(id,false); return 42 end; assert(code==0); "
          "if id==3 and failing then assert(status==2) else "
          "assert(status==0 and n==42 and fn()==shared) end; "
          "report(id,true,token); return 42 end") == LUA_OK);
  require(lua_pcall(pool->main, 0, 0, 0) == LUA_OK);
  if (library)
    install_library_audit(pool->main);
  if (utf8)
    install_utf8_audit(pool);
  if (pattern)
    install_pattern_audit(pool);
  if (strings)
    install_string_audit(pool);
  if (dump)
    install_dump_audit(pool);
  if (load)
    install_load_audit(pool);
  if (package)
    install_package_audit(pool);
  if (strstr(name, "generational") != NULL)
    (void)lua_gc(pool->main, LUA_GCGEN);
  for (id = 0; id < MAX_TASKS; ++id) {
    struct task *task = &pool->tasks[id];
    if (id == 2 || id == 3) {
      unsigned *token = lua_newuserdatauv(pool->main, sizeof(*token), 0);
      *token = 12345U;
      pool->await_payloads[id - 2] = token;
      lua_newtable(pool->main);
      luaQ_pushmanaged(pool->main, await_finalize, 0);
      lua_setfield(pool->main, -2, "__gc");
      lua_setmetatable(pool->main, -2);
      require(luaL_loadstring(pool->main, producer) == LUA_OK);
      lua_pushvalue(pool->main, -2);
      lua_pushinteger(pool->main, id);
      require(lua_pcall(pool->main, 2, 1, 0) == LUA_OK);
      task->state = lua_newthread(pool->main);
      lua_pushvalue(pool->main, -2);
      lua_xmove(pool->main, task->state, 1);
    } else {
      task->state = lua_newthread(pool->main);
      require(luaL_loadstring(
                  task->state,
                  id < 2 ? "return block(...)"
                  : id == 4 && driver
                      ? "local code,token,fn,n=...; if code==-3 then "
                        "report(2,false) "
                        "else assert(code==0 and n==42 and fn()==shared); "
                        "report(2,true,token) end; return 42"
                  : id == 5 ? "return await_checked(3)"
                  : id == 6 ? "local n=0; for i=1,100 do local "
                              "t={i}; n=n+t[1] end; return n"
                            : "return await_checked(2)") == LUA_OK);
      if (id < 2) {
        unsigned *payload;
        lua_pushinteger(task->state, id);
        payload = lua_newuserdatauv(task->state, sizeof(*payload), 0);
        *payload = 12345U;
      }
    }
    init_task(pool, task, id, NULL);
    if (id == 4 && driver)
      task->run = run_await_driver;
    task->target = id < 2 ? id : id == 2 ? 2 : id == 3 ? 3 : -1;
    if (id == 2 || id == 3) {
      task->state->state_poll = parallel_poll;
      task->state->state_poll_context = task;
      task->state->poll_remaining = 1;
    }
    lua_settop(pool->main, 0); /* Scheduled roots are the only child owners. */
  }
  if (early)
    pool->tasks[4].before_wait = await_before_wait;
  for (id = 0; id < MAX_WORKERS; ++id) {
    pool->workers[id].pool = pool;
    if (id >= 2) {
      pool->workers[id].bind_cpu = 1;
      pool->workers[id].cpu = cpus[id - 2];
    }
    check(pthread_create(&workers[id], NULL, worker_entry, &pool->workers[id]));
  }
  submit(pool, 0);
  submit(pool, 1);
  lock(pool);
  while (pool->entered != 2)
    wait_change(pool);
  require(pool->native == 2);
  append(&pool->requests, &pool->requests_tail, &pool->tasks[2]);
  append(&pool->requests, &pool->requests_tail, &pool->tasks[3]);
  grant(pool);
  if (helpers) {
    while (!pool->tasks[2].helper_held || !pool->tasks[3].helper_held) {
      require(!pool->tasks[2].finished && !pool->tasks[3].finished);
      wait_change(pool);
    }
    require(pool->managed == 0); /* Both C contexts are saved and inactive. */
    unlock(pool);
    (void)lua_gc(pool->main, LUA_GCCOLLECT);
    submit(pool, 6);
    lock(pool);
    while (!pool->tasks[6].finished)
      wait_change(pool);
    require(pool->native == 2 && !pool->tasks[2].finished &&
            !pool->tasks[3].finished);
    for (id = 2; id <= 3; ++id) {
      pool->tasks[id].helper_held = 0;
      append(&pool->requests, &pool->requests_tail, &pool->tasks[id]);
    }
    grant(pool);
  }
  while (!pool->await_ready[0] || !pool->await_ready[1] ||
         !pool->tasks[2].published || !pool->tasks[3].published ||
         ((library || pattern || strings || dump || load || package) &&
          !pool->tasks[2].await_waits)) {
    require(!pool->tasks[2].finished && !pool->tasks[3].finished);
    wait_change(pool);
  }
  require(pool->parallel_arrivals == 2 && pool->managed_peak == 2);
  require(pool->tasks[2].parallel_cpu == cpus[0] &&
          pool->tasks[3].parallel_cpu == cpus[1]);
  unlock(pool);
  submit(pool, 4);
  submit(pool, 5);
  submit(pool, 7);
  lock(pool);
  while (!pool->tasks[4].await_waits || !pool->tasks[5].await_waits ||
         !pool->tasks[7].await_waits || !pool->tasks[5].published ||
         !pool->tasks[7].published ||
         (early ? pool->stage != 1 : !pool->tasks[4].published))
    wait_change(pool);
  if (callback) {
    pool->callback_request = 1;
    signal_change(pool);
    while (!pool->tasks[0].await_waits || !pool->tasks[0].pinned_resource_waits)
      wait_change(pool);
    require(!pool->callback_done);
  }
  unlock(pool);
  if (!helpers)
    submit(pool, 6);
  lock(pool);
  while (!pool->tasks[6].finished)
    wait_change(pool);
  require(pool->native == 2 && !pool->tasks[0].finished &&
          !pool->tasks[1].finished);
  require(!pool->tasks[2].finished && !pool->tasks[3].finished &&
          !pool->tasks[4].finished && !pool->tasks[5].finished &&
          !pool->tasks[7].finished);
  unlock(pool);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  require(pool->finalized == 0);
  release_producer(pool, "gate2");
  if (early) {
    lock(pool);
    while (!pool->tasks[4].early_wakes)
      wait_change(pool);
    pool->stage = 2;
    signal_change(pool);
    unlock(pool);
  }
  release_producer(pool, "gate3");
  lock(pool);
  while (pool->done != 6 || (callback && !pool->callback_done))
    wait_change(pool);
  require(pool->native == 2 && pool->await_closed == 2);
  require(pool->await_winners[0] == 1 && pool->await_winners[1] == 1);
  require(pool->await_losers[0] == (callback ? 2U : 1U) &&
          pool->await_losers[1] ==
              ((library || pattern || strings || dump || load || package)
                   ? 1U
                   : 0U));
  pool->release[0] = pool->release[1] = 1;
  signal_change(pool);
  while (pool->done != MAX_TASKS)
    wait_change(pool);
  pool->stop = 1;
  signal_change(pool);
  unlock(pool);
  for (id = 0; id < MAX_WORKERS; ++id)
    check(pthread_join(workers[id], NULL));
  require(pool->managed == 0 && pool->native == 0 && pool->native_peak == 2 &&
          pool->managed_peak == 2 && pool->native_waits == 0);
  for (id = 0; id < MAX_TASKS; ++id) {
    struct task *task = &pool->tasks[id];
    require(task->status == (id == 3 && error ? LUA_ERRRUN : LUA_OK));
    require(task->root.waiters == NULL);
    if (id == 2 || id == 3)
      require(task->root.phase == LUA_TASK_COLLECTED);
    else {
      require(task->results == 1 &&
              lua_tointeger(task->state, -1) == (id < 2    ? id + 10
                                                 : id == 6 ? 5050
                                                           : 42));
      collect_task(task);
    }
    if (id == 4 || id == 5 || id == 7) {
      require(task->await_waits != 0 && task->pinned_resource_waits == 0);
      if (pool->force_migrate)
        require(task->migrations != 0);
    }
    if ((library || pattern || strings || dump || load || package) && id == 2) {
      require(task->await_waits != 0 && task->pinned_resource_waits == 0);
      if (pool->force_migrate)
        require(task->migrations != 0);
    }
    if (helpers && (id == 2 || id == 3 || (id == 0 && callback))) {
      int index;
      for (index = 0; index < pool->helper_count; ++index) {
        require(task->helper_polls[index] != 0);
        if (id == 0)
          require(task->helper_pinned[index] == task->helper_polls[index] &&
                  task->helper_migrations[index] == 0);
        else {
          require(task->helper_pinned[index] == 0);
          if (pool->force_migrate)
            require(task->helper_migrations[index] != 0);
        }
      }
    }
    test_fiber_destroy(&task->fiber);
  }
  require(G(pool->main)->tasks == NULL &&
          G(pool->main)->coroutine_scopes == NULL);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  require(pool->finalized == 2);
  if (facade)
    luthrum_lua_runtime_free(pool->runtime);
  else
    lua_close(pool->main);
  if (movealloc)
    require(pool->stream_live == 0);
  check(pthread_barrier_destroy(&pool->parallel));
  check(pthread_cond_destroy(&pool->changed));
  check(pthread_mutex_destroy(&pool->mutex));
  printf("task await %s: 2 managed permits on CPUs %d,%d, 2 blocked native "
         "slots, 4 carriers; "
         "parallel bytecode, waiting and result ownership preserved\n",
         name, cpus[0], cpus[1]);
  free(pool);
  return 0;
}
int main(int argc, char **argv) {
  struct pool *pool;
  pthread_t workers[WORKERS];
  int id, generational;
  static const unsigned char capture[] = "require('fixture')";
  require(argc == 2);
  if (strncmp(argv[1], "task_", 5) == 0)
    return exercise_task_wait(argv[1]);
  if (strncmp(argv[1], "stream_", 7) == 0)
    return exercise_stream(argv[1]);
  if (strncmp(argv[1], "wait_", 5) == 0)
    return exercise_resource_wait(argv[1]);
  if (strncmp(argv[1], "foreign_", 8) == 0)
    return exercise_callback(argv[1]);
  pool = calloc(1, sizeof(*pool));
  require(pool != NULL);
  generational = strstr(argv[1], "generational") != NULL;
  pool->budget = strstr(argv[1], "budget") != NULL;
  pool->trace = strstr(argv[1], "trace") != NULL;
  pool->native_resource = strstr(argv[1], "resource") != NULL;
  fixture = pool;
  check(pthread_mutex_init(&pool->mutex, NULL));
  check(pthread_cond_init(&pool->changed, NULL));
  require(luthrum_lua_runtime_new(&pool->runtime) == LUTHRUM_LUA_RUNTIME_OK);
  require(luthrum_lua_runtime_openlibs(pool->runtime) ==
          LUTHRUM_LUA_RUNTIME_OK);
  require(luthrum_lua_runtime_register_c_module(pool->runtime, "fixture",
                                                open_fixture) ==
          LUTHRUM_LUA_RUNTIME_OK);
  require(luthrum_lua_runtime_run_buffer(pool->runtime, capture,
                                         sizeof(capture) - 1, "capture", 0,
                                         NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  require(pool->main != NULL);
  if (pool->budget) {
    require(luthrum_lua_runtime_set_instruction_limit(pool->runtime, 60000) ==
            LUTHRUM_LUA_RUNTIME_OK);
    /* The facade owns invocation boundaries and resets the shared budget. */
    require(luthrum_lua_runtime_run_buffer(
                pool->runtime, capture, sizeof(capture) - 1, "budget-start", 0,
                NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  }
  if (pool->trace)
    require(luthrum_lua_runtime_set_trace_callback(
                pool->runtime, observe, pool) == LUTHRUM_LUA_RUNTIME_OK);
  lua_pushcfunction(pool->main, block_binding);
  lua_setglobal(pool->main, "block");
  lua_pushcfunction(pool->main, failing_binding);
  lua_setglobal(pool->main, "fail");
  lua_pushcfunction(pool->main, yielding_binding);
  lua_setglobal(pool->main, "pause");
  lua_pushcfunction(pool->main, warmup);
  lua_setglobal(pool->main, "warm");
  if (generational)
    (void)lua_gc(pool->main, LUA_GCGEN);
  for (id = 0; id < TASKS; ++id) {
    struct task *task = &pool->tasks[id];
    const char *source =
        id < 2 ? "return block(...)"
        : id == 2
            ? (pool->native_resource
                   ? "warm(); local n=0; for i=1,1000 do n=n+i end; return n"
                   : "local n=0; for i=1,1000 do n=n+i end; return n")
        : id == 3      ? (pool->trace ? "return 1+2" : "return fail()")
        : id == 4      ? "return pause()"
        : pool->budget ? "while true do end"
                       : "return fail()";
    task->pool = pool;
    task->id = id;
    task->state = lua_newthread(pool->main);
    if (id == 5 && !pool->budget)
      lua_sethook(task->state, failing_hook, LUA_MASKCOUNT, 1);
    if (pool->trace && id == 2)
      lua_sethook(task->state, lua_gethook(task->state),
                  lua_gethookmask(task->state) & ~LUA_MASKLINE,
                  lua_gethookcount(task->state));
    require(luaL_loadstring(task->state, source) == LUA_OK);
    if (pool->native_resource && id == 2)
      require(lua_resume(task->state, NULL, 0, &task->results) == LUA_YIELD);
    if (id < 2) {
      unsigned *payload;
      lua_pushinteger(task->state, id);
      payload = lua_newuserdatauv(task->state, sizeof(*payload), 0);
      *payload = 12345U;
      lua_newtable(task->state);
      lua_pushcfunction(task->state, finalize);
      lua_setfield(task->state, -2, "__gc");
      lua_setmetatable(task->state, -2);
    }
    init_task(pool, task, id, NULL);
    task->state->poll_remaining = 1;
  }
  for (id = 0; id < WORKERS; ++id) {
    pool->workers[id].pool = pool;
    check(pthread_create(&workers[id], NULL, worker_entry, &pool->workers[id]));
  }
  submit(pool, 0);
  submit(pool, 1);
  lock(pool);
  while (pool->entered != 2)
    wait_change(pool);
  require(pool->native == NATIVE);
  unlock(pool);
  if (pool->native_resource) {
    luaX_lockstate(pool->main);
    luaW_heapenter(pool->main);
  }
  submit(pool, 3); /* Cannot enter its C body until a native slot retires. */
  submit(pool, 2);
  lock(pool);
  pool->callback_request = 1;
  signal_change(pool);
  if (pool->native_resource) {
    while (!pool->tasks[2].finished || !pool->tasks[0].pinned_resource_waits)
      wait_change(pool);
    unlock(pool);
    luaW_heapleave(pool->main);
    luaX_unlockstate(pool->main);
    lock(pool);
  }
  while (!pool->tasks[2].finished || !pool->callback_done ||
         !pool->native_waits)
    wait_change(pool);
  require(!pool->tasks[3].finished && pool->entered == 2);
  require(pool->native == NATIVE && pool->managed <= MANAGED);
  unlock(pool);
  require(lua_tointeger(pool->tasks[2].state, -1) == 500500);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  require(pool->finalized == 0);
  lock(pool);
  pool->release[0] = 1;
  signal_change(pool);
  while (!pool->tasks[3].finished)
    wait_change(pool);
  pool->release[1] = 1;
  signal_change(pool);
  while (pool->done != 4)
    wait_change(pool);
  unlock(pool);
  submit(pool, 4);
  lock(pool);
  while (!pool->tasks[4].finished)
    wait_change(pool);
  unlock(pool);
  submit(pool, 5);
  lock(pool);
  while (pool->done != TASKS)
    wait_change(pool);
  pool->stop = 1;
  signal_change(pool);
  unlock(pool);
  for (id = 0; id < WORKERS; ++id)
    check(pthread_join(workers[id], NULL));
  require(pool->native == 0 && pool->managed == 0);
  require(pool->native_peak == NATIVE && pool->managed_peak == MANAGED);
  require(pool->tasks[0].pinned_waits != 0);
  require(pool->tasks[2].parks != 0);
  require(!pool->trace || pool->observed != 0);
  require(pool->tasks[0].fiber.execution.native_depth == 0);
  for (id = 0; id < TASKS; ++id) {
    struct task *task = &pool->tasks[id];
    require(task->status ==
            ((id == 3 && !pool->trace) || id == 5 ? LUA_ERRRUN : LUA_OK));
    if (id == 3 && pool->trace)
      require(lua_tointeger(task->state, -1) == 3);
    if (id < 2)
      require(lua_tointeger(task->state, -1) == id + 10);
    if (id == 4)
      require(lua_tointeger(task->state, -1) == 42);
    if (id == 5 && pool->budget)
      require(strstr(lua_tostring(task->state, -1), "instruction limit") !=
              NULL);
    if (id == 5 && !pool->budget)
      require(strstr(lua_tostring(task->state, -1), "hook expected") != NULL);
    require(task->fiber.execution.protected == NULL);
    collect_task(task);
    (void)lua_closethread(task->state, NULL);
    lua_settop(task->state, 0);
    test_fiber_destroy(&task->fiber);
  }
  lua_settop(pool->main, 0);
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  require(pool->finalized == 2);
  luthrum_lua_runtime_free(pool->runtime);
  check(pthread_cond_destroy(&pool->changed));
  check(pthread_mutex_destroy(&pool->mutex));
  printf(
      "bounded native scheduler: 1 managed permit, 2 native slots, 3 carriers; "
      "Lua and nested callback progress before release\n");
  free(pool);
  return 0;
}
