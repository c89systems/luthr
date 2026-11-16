#include <lauxlib.h>
#include <ldo.h>
#include <ltask.h>
#include <lualib.h>
#include <luthrum/lua_runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *main;
  luthrum_lua_runtime *runtime;
  LuaTaskRoot roots[2];
  LuaExecution executions[2];
  const char *mode;
  size_t live;
  unsigned failures, started, closed, finalized, handlers;
  int armed, entered, release, returned, finish, yielded, resume;
  int callback_request, callback_done, deny, status[2], results[2];
  int cleanup_entered, cleanup_release;
};
struct actor {
  struct fixture *test;
  int index;
};
static struct fixture *active;
static __thread int native_marker;
static void lock(struct fixture *test) {
  require(pthread_mutex_lock(&test->mutex) == 0);
}
static void unlock(struct fixture *test) {
  require(pthread_mutex_unlock(&test->mutex) == 0);
}
static void notify(struct fixture *test) {
  require(pthread_cond_broadcast(&test->changed) == 0);
}
static void waitfor(struct fixture *test) {
  require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
}
int __real_lua_resume(lua_State *, lua_State *, int, int *);
int __wrap_lua_resume(lua_State *state, lua_State *from, int nargs,
                      int *results) {
  int status = __real_lua_resume(state, from, nargs, results);
  if (strstr(active->mode, "late") && state == active->roots[0].thread &&
      status == LUA_OK) {
    lock(active);
    active->entered = 1;
    notify(active);
    while (!active->release)
      waitfor(active);
    unlock(active);
  }
  return status;
}
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *test = context;
  void *memory;
  if (old == NULL)
    old_size = 0;
  if (!size) {
    test->live -= old_size;
    free(old);
    return NULL;
  }
  if (test->deny && size > old_size) {
    test->failures++;
    return NULL;
  }
  memory = malloc(size);
  if (memory) {
    if (old) {
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
static void host_free(void *context, void *p, size_t size) {
  (void)allocate(context, p, size, 0);
}
static int capture(void *state) {
  active->main = state;
  return 0;
}
static void execute(lua_State *state, const char *source) {
  int status = luaL_loadstring(state, source);
  if (status == LUA_OK)
    status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "task stop: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static int mark_started(lua_State *state) {
  (void)state;
  active->started++;
  return 0;
}
static int handler(lua_State *state) {
  (void)state;
  active->handlers++;
  return 1;
}
static int cleanup(lua_State *state) {
  unsigned *token = lua_touserdata(state, 1);
  require(token != NULL && *token == 12345);
  require(active->roots[0].phase == LUA_TASK_FINISHING);
  active->closed++;
  if (strstr(active->mode, "cleanup_wait")) {
    pthread_t carrier = pthread_self();
    require(luaQ_current()->native_depth != 0);
    native_marker = 321;
    lock(active);
    active->cleanup_entered = 1;
    notify(active);
    while (!active->cleanup_release)
      waitfor(active);
    unlock(active);
    require(pthread_equal(carrier, pthread_self()) && native_marker == 321);
  }
  return 0;
}
static int finalize(lua_State *state) {
  unsigned *token = lua_touserdata(state, 1);
  require(token != NULL && *token == 12345);
  *token = 0;
  active->finalized++;
  return 0;
}
static void poll(LuaExecution *execution, lua_State *state) {
  struct fixture *test = execution->context;
  if (test->armed) {
    LuaWorldScope scope;
    if (strstr(test->mode, "parser")) {
      require(!isLua(state->ci) && state->native_roots != NULL);
    }
    test->armed = 0;
    scope = luaW_detach(state);
    lock(test);
    test->entered = 1;
    notify(test);
    while (!test->release)
      waitfor(test);
    unlock(test);
    luaW_attach(state, scope);
  }
}
static int block(lua_State *state) {
  struct fixture *test = active;
  pthread_t carrier = pthread_self();
  require(luaQ_current() != NULL && luaQ_current()->native_depth != 0);
  native_marker = 123;
  lock(test);
  test->entered = 1;
  notify(test);
  while (!test->release) {
    if (test->callback_request && !test->callback_done) {
      lua_State *child;
      int results = 0;
      unlock(test);
      if (strstr(test->mode, "foreign_vm")) {
        child = lua_newstate(allocate, test, 0);
        require(child != NULL);
        require(luaL_loadstring(child, "return 42") == LUA_OK);
        require(lua_pcall(child, 0, 1, 0) == LUA_OK &&
                lua_tointeger(child, -1) == 42);
        lua_close(child);
      } else {
        child = lua_newthread(state);
        require(luaL_loadstring(child, "callback_ran=true; return 42") ==
                LUA_OK);
        if (strstr(test->mode, "unprotected")) {
          lua_call(child, 0, 1);
          require(lua_tointeger(child, -1) == 42);
          require(lua_closethread(child, state) == LUA_OK);
        } else {
          require(lua_resume(child, state, 0, &results) == LUA_ERRRUN &&
                  results >= 1);
          require(strcmp(lua_tostring(child, -1), "task stopped") == 0);
          require(lua_closethread(child, state) == LUA_ERRRUN);
        }
        lua_pop(state, 1);
      }
      require(pthread_equal(carrier, pthread_self()) && native_marker == 123);
      lock(test);
      test->callback_done = 1;
      notify(test);
    } else
      waitfor(test);
  }
  unlock(test);
  require(pthread_equal(carrier, pthread_self()) && native_marker == 123);
  return 0;
}
static int native_continue(lua_State *state, int status, lua_KContext context) {
  (void)context;
  require(status == LUA_YIELD);
  return block(state);
}
static int native_yield(lua_State *state) {
  active->started++;
  return lua_yieldk(state, 0, 0, native_continue);
}
static void create_tasks(struct fixture *test) {
  lua_State *state = test->main, *child;
  unsigned *token = lua_newuserdatauv(state, sizeof(*token), 0);
  *token = 12345;
  lua_newtable(state);
  luaQ_pushmanaged(state, finalize, 0);
  lua_setfield(state, -2, "__gc");
  lua_setmetatable(state, -2);
  if (strstr(test->mode, "continuation")) {
    lua_pushvalue(state, -1);
    lua_pushcclosure(state, native_yield, 1);
  } else {
    require(
        luaL_loadstring(
            state,
            "local token=...; return function() mark_started(); "
            "if completing then return 42 end; "
            "local resource <close> =setmetatable({}, {__close=function() "
            "cleanup(token); "
            "local n=0; for i=1,4000 do n=n+i end; cleaned_loop=n; "
            "if cleanup_fail then error('cleanup expected') end end}); "
            "if yielding then coroutine.yield(token) end; "
            "if native then block() end; "
            "if parsing then local f=load(pending); assert(f); return f() end; "
            "if catching then while true do pcall(function() while true do end "
            "end) end end; "
            "if xcatching then while true do xpcall(function() while true do "
            "end "
            "end,handler) end end; "
            "local n=0; while true do n=n+1 end end") == LUA_OK);
    lua_pushvalue(state, -2);
    require(lua_pcall(state, 1, 1, 0) == LUA_OK);
  }
  child = lua_newthread(state);
  lua_pushvalue(state, -2);
  lua_xmove(state, child, 1);
  test->executions[0].context = test;
  test->executions[0].poll = poll;
  require(luaTK_admit(child, &test->roots[0], &test->executions[0]) == LUA_OK);
  lua_settop(state, 0);
  child = lua_newthread(state);
  require(luaL_loadstring(child, "return 42") == LUA_OK);
  require(luaTK_admit(child, &test->roots[1], &test->executions[1]) == LUA_OK);
  lua_settop(state, 0);
}
static void *run_actor(void *context) {
  struct actor *actor = context;
  struct fixture *test = actor->test;
  int index = actor->index;
  NativeStackContext native = *luaE_getnativestackcontext(), *previous;
  native.execution = &test->executions[index];
  previous = luaE_setnativestackcontext(&native);
  test->status[index] =
      luaTK_resume(&test->roots[index], 0, &test->results[index]);
  if (index == 0 && test->status[index] == LUA_YIELD) {
    lua_pop(test->roots[0].thread, test->results[0]);
    lock(test);
    test->yielded = 1;
    notify(test);
    while (!test->resume && !strstr(test->mode, "continuation"))
      waitfor(test);
    unlock(test);
    test->status[0] = luaTK_resume(&test->roots[0], 0, &test->results[0]);
  }
  if (index == 0) {
    lock(test);
    test->returned = 1;
    notify(test);
    while (!test->finish)
      waitfor(test);
    unlock(test);
  }
  require(luaTK_finish(&test->roots[index], &test->status[index],
                       &test->results[index]) == LUA_OK);
  require(test->executions[index].managed_depth == 0 &&
          test->executions[index].native_depth == 0);
  require(test->roots[index].thread->native_roots == NULL &&
          test->roots[index].thread->object_scopes == NULL);
  luaE_setnativestackcontext(previous);
  return NULL;
}
static void collect_peer(struct fixture *test) {
  struct actor peer = {test, 1};
  int status = -99, results = -99;
  run_actor(&peer);
  require(luaTK_collect(test->main, &test->roots[1], &status, &results) ==
          LUA_OK);
  require(status == LUA_OK && results == 1 &&
          lua_tointeger(test->main, -1) == 42);
  lua_pop(test->main, 1);
  require(!__atomic_load_n(&test->roots[1].stop_requested, __ATOMIC_ACQUIRE));
}
static void gc(struct fixture *test) {
  lua_gc(test->main, LUA_GCCOLLECT);
  lua_gc(test->main, LUA_GCCOLLECT);
}
static void *request(void *context) {
  struct fixture *test = context;
  for (int i = 0; i < 32; ++i)
    require(luaTK_requeststop(&test->roots[0]) == LUA_OK);
  return NULL;
}
static void *run(void *context) {
  struct fixture *test = context;
  pthread_t actor_thread;
  struct actor actor = {test, 0};
  lua_State *state;
  int prestart = strstr(test->mode, "prestart") != NULL;
  int yielding = strstr(test->mode, "yield") != NULL;
  int native = strstr(test->mode, "native") != NULL ||
               strstr(test->mode, "callback") != NULL ||
               strstr(test->mode, "foreign_vm") != NULL ||
               strstr(test->mode, "continuation") != NULL ||
               strstr(test->mode, "unprotected") != NULL;
  int unprotected = strstr(test->mode, "unprotected") != NULL;
  int continuation = strstr(test->mode, "continuation") != NULL;
  int denial = strstr(test->mode, "oom") != NULL;
  int late = strstr(test->mode, "late") != NULL;
  require(pthread_mutex_init(&test->mutex, NULL) == 0);
  require(pthread_cond_init(&test->changed, NULL) == 0);
  if (strstr(test->mode, "facade")) {
    static const unsigned char source[] = "require('fixture')";
    luthrum_lua_runtime_allocator_config config = {0};
    config.user = test;
    config.alloc_fn = host_allocate;
    config.realloc_fn = allocate;
    config.free_fn = host_free;
    require(luthrum_lua_runtime_new_with_allocator(&test->runtime, &config) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_openlibs(test->runtime) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_register_c_module(
                test->runtime, "fixture", capture) == LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_run_buffer(test->runtime, source,
                                           sizeof(source) - 1, "task-stop", 0,
                                           NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  } else {
    test->main = lua_newstate(allocate, test, 0);
    require(test->main != NULL);
    luaL_openlibs(test->main);
  }
  state = test->main;
  if (strstr(test->mode, "generational"))
    lua_gc(state, LUA_GCGEN);
  luaQ_pushmanaged(state, mark_started, 0);
  lua_setglobal(state, "mark_started");
  luaQ_pushmanaged(state, handler, 0);
  lua_setglobal(state, "handler");
  if (strstr(test->mode, "cleanup_wait"))
    lua_pushcfunction(state, cleanup);
  else
    luaQ_pushmanaged(state, cleanup, 0);
  lua_setglobal(state, "cleanup");
  lua_pushcfunction(state, block);
  lua_setglobal(state, "block");
  lua_pushboolean(state, native);
  lua_setglobal(state, "native");
  lua_pushboolean(state, late);
  lua_setglobal(state, "completing");
  lua_pushboolean(state, yielding);
  lua_setglobal(state, "yielding");
  lua_pushboolean(state, strstr(test->mode, "parser") != NULL);
  lua_setglobal(state, "parsing");
  lua_pushboolean(state, strstr(test->mode, "catch") != NULL &&
                             strstr(test->mode, "xpcall") == NULL);
  lua_setglobal(state, "catching");
  lua_pushboolean(state, strstr(test->mode, "xpcall") != NULL);
  lua_setglobal(state, "xcatching");
  lua_pushboolean(state, strstr(test->mode, "cleanup_error") != NULL);
  lua_setglobal(state, "cleanup_fail");
  execute(state, "pending='--'..string.rep(' ',8192)..'\\nreturn 42'; "
                 "assert(assert(load(pending))()==42)");
  create_tasks(test);
  {
    LuaTaskRoot empty = {0};
    require(luaTK_requeststop(&empty) == LUA_TK_INVALID);
  }
  if (prestart) {
    require(luaTK_requeststop(&test->roots[0]) == LUA_OK);
    collect_peer(test);
    gc(test);
    require(test->finalized == 0);
  }
  test->armed = !native && !prestart && !yielding && !late;
  require(pthread_create(&actor_thread, NULL, run_actor, &actor) == 0);
  if (!prestart) {
    lock(test);
    while (yielding ? !test->yielded : !test->entered)
      waitfor(test);
    unlock(test);
    collect_peer(test);
    if (denial)
      test->deny = 1;
    require(luaTK_requeststop(&test->roots[0]) == LUA_OK);
    require(luaTK_requeststop(&test->roots[0]) == LUA_OK);
    if (strstr(test->mode, "request_race")) {
      pthread_t requests[4];
      for (int i = 0; i < 4; ++i)
        require(pthread_create(&requests[i], NULL, request, test) == 0);
      for (int i = 0; i < 4; ++i)
        require(pthread_join(requests[i], NULL) == 0);
    }
    if (strstr(test->mode, "callback") || strstr(test->mode, "foreign_vm") ||
        unprotected) {
      lock(test);
      test->callback_request = 1;
      notify(test);
      while (!test->callback_done)
        waitfor(test);
      unlock(test);
      require(
          __atomic_load_n(&test->roots[0].stop_delivered, __ATOMIC_ACQUIRE) ==
          (strstr(test->mode, "foreign_vm") == NULL && !unprotected));
      lua_getglobal(state, "callback_ran");
      require(unprotected ? lua_toboolean(state, -1) : lua_isnil(state, -1));
      lua_pop(state, 1);
    } else
      require(
          !__atomic_load_n(&test->roots[0].stop_delivered, __ATOMIC_ACQUIRE));
    {
      int status = -99, results = -99;
      require(luaTK_collect(state, &test->roots[0], &status, &results) ==
              LUA_TK_PENDING);
      require(status == -99 && results == -99);
    }
    if (denial) {
      require(test->failures == 0);
      test->deny = 0;
    }
    gc(test);
    require((late || test->finalized == 0) && test->closed == 0);
    if (denial)
      test->deny = 1;
    lock(test);
    require(!test->returned);
    test->release = test->resume = 1;
    notify(test);
    unlock(test);
  }
  lock(test);
  while (!test->returned)
    waitfor(test);
  unlock(test);
  require(test->status[0] == (late ? LUA_OK : LUA_ERRRUN) &&
          test->results[0] >= 1);
  require(__atomic_load_n(&test->roots[0].stop_delivered, __ATOMIC_ACQUIRE) ==
          !late);
  if (denial) {
    require(test->failures == 0);
    test->deny = 0;
  }
  require(luaTK_requeststop(&test->roots[0]) == LUA_TK_INVALID);
  lock(test);
  test->finish = 1;
  notify(test);
  unlock(test);
  if (strstr(test->mode, "cleanup_wait")) {
    int status = -99, results = -99;
    lock(test);
    while (!test->cleanup_entered)
      waitfor(test);
    unlock(test);
    require(luaTK_requeststop(&test->roots[0]) == LUA_TK_INVALID);
    require(luaTK_collect(state, &test->roots[0], &status, &results) ==
            LUA_TK_PENDING);
    require(status == -99 && results == -99);
    gc(test);
    require(test->finalized == 0);
    lock(test);
    test->cleanup_release = 1;
    notify(test);
    unlock(test);
  }
  require(pthread_join(actor_thread, NULL) == 0);
  require(luaTK_requeststop(&test->roots[0]) == LUA_TK_INVALID);
  require(test->started == (prestart ? 0u : 1u) &&
          test->closed == (prestart || late || continuation ? 0u : 1u));
  require(test->handlers == 0);
  if (!prestart && !late && !continuation) {
    lua_getglobal(state, "cleaned_loop");
    require(lua_tointeger(state, -1) == 8002000);
    lua_pop(state, 1);
  }
  {
    int status = -99, results = -99;
    const char *message;
    require(luaTK_collect(state, &test->roots[0], &status, &results) == LUA_OK);
    require(status == (late ? LUA_OK : LUA_ERRRUN) && results == 1);
    if (late)
      require(lua_tointeger(state, -1) == 42);
    else {
      message = lua_tostring(state, -1);
      require(message != NULL);
      if (strstr(test->mode, "cleanup_error"))
        require(strstr(message, "cleanup expected") != NULL);
      else
        require(strcmp(message, "task stopped") == 0);
    }
    lua_pop(state, 1);
  }
  require(luaTK_requeststop(&test->roots[0]) == LUA_TK_INVALID);
  gc(test);
  require(test->finalized == 1 && G(state)->tasks == NULL);
  execute(state, "assert(6*7==42)");
  if (test->runtime)
    luthrum_lua_runtime_free(test->runtime);
  else
    lua_close(state);
  require(test->live == 0);
  require(pthread_cond_destroy(&test->changed) == 0);
  require(pthread_mutex_destroy(&test->mutex) == 0);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *test = calloc(1, sizeof(*test));
  pthread_t worker;
  require(test != NULL && argc == 2);
  active = test;
  test->mode = argv[1];
  require(pthread_create(&worker, NULL, run, test) == 0);
  require(pthread_join(worker, NULL) == 0);
  free(test);
  return 0;
}
