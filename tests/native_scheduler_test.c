#define _GNU_SOURCE
#include "private/context.h"
#include <lauxlib.h>
#include <ldo.h>
#include <lualib.h>
#include <luthrum/lua_runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { MANAGED = 1, NATIVE = 2, WORKERS = MANAGED + NATIVE, TASKS = 6 };
enum reason { READY, NATIVE_WAIT, DONE };
struct pool;
struct task {
  struct pool *pool;
  struct test_fiber fiber;
  lua_State *state;
  struct task *next;
  enum reason reason;
  int permit, native, pinned, finished, status, results, id;
  unsigned parks, migrations, pinned_waits;
  long last_carrier;
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
static void task_entry(void *argument) {
  struct task *task = argument;
  task->status =
      lua_resume(task->state, NULL, task->id < 2 ? 2 : 0, &task->results);
  if (task->id == 4 && task->status == LUA_YIELD)
    task->status = lua_resume(task->state, NULL, 0, &task->results);
  require(task->fiber.execution.managed_depth == 0);
  require(task->fiber.execution.native_depth == 0);
  require(task->fiber.execution.protected == NULL);
  require(!task->permit && !task->native);
  task->reason = DONE;
  test_fiber_pause(&task->fiber);
  abort();
}
static void *worker_entry(void *argument) {
  struct worker *worker = argument;
  struct pool *pool = worker->pool;
  for (;;) {
    struct task *task;
    lock(pool);
    while (!pool->stop && pool->ready == NULL)
      wait_change(pool);
    if (pool->stop) {
      unlock(pool);
      return NULL;
    }
    task = take(&pool->ready, &pool->ready_tail);
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
int main(int argc, char **argv) {
  struct pool *pool = calloc(1, sizeof(*pool));
  pthread_t workers[WORKERS];
  int id, generational;
  static const unsigned char capture[] = "require('fixture')";
  require(pool != NULL && argc == 2);
  generational = strstr(argv[1], "generational") != NULL;
  pool->budget = strstr(argv[1], "budget") != NULL;
  pool->trace = strstr(argv[1], "trace") != NULL;
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
  if (generational)
    (void)lua_gc(pool->main, LUA_GCGEN);
  for (id = 0; id < TASKS; ++id) {
    struct task *task = &pool->tasks[id];
    const char *source =
        id < 2         ? "return block(...)"
        : id == 2      ? "local n=0; for i=1,1000 do n=n+i end; return n"
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
    check(test_fiber_create(&task->fiber, task_entry, task));
    task->fiber.execution.context = task;
    task->fiber.execution.managed_enter = managed_enter;
    task->fiber.execution.managed_leave = managed_leave;
    task->fiber.execution.native_enter = native_enter;
    task->fiber.execution.native_leave = native_leave;
    task->fiber.execution.native_restore = native_restore;
    task->fiber.execution.poll = poll;
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
  submit(pool, 3); /* Cannot enter its C body until a native slot retires. */
  submit(pool, 2);
  lock(pool);
  pool->callback_request = 1;
  signal_change(pool);
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
