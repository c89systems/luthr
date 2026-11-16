#include <errno.h>
#include <lauxlib.h>
#include <lcallback.h>
#include <lstate.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
  lua_State *state;
  LuaCallback callbacks[3];
  unsigned allocations, fail_at, remaining, failures, finalized, handlers;
  size_t live;
  const char *mode;
};
static struct fixture *active;
static int sync_failure;

#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)
int __real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int __wrap_pthread_mutex_init(pthread_mutex_t *mutex,
                              const pthread_mutexattr_t *attributes) {
  if (sync_failure == 1) {
    sync_failure = 0;
    return ENOMEM;
  }
  return __real_pthread_mutex_init(mutex, attributes);
}
int __wrap_pthread_cond_init(pthread_cond_t *condition,
                             const pthread_condattr_t *attributes) {
  if (sync_failure == 2) {
    sync_failure = 0;
    return ENOMEM;
  }
  return __real_pthread_cond_init(condition, attributes);
}
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *fixture = context;
  void *pointer;
  if (old == NULL)
    old_size = 0;
  if (size == 0) {
    fixture->live -= old_size;
    free(old);
    return NULL;
  }
  fixture->allocations++;
  if (size > old_size && fixture->remaining != 0 &&
      fixture->allocations >= fixture->fail_at) {
    fixture->remaining--;
    fixture->failures++;
    return NULL;
  }
  pointer = malloc(size);
  if (pointer != NULL) {
    if (old != NULL) {
      memcpy(pointer, old, old_size < size ? old_size : size);
      free(old);
    }
    fixture->live = fixture->live - old_size + size;
  }
  return pointer;
}
static int finalize(lua_State *state) {
  require(lua_touserdata(state, 1) != NULL);
  active->finalized++;
  return 0;
}
static void marshal(lua_State *state, void *context) {
  int *value = context;
  if (strcmp(active->mode, "invoke_oom") == 0 && active->remaining != 0) {
    (void)lua_newuserdatauv(state, 8192, 0);
    lua_pop(state, 1);
  }
  lua_pushinteger(state, *value);
  lua_call(state, 1, 1);
  *value = (int)lua_tointeger(state, -1);
}
static int self_revoke(lua_State *state) {
  luaCB_revoke(&active->callbacks[0]);
  require(luaCB_destroy(state, &active->callbacks[0]) == LUA_CB_PENDING);
  lua_pushinteger(state, 88);
  return 1;
}
static int self_close(lua_State *state) {
  return lua_closethread(state, state);
}
static int close_registered(lua_State *state) {
  active->fail_at = active->allocations + 1;
  active->remaining = 2;
  require(lua_closethread(state, state) == LUA_ERRRUN);
  require(active->remaining == 2); /* Rejection requires no Lua allocation. */
  active->remaining = 0;
  require(strstr(lua_tostring(state, -1), "active registered callback") !=
          NULL);
  lua_pop(state, 1);
  lua_pushinteger(state, 99);
  return 1;
}
static int recursive_callback(lua_State *state) {
  char error[80];
  int value = 0;
  require(luaCB_call(&active->callbacks[0], NULL, marshal, &value, error,
                     sizeof(error)) == LUA_OK);
  require(value == 42);
  lua_pushinteger(state, value);
  return 1;
}
static int overflow_handler(lua_State *state) {
  LuaExecution execution;
  char error[256];
  int value = 0;
  memset(&execution, 0, sizeof(execution));
  require(luaCB_call(&active->callbacks[0], &execution, marshal, &value, error,
                     sizeof(error)) == LUA_OK);
  require(value == 42);
  active->handlers++;
  lua_pushvalue(state, 1);
  return 1;
}
static void closure(struct fixture *fixture) {
  static const char source[] =
      "local token=...; return function(id) local retained=token; "
      "if id==1 then error('callback expected') end; "
      "if id==2 then error({}) end; "
      "if id==3 then coroutine.yield() end; "
      "if id==4 then return revoke_self() end; "
      "if id==5 then local co=coroutine.create(function() self_close() end); "
      "assert(coroutine.resume(co)) end; "
      "if id==6 then return close_registered() end; "
      "if id==7 then return recursive_callback() end; "
      "if id==8 then local value <close> =setmetatable({}, "
      "{__close=function() error('close expected') end}); return 0 end; "
      "if id==9 then local function f() return string.gsub('x','x',f) end; "
      "return f() end; return id+42 end";
  lua_State *state = fixture->state;
  (void)lua_newuserdatauv(state, 1, 0);
  lua_newtable(state);
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  lua_setmetatable(state, -2);
  require(luaL_loadstring(state, source) == LUA_OK);
  lua_pushvalue(state, -2);
  require(lua_pcall(state, 1, 1, 0) == LUA_OK);
  lua_remove(state, -2);
}
static void call(struct fixture *fixture, int argument, int expected,
                 const char *message) {
  LuaExecution execution;
  char error[256];
  int result = argument;
  memset(&execution, 0, sizeof(execution));
  require(luaCB_call(&fixture->callbacks[0], &execution, marshal, &result,
                     error, sizeof(error)) == expected);
  if (message != NULL) {
    if (strstr(error, message) == NULL)
      fprintf(stderr, "argument=%d expected=%s actual=%s\n", argument, message,
              error);
    require(strstr(error, message) != NULL);
  }
  if (expected == LUA_OK && argument != 4 && argument != 6 && argument != 7)
    require(result == argument + 42);
  if (expected == LUA_OK && argument == 4)
    require(result == 88);
  if (expected == LUA_OK && argument == 6)
    require(result == 99);
  if (expected == LUA_OK && argument == 7)
    require(result == 42);
  require(execution.native_depth == 0 && execution.managed_depth == 0 &&
          execution.protected == NULL && luaQ_current() == NULL);
  require(fixture->callbacks[0].thread->luthr_stack_context == NULL);
  require(fixture->callbacks[0].entries == 0 &&
          fixture->callbacks[0].depth == 0);
}
static void *run(void *context) {
  struct fixture *fixture = context;
  lua_State *state;
  unsigned i;
  fixture->state = state = lua_newstate(allocate, fixture, 0);
  require(state != NULL);
  luaL_openlibs(state);
  lua_pushcfunction(state, self_revoke);
  lua_setglobal(state, "revoke_self");
  lua_pushcfunction(state, self_close);
  lua_setglobal(state, "self_close");
  lua_pushcfunction(state, close_registered);
  lua_setglobal(state, "close_registered");
  lua_pushcfunction(state, recursive_callback);
  lua_setglobal(state, "recursive_callback");
  lua_pushcfunction(state, overflow_handler);
  lua_setglobal(state, "overflow_handler");
  lua_pushnil(state);
  require(luaCB_init(state, &fixture->callbacks[0], 0) == LUA_CB_INVALID);
  lua_pop(state, 1);
  closure(fixture);
  if (strstr(fixture->mode, "generational") != NULL)
    (void)lua_gc(state, LUA_GCGEN);
  if (strcmp(fixture->mode, "mutex") == 0 ||
      strcmp(fixture->mode, "condition") == 0) {
    sync_failure = strcmp(fixture->mode, "mutex") == 0 ? 1 : 2;
    require(luaCB_init(state, &fixture->callbacks[0], 0) == LUA_ERRMEM);
    require(sync_failure == 0 && G(state)->callbacks == NULL);
  } else if (strncmp(fixture->mode, "create_oom", 10) == 0) {
    int status;
    fixture->fail_at = fixture->allocations +
                       (strcmp(fixture->mode, "create_oom_message") == 0 ? 3U
                        : strcmp(fixture->mode, "create_oom_stack") == 0 ? 2U
                                                                         : 1U);
    fixture->remaining = 2;
    status = luaCB_init(state, &fixture->callbacks[0], 0);
    if (status == LUA_OK) {
      /* The fixed rejection string can reuse a locally allocated page. */
      require(strcmp(fixture->mode, "create_oom_message") == 0);
      require(fixture->failures == 0 && fixture->remaining == 2);
      luaCB_revoke(&fixture->callbacks[0]);
      require(luaCB_destroy(state, &fixture->callbacks[0]) == LUA_OK);
      fixture->remaining = 0;
    } else {
      require(status == LUA_ERRMEM);
      require(fixture->failures == 2 && fixture->remaining == 0);
    }
    require(G(state)->callbacks == NULL);
  }
  require(luaCB_init(state, &fixture->callbacks[0], 0) == LUA_OK);
  require(fixture->callbacks[0].limit == 1);
  require(luaCB_destroy(state, &fixture->callbacks[0]) == LUA_CB_INVALID);
  {
    lua_State *other = luaL_newstate();
    require(other != NULL);
    require(luaCB_destroy(other, &fixture->callbacks[0]) == LUA_CB_INVALID);
    lua_close(other);
  }
  if (strncmp(fixture->mode, "roots_", 6) == 0) {
    for (i = 1; i < 3; ++i)
      require(luaCB_init(state, &fixture->callbacks[i], 0) == LUA_OK);
    lua_pop(state, 1);
    (void)lua_gc(state, LUA_GCCOLLECT);
    require(fixture->finalized == 0);
    luaCB_revoke(&fixture->callbacks[1]);
    require(luaCB_destroy(state, &fixture->callbacks[1]) == LUA_OK);
    luaCB_revoke(&fixture->callbacks[2]);
    require(luaCB_destroy(state, &fixture->callbacks[2]) == LUA_OK);
    (void)lua_gc(state, LUA_GCCOLLECT);
    require(fixture->finalized == 0);
  } else {
    lua_pop(state, 1);
    if (strcmp(fixture->mode, "invoke_oom") == 0) {
      fixture->fail_at = fixture->allocations + 1;
      fixture->remaining = ~0U;
      call(fixture, 0, LUA_ERRMEM, "memory");
      require(fixture->failures >= 2);
      fixture->remaining = 0;
    }
    call(fixture, 0, LUA_OK, NULL);
    call(fixture, 1, LUA_ERRRUN, "callback expected");
    call(fixture, 2, LUA_ERRRUN, "non-string");
    call(fixture, 3, LUA_ERRRUN, "yield");
    call(fixture, 5, LUA_OK, NULL);
    call(fixture, 6, LUA_OK, NULL);
    call(fixture, 7, LUA_OK, NULL);
    call(fixture, 8, LUA_ERRRUN, "close expected");
    call(fixture, 9, LUA_ERRRUN, "stack overflow");
    call(fixture, 0, LUA_OK, NULL);
    if (strcmp(fixture->mode, "reserve") == 0) {
      require(
          luaL_dostring(
              state,
              "local function f() return string.gsub('x','x',f) end; "
              "local ok,e=xpcall(f,overflow_handler); "
              "assert(not ok and tostring(e):find('stack overflow',1,true))") ==
          LUA_OK);
      require(fixture->handlers != 0);
    }
    {
      LuaExecution execution;
      int value = 1;
      char error[4] = {1, 1, 1, 1};
      memset(&execution, 0, sizeof(execution));
      require(luaCB_call(&fixture->callbacks[0], NULL, marshal, &value, error,
                         sizeof(error)) == LUA_CB_INVALID);
      require(luaCB_call(&fixture->callbacks[0], &execution, NULL, &value,
                         error, sizeof(error)) == LUA_CB_INVALID);
      require(luaCB_call(&fixture->callbacks[0], &execution, marshal, &value,
                         NULL, 1) == LUA_CB_INVALID);
      execution.native_depth = 1;
      require(luaCB_call(&fixture->callbacks[0], &execution, marshal, &value,
                         error, sizeof(error)) == LUA_CB_INVALID);
      execution.native_depth = 0;
      execution.pin_depth = 1;
      require(luaCB_call(&fixture->callbacks[0], &execution, marshal, &value,
                         error, sizeof(error)) == LUA_CB_INVALID);
      execution.pin_depth = 0;
      require(luaCB_call(&fixture->callbacks[0], &execution, marshal, &value,
                         error, sizeof(error)) == LUA_ERRRUN);
      require(error[0] != '\0' && error[3] == '\0');
    }
    call(fixture, 4, LUA_OK, NULL);
    call(fixture, 0, LUA_CB_REVOKED, NULL);
  }
  luaCB_revoke(&fixture->callbacks[0]);
  require(luaCB_destroy(state, &fixture->callbacks[0]) == LUA_OK);
  require(G(state)->callbacks == NULL);
  (void)lua_gc(state, LUA_GCCOLLECT);
  require(fixture->finalized == 1);
  lua_close(state);
  require(fixture->live == 0);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *fixture = calloc(1, sizeof(*fixture));
  pthread_t worker;
  require(fixture != NULL && argc == 2);
  fixture->mode = argv[1];
  active = fixture;
  require(pthread_create(&worker, NULL, run, fixture) == 0);
  require(pthread_join(worker, NULL) == 0);
  free(fixture);
  return 0;
}
