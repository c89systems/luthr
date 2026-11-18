#include <lauxlib.h>
#include <lcallback.h>
#include <ldo.h>
#include <ltask.h>
#include <lualib.h>
#include <luthr/lua_runtime.h>
#include <pthread.h>
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
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *main, *receivers[2];
  LuaTaskRoot tasks[2];
  LuaExecution executions[2];
  luthr_lua_runtime *runtime;
  const char *mode;
  size_t live;
  unsigned finalized, closed, failures;
  int deny, entered, release, armed, collect_status, status, results;
  int waiting, second_code, second_status, second_results;
  int notified;
  LuaWaiter *waiter;
};
static struct fixture *active;
static __thread int host_awaiting;
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __wrap_pthread_cond_wait(pthread_cond_t *condition,
                             pthread_mutex_t *mutex) {
  if (host_awaiting && condition == &G(active->main)->world.changed) {
    require(mutex == &G(active->main)->world.mutex);
    require(active->tasks[0].phase == LUA_TASK_COLLECTING);
    require(pthread_mutex_lock(&active->mutex) == 0);
    active->waiting = 1;
    require(pthread_cond_broadcast(&active->changed) == 0);
    require(pthread_mutex_unlock(&active->mutex) == 0);
  }
  return __real_pthread_cond_wait(condition, mutex);
}
static void hold(void) {
  require(pthread_mutex_lock(&active->mutex) == 0);
  active->entered = 1;
  require(pthread_cond_broadcast(&active->changed) == 0);
  while (!active->release)
    require(pthread_cond_wait(&active->changed, &active->mutex) == 0);
  require(pthread_mutex_unlock(&active->mutex) == 0);
}
int __real_lua_checkstack(lua_State *, int);
int __wrap_lua_checkstack(lua_State *state, int count) {
  int result = __real_lua_checkstack(state, count);
  if (active->armed && state == active->receivers[0]) {
    active->armed = 0;
    if (strstr(active->mode, "rollback") != NULL) {
      require(result == 0 && active->failures != 0);
      active->deny = 0;
    }
    hold();
  }
  return result;
}
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *test = context;
  void *memory;
  if (old == NULL)
    old_size = 0;
  if (size == 0) {
    test->live -= old_size;
    free(old);
    return NULL;
  }
  if (test->deny && size > old_size) {
    test->failures++;
    return NULL;
  }
  memory = malloc(size);
  if (memory != NULL) {
    if (old != NULL) {
      memcpy(memory, old, old_size < size ? old_size : size);
      free(old);
    }
    test->live = test->live - old_size + size;
  }
  return memory;
}
static void *host_allocate(void *context, size_t size) {
  return allocate(context, NULL, 0, size);
}
static void host_free(void *context, void *memory, size_t size) {
  (void)allocate(context, memory, size, 0);
}
static int capture(void *context) {
  active->main = context;
  return 0;
}
static void execute(lua_State *state, const char *source) {
  int status = luaL_loadstring(state, source);
  if (status == LUA_OK)
    status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "task ownership: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static int finalize(lua_State *state) {
  unsigned *token = lua_touserdata(state, 1);
  require(token != NULL && *token == 12345);
  *token = 0;
  active->finalized++;
  return 0;
}
static int cleanup(lua_State *state) {
  unsigned *token = lua_touserdata(state, 1);
  require(token != NULL && *token == 12345);
  active->closed++;
  return 0;
}
static int hold_execution(lua_State *state) {
  (void)state;
  hold();
  return 0;
}
static int fail_allocation(lua_State *state) {
  active->deny = 1;
  (void)lua_newuserdatauv(state, 4096, 0);
  abort();
}
static int obtain(lua_State *state) {
  int index = (int)luaL_checkinteger(state, 1);
  require(index >= 0 && index < 2);
  luaX_lockstate(state);
  luaD_checkstack(state, 1);
  setthvalue(state, s2v(state->top.p++), active->tasks[index].thread);
  luaX_unlockstate(state);
  return 1;
}
static int forty_two(lua_State *state) {
  lua_pushinteger(state, 42);
  return 1;
}
static void callback_invoke(lua_State *state, void *context) {
  (void)state;
  (void)context;
}
struct snapshot {
  int count;
  TStatus status;
  TValue values[8];
};
static void snapshot(lua_State *state, struct snapshot *value, int compare) {
  int i, count;
  luaX_lockstate(state);
  count = (int)(state->top.p - (state->ci->func.p + 1));
  require(count >= 0 && count <= 8);
  if (compare)
    require(count == value->count && state->status == value->status);
  else {
    value->count = count;
    value->status = state->status;
  }
  for (i = 0; i < count; ++i) {
    const TValue *item = s2v(state->ci->func.p + 1 + i);
    if (!compare)
      value->values[i] = *item;
    else {
      require(rawtt(item) == rawtt(&value->values[i]));
      require(memcmp(&item->value_, &value->values[i].value_, sizeof(Value)) ==
              0);
    }
  }
  luaX_unlockstate(state);
}
static void busy(int index) {
  char script[512];
  struct snapshot before;
  lua_State *state = active->tasks[index].thread;
  snapshot(state, &before, 0);
  require(
      snprintf(script, sizeof(script),
               "local co=obtain(%d); assert(coroutine.status(co)=='normal'); "
               "local ok=coroutine.resume(co); assert(not ok); "
               "local f=coroutine.wrap(function() end); "
               "debug.setupvalue(f,1,co); assert(not pcall(f)); "
               "assert(not pcall(coroutine.close,co))",
               index) < (int)sizeof(script));
  execute(active->main, script);
  snapshot(state, &before, 1);
}
static void collect_gc(void) {
  (void)lua_gc(active->main, LUA_GCCOLLECT);
  (void)lua_gc(active->main, LUA_GCCOLLECT);
}
static void create_task(int index) {
  struct fixture *test = active;
  lua_State *state = test->main, *child;
  unsigned *token = lua_newuserdatauv(state, sizeof(*token), 0);
  *token = 12345;
  lua_newtable(state);
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  lua_setmetatable(state, -2);
  require(luaL_loadstring(
              state,
              "local token=...; return function() "
              "local resource <close> =setmetatable({}, {__close=function() "
              "cleanup(token); if cleanup_wait then hold_execution() end; "
              "if cleanup_fail then "
              "error({token=token,marker='cleanup expected'}) end end}); "
              "if holding then hold_execution() end; "
              "if yielding then coroutine.yield(token) end; "
              "if selfclosing then coroutine.close() end; "
              "if failing then error(token) end; "
              "if oom then fail_allocation() end; "
              "return token,function() return 42 end,42 end") == LUA_OK);
  lua_pushvalue(state, -2);
  require(lua_pcall(state, 1, 1, 0) == LUA_OK);
  child = lua_newthread(state);
  lua_pushvalue(state, -2);
  lua_xmove(state, child, 1);
  test->deny = 1; /* Registration itself cannot require an allocation. */
  require(luaTK_admit(child, &test->tasks[index], &test->executions[index]) ==
          LUA_OK);
  test->deny = 0;
  require(test->failures == 0);
  lua_settop(state, 0);
}
static void drive(LuaTaskRoot *task) {
  NativeStackContext context = {0}, *previous;
  int status, results, saved_status;
  context.execution = task->execution;
  previous = luaE_setnativestackcontext(&context);
  status = LUA_OK;
  results = 0;
  require(luaTK_finish(task, &status, &results) == LUA_TK_INVALID);
  status = luaTK_resume(task, 0, &results);
  if (status == LUA_YIELD) {
    saved_status = status;
    require(luaTK_finish(task, &status, &results) == LUA_TK_INVALID);
    require(status == saved_status && results == 1);
    lua_pop(task->thread, results);
    status = luaTK_resume(task, 0, &results);
  }
  require(status == LUA_OK || status == LUA_ERRRUN || status == LUA_ERRMEM);
  saved_status = results + 1;
  require(luaTK_finish(task, &status, &saved_status) == LUA_TK_INVALID);
  require(luaTK_resume(task, 0, &saved_status) == LUA_TK_INVALID);
  require(luaTK_finish(task, &status, &results) == LUA_OK);
  active->deny = 0;
  require(luaTK_resume(task, 0, &saved_status) == LUA_TK_INVALID);
  require(luaTK_finish(task, &status, &results) == LUA_TK_INVALID);
  require(task->execution->protected == NULL &&
          task->execution->managed_depth == 0 &&
          task->execution->native_depth == 0 &&
          context.coroutine_scopes == NULL);
  luaE_setnativestackcontext(previous);
}
static void *owner_entry(void *context) {
  struct fixture *test = context;
  drive(&test->tasks[1]); /* Completion order differs from admission. */
  if (strstr(test->mode, "active") != NULL)
    execute(test->main, "holding=true");
  if (strstr(test->mode, "finish_error") != NULL)
    execute(test->main, "cleanup_wait=true");
  drive(&test->tasks[0]);
  return NULL;
}
static void *collector_entry(void *context) {
  struct fixture *test = context;
  test->collect_status = luaTK_collect(test->receivers[0], &test->tasks[0],
                                       &test->status, &test->results);
  return NULL;
}
static void host_wait(LuaExecution *execution, pthread_mutex_t *mutex,
                      LuaWaiter *waiter) {
  struct fixture *test = execution->context;
  require(pthread_mutex_lock(&test->mutex) == 0);
  test->waiter = waiter;
  test->waiting = 1;
  require(pthread_mutex_unlock(mutex) == 0);
  require(pthread_cond_broadcast(&test->changed) == 0);
  while (!test->notified)
    require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
  require(pthread_mutex_unlock(&test->mutex) == 0);
}
static void host_wake(LuaExecution *execution, LuaWaiter *waiter) {
  struct fixture *test = execution->context;
  require(pthread_mutex_lock(&test->mutex) == 0);
  require(test->waiter == waiter && !test->notified);
  test->notified = 1;
  require(pthread_cond_broadcast(&test->changed) == 0);
  require(pthread_mutex_unlock(&test->mutex) == 0);
}
static void *awaiter_entry(void *context) {
  struct fixture *test = context;
  LuaExecution execution = {0};
  NativeStackContext native = {0}, *previous = NULL;
  int logical = strstr(test->mode, "notify") != NULL;
  test->second_status = test->second_results = 777;
  if (logical) {
    execution.context = test;
    execution.wait = host_wait;
    execution.wake = host_wake;
    execution.native_depth = 1;
    native.execution = &execution;
    previous = luaE_setnativestackcontext(&native);
  }
  host_awaiting = !logical;
  test->second_code = luaTK_await(test->receivers[1], &test->tasks[0],
                                  &test->second_status, &test->second_results);
  host_awaiting = 0;
  if (logical) {
    require(execution.native_depth == 1 && execution.managed_depth == 0 &&
            execution.protected == NULL && native.coroutine_scopes == NULL);
    luaE_setnativestackcontext(previous);
  }
  return NULL;
}
static int collect_result(lua_State *state, LuaTaskRoot *task, int *status,
                          int *results) {
  if (strstr(active->mode, "await") != NULL)
    return luaTK_await(state, task, status, results);
  return luaTK_collect(state, task, status, results);
}
static void verify_result(lua_State *state, int status, int results) {
  if (strstr(active->mode, "selfclose") != NULL)
    require(status == LUA_OK && results == 0);
  else if (strstr(active->mode, "oom") != NULL) {
    require(status == LUA_ERRMEM && results == 1 && lua_isstring(state, -1));
  } else if (strstr(active->mode, "error") != NULL) {
    require(status == LUA_ERRRUN && results == 1);
    if (strstr(active->mode, "cleanup") != NULL) {
      lua_getfield(state, -1, "marker");
      require(strcmp(lua_tostring(state, -1), "cleanup expected") == 0);
      lua_pop(state, 1);
      lua_getfield(state, -1, "token");
      require(lua_isuserdata(state, -1));
      lua_pop(state, 1);
    } else
      require(lua_isuserdata(state, -1));
  } else {
    require(status == LUA_OK && results == 3 && lua_isuserdata(state, -3));
    require(lua_tointeger(state, -1) == 42 && lua_isfunction(state, -2));
    lua_pushvalue(state, -2);
    require(lua_pcall(state, 0, 1, 0) == LUA_OK);
    require(lua_tointeger(state, -1) == 42);
    lua_pop(state, 1);
  }
}
static void await_hold(void) {
  require(pthread_mutex_lock(&active->mutex) == 0);
  while (!active->entered)
    require(pthread_cond_wait(&active->changed, &active->mutex) == 0);
  require(pthread_mutex_unlock(&active->mutex) == 0);
}
static void release_hold(void) {
  require(pthread_mutex_lock(&active->mutex) == 0);
  active->release = 1;
  require(pthread_cond_broadcast(&active->changed) == 0);
  require(pthread_mutex_unlock(&active->mutex) == 0);
}
static int fill_receiver(lua_State *receiver) {
  int padding;
  require(lua_checkstack(receiver, 128));
  luaX_lockstate(receiver);
  padding = (int)(receiver->stack_last.p - (receiver->ci->func.p + 1)) - 1;
  luaX_unlockstate(receiver);
  require(lua_checkstack(receiver, padding));
  lua_settop(receiver, padding);
  return padding;
}
static void *run(void *context) {
  struct fixture *test = context;
  pthread_t owner, collector, awaiter;
  LuaTaskRoot other = {0};
  LuaCallback callback;
  lua_State *first_result;
  int index, status = 777, results = 777;
  int rollback = strstr(test->mode, "rollback") != NULL;
  int facade = strstr(test->mode, "facade") != NULL;
  int early_gc = strstr(test->mode, "oom") != NULL ||
                 strstr(test->mode, "selfclose") != NULL;
  if (facade) {
    static const unsigned char load[] = "require('fixture')";
    luthr_lua_runtime_allocator_config config = {0};
    config.user = test;
    config.alloc_fn = host_allocate;
    config.realloc_fn = allocate;
    config.free_fn = host_free;
    require(luthr_lua_runtime_new_with_allocator(&test->runtime, &config) ==
            LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_openlibs(test->runtime) == LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_register_c_module(
                test->runtime, "fixture", capture) == LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_set_instruction_limit(test->runtime, 100000) ==
            LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_run_buffer(test->runtime, load, sizeof(load) - 1,
                                         "task-ownership", 0, NULL,
                                         0) == LUTHR_LUA_RUNTIME_OK);
  } else {
    test->main = lua_newstate(allocate, test, 0);
    require(test->main != NULL);
    luaL_openlibs(test->main);
  }
  lua_pushcfunction(test->main, obtain);
  lua_setglobal(test->main, "obtain");
  lua_pushcfunction(test->main, cleanup);
  lua_setglobal(test->main, "cleanup");
  lua_pushcfunction(test->main, hold_execution);
  lua_setglobal(test->main, "hold_execution");
  lua_pushcfunction(test->main, fail_allocation);
  lua_setglobal(test->main, "fail_allocation");
  if (strstr(test->mode, "generational") != NULL)
    (void)lua_gc(test->main, LUA_GCGEN);
  if (strstr(test->mode, "yield") != NULL)
    execute(test->main, "yielding=true");
  if (strstr(test->mode, "error") != NULL)
    execute(test->main, "failing=true");
  if (strstr(test->mode, "cleanup") != NULL)
    execute(test->main, "cleanup_fail=true");
  if (strstr(test->mode, "selfclose") != NULL)
    execute(test->main, "selfclosing=true");
  if (strstr(test->mode, "oom") != NULL)
    execute(test->main, "oom=true");
  require(luaTK_admit(test->main, &other, &test->executions[0]) ==
          LUA_TK_INVALID);
  for (index = 0; index < 2; ++index)
    create_task(index);
  require(luaTK_admit(test->tasks[0].thread, &other, &test->executions[1]) ==
          LUA_TK_INVALID);
  require(luaTK_admit(test->tasks[0].thread, &test->tasks[0],
                      &test->executions[0]) == LUA_TK_INVALID);
  {
    lua_State *spare = lua_newthread(test->main);
    lua_pushcfunction(spare, forty_two);
    require(luaTK_admit(spare, &other, &test->executions[0]) == LUA_TK_INVALID);
    lua_pop(test->main, 1);
  }
  for (index = 0; index < 2; ++index) {
    busy(index);
    require(luaTK_resume(&test->tasks[index], 0, &results) == LUA_TK_INVALID);
    require(luaTK_collect(test->main, &test->tasks[index], &status, &results) ==
            LUA_TK_PENDING);
  }
  require(status == 777 && results == 777);
  collect_gc();
  require(test->finalized == 0);
  lua_pushcfunction(test->main, forty_two);
  require(luaCB_init(test->main, &callback, 0) == LUA_OK);
  require(luaCB_call(&callback, &test->executions[0], callback_invoke, NULL,
                     NULL, 0) == LUA_CB_INVALID);
  require(luaTK_admit(callback.thread, &other, &test->executions[0]) ==
          LUA_TK_INVALID);
  luaCB_revoke(&callback);
  require(luaCB_destroy(test->main, &callback) == LUA_OK);
  lua_settop(test->main, 0);
  require(pthread_create(&owner, NULL, owner_entry, test) == 0);
  if (strstr(test->mode, "active") != NULL ||
      strstr(test->mode, "finish_error") != NULL) {
    await_hold();
    busy(0);
    collect_gc();
    require(test->finalized == 0);
    release_hold();
  }
  require(pthread_join(owner, NULL) == 0);
  require(luaTK_collect(test->tasks[0].thread, &test->tasks[0], &status,
                        &results) == LUA_TK_INVALID);
  require(luaTK_await(test->tasks[0].thread, &test->tasks[0], &status,
                      &results) == LUA_TK_INVALID);
  {
    lua_State *other_vm = lua_newstate(allocate, test, 0);
    require(other_vm != NULL);
    require(luaTK_collect(other_vm, &test->tasks[0], &status, &results) ==
            LUA_TK_INVALID);
    require(luaTK_await(other_vm, &test->tasks[0], &status, &results) ==
            LUA_TK_INVALID);
    lua_close(other_vm);
  }
  require(test->closed == 2);
  if (strstr(test->mode, "oom") != NULL)
    require(test->failures != 0);
  collect_gc();
  require(test->finalized == (early_gc ? 2U : 0U));
  for (index = 0; index < 2; ++index)
    busy(index);
  test->receivers[0] = lua_newthread(test->main);
  test->receivers[1] = lua_newthread(test->main);
  first_result = test->receivers[0];
  if (strstr(test->mode, "retry") != NULL) {
    lua_State *receiver = test->receivers[0];
    int padding = fill_receiver(receiver);
    struct snapshot before;
    snapshot(test->tasks[0].thread, &before, 0);
    test->deny = 1;
    require(collect_result(receiver, &test->tasks[0], &status, &results) ==
            LUA_TK_STACK);
    test->deny = 0;
    require(test->failures != 0 && lua_gettop(receiver) == padding);
    snapshot(test->tasks[0].thread, &before, 1);
    collect_gc();
    require(test->finalized == 0);
    lua_settop(receiver, 0);
  }
  if (strstr(test->mode, "race") != NULL ||
      strstr(test->mode, "notify") != NULL) {
    if (rollback) {
      (void)fill_receiver(test->receivers[0]);
      test->deny = 1;
    }
    test->entered = test->release = 0;
    test->status = test->results = 777;
    test->armed = 1;
    require(pthread_create(&collector, NULL, collector_entry, test) == 0);
    await_hold();
    require(luaTK_collect(test->receivers[1], &test->tasks[0], &status,
                          &results) == LUA_TK_PENDING);
    require(lua_gettop(test->receivers[1]) == 0);
    if (strstr(test->mode, "await") != NULL) {
      require(pthread_create(&awaiter, NULL, awaiter_entry, test) == 0);
      require(pthread_mutex_lock(&test->mutex) == 0);
      while (!test->waiting)
        require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
      require(pthread_mutex_unlock(&test->mutex) == 0);
    }
    busy(0);
    collect_gc();
    require(test->finalized == 0);
    release_hold();
    require(pthread_join(collector, NULL) == 0);
    if (strstr(test->mode, "await") != NULL) {
      require(pthread_join(awaiter, NULL) == 0);
      if (rollback) {
        require(test->second_code == LUA_OK && test->second_status == LUA_OK &&
                test->second_results == 3);
        first_result = test->receivers[1];
      } else
        require(test->second_code == LUA_TK_COLLECTED &&
                test->second_status == 777 && test->second_results == 777);
    }
    require(test->collect_status == (rollback ? LUA_TK_STACK : LUA_OK));
    if (rollback) {
      require(test->status == 777 && test->results == 777);
      status = test->second_status;
      results = test->second_results;
    } else {
      status = test->status;
      results = test->results;
    }
  } else
    require(collect_result(test->receivers[0], &test->tasks[0], &status,
                           &results) == LUA_OK);
  verify_result(first_result, status, results);
  collect_gc();
  require(test->finalized == (early_gc ? 2U : 0U));
  require(luaTK_collect(test->main, &test->tasks[0], &status, &results) ==
          LUA_TK_COLLECTED);
  busy(1);
  require(collect_result(test->receivers[1], &test->tasks[1], &status,
                         &results) == LUA_OK);
  verify_result(test->receivers[1], status, results);
  require(G(test->main)->tasks == NULL);
  require(test->executions[0].task == NULL && test->executions[1].task == NULL);
  lua_settop(test->receivers[0], 0);
  lua_settop(test->receivers[1], 0);
  lua_settop(test->main, 0);
  collect_gc();
  require(test->finalized == 2);
  execute(test->main,
          "holding=false; failing=false; cleanup_fail=false; "
          "yielding=false; selfclosing=false; oom=false; cleanup_wait=false");
  {
    lua_State *child = lua_newthread(test->main);
    lua_pushcfunction(child, forty_two);
    require(luaTK_admit(child, &other, &test->executions[0]) == LUA_OK);
    drive(&other);
    require(luaTK_collect(test->main, &other, &status, &results) == LUA_OK);
    require(status == LUA_OK && results == 1 &&
            lua_tointeger(test->main, -1) == 42);
  }
  require(G(test->main)->tasks == NULL &&
          G(test->main)->coroutine_scopes == NULL);
  if (facade)
    luthr_lua_runtime_free(test->runtime);
  else
    lua_close(test->main);
  require(test->live == 0 && test->finalized == 2);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *test = calloc(1, sizeof(*test));
  pthread_t worker;
  require(test != NULL && argc == 2);
  active = test;
  test->mode = argv[1];
  require(pthread_mutex_init(&test->mutex, NULL) == 0);
  require(pthread_cond_init(&test->changed, NULL) == 0);
  require(pthread_create(&worker, NULL, run, test) == 0);
  require(pthread_join(worker, NULL) == 0);
  require(pthread_cond_destroy(&test->changed) == 0);
  require(pthread_mutex_destroy(&test->mutex) == 0);
  free(test);
  return 0;
}
