#define _GNU_SOURCE
#include "private/context.h"
#include <laccess.h>
#include <lauxlib.h>
#include <lcallback.h>
#include <ldo.h>
#include <lgc.h>
#include <lpipe.h>
#include <ltable.h>
#include <lualib.h>
#include <luthrum/lua_runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { MANAGED = 1, NATIVE = 2, WORKERS = MANAGED + NATIVE, TASKS = 6 };
enum reason { READY, NATIVE_WAIT, RESOURCE_WAIT, DONE };
enum wait_kind { OBJECT_WAIT, GATE_WAIT, GC_WAIT, EARLY_WAIT };
struct pool;
struct task {
  struct pool *pool;
  struct test_fiber fiber;
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
  void (*run)(struct task *);
  void (*before_wait)(struct task *);
};
struct worker {
  struct pool *pool;
  struct test_context context;
  unsigned dispatches;
};
struct pool {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  struct task tasks[TASKS];
  struct worker workers[WORKERS];
  struct task *requests, *requests_tail, *ready, *ready_tail;
  struct task *native_wait, *native_tail;
  lua_State *main;
  luthrum_lua_runtime *runtime;
  unsigned managed, native, managed_peak, native_peak, native_waits;
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
  while (pool->managed < MANAGED && pool->requests != NULL) {
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
  if (task->pool->force_migrate)
    task->target = 1 - task->worker;
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
  LuaWorldScope scope = luaW_detach(state);
  if (execution->native_depth == 0)
    park(task, READY);
  luaW_attach(state, scope);
}
static void resource_wait(LuaExecution *execution, pthread_mutex_t *mutex,
                          LuaWaiter *waiter) {
  struct task *task = execution->context;
  struct pool *pool = task->pool;
  lock(pool);
  require(task->waiter == NULL && !task->permit);
  task->waiter = waiter;
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
        lua_resume(task->state, NULL, task->id < 2 ? 2 : 0, &task->results);
    if (task->id == 4 && task->status == LUA_YIELD)
      task->status = lua_resume(task->state, NULL, 0, &task->results);
  }
  require(task->fiber.execution.managed_depth == 0);
  require(task->fiber.execution.native_depth == 0);
  require(task->fiber.execution.protected == NULL);
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
      require(luaL_loadstring(
                  child,
                  "local n=0; for i=1,4000 do n=n+i end; "
                  "local bytes=string.dump(function() return 42 end); "
                  "local f=assert(load(bytes)); if f()~=42 then return -2 end; "
                  "warn('nested warning'); "
                  "local ok=pcall(function() error('nested expected') end); "
                  "if ok then return -1 end; return n") == LUA_OK);
      require(lua_resume(child, NULL, 0, &results) == LUA_OK);
      require(results == 1 && lua_tointeger(child, -1) == 8002000);
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
      lua_resume(task->state, NULL, task->id < 2 ? 2 : 0, &task->results);
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
int main(int argc, char **argv) {
  struct pool *pool;
  pthread_t workers[WORKERS];
  int id, generational;
  static const unsigned char capture[] = "require('fixture')";
  require(argc == 2);
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
