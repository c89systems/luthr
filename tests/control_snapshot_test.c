#include <lauxlib.h>
#include <lstate.h>
#include <lualib.h>
#include <luthr/lua_runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *main, *caller;
  luthr_lua_runtime *runtime;
  int armed, entered, release, status, results, finalized;
  const char *mode;
};
static struct fixture *active;
#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)
lua_State *__real_lua_tothread(lua_State *, int);
lua_State *__wrap_lua_tothread(lua_State *state, int index) {
  struct fixture *test = active;
  lua_State *thread = __real_lua_tothread(state, index);
  require(pthread_mutex_lock(&test->mutex) == 0);
  if (test->armed && state == test->caller &&
      (index == 1 || index == lua_upvalueindex(1))) {
    test->armed = 0;
    test->entered = 1;
    require(thread != NULL && thread->native_depth == 0);
    require(state->native_depth != 0 && state->world_depth == 0);
    require(pthread_cond_broadcast(&test->changed) == 0);
    while (!test->release)
      require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
  }
  require(pthread_mutex_unlock(&test->mutex) == 0);
  return thread;
}
static int capture(void *context) {
  active->main = context;
  return 0;
}
static int finalize(lua_State *state) {
  unsigned *token = lua_touserdata(state, 1);
  require(token != NULL && *token == 12345);
  *token = 0;
  active->finalized++;
  return 0;
}
static void *invoke(void *context) {
  struct fixture *test = context;
  test->status = lua_resume(test->caller, NULL, 0, &test->results);
  return NULL;
}
static void *run(void *context) {
  struct fixture *test = context;
  lua_State *state, *replacement;
  pthread_t caller;
  unsigned *token;
  int facade = strstr(test->mode, "facade") != NULL;
  if (facade) {
    static const unsigned char source[] = "require('fixture')";
    require(luthr_lua_runtime_new(&test->runtime) == LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_openlibs(test->runtime) == LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_register_c_module(
                test->runtime, "fixture", capture) == LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_set_instruction_limit(test->runtime, 10000) ==
            LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_run_buffer(test->runtime, source,
                                         sizeof(source) - 1, "control-snapshot",
                                         0, NULL, 0) == LUTHR_LUA_RUNTIME_OK);
  } else {
    test->main = luaL_newstate();
    require(test->main != NULL);
    luaL_openlibs(test->main);
  }
  state = test->main;
  if (strstr(test->mode, "generational") != NULL)
    (void)lua_gc(state, LUA_GCGEN);
  token = lua_newuserdatauv(state, sizeof(*token), 0);
  *token = 12345;
  lua_newtable(state);
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  lua_setmetatable(state, -2);
  require(luaL_loadstring(
              state, "local token=...; wrapped=coroutine.wrap(function(value) "
                     "return token,value+42 end)") == LUA_OK);
  lua_pushvalue(state, -2);
  require(lua_pcall(state, 1, 0, 0) == LUA_OK);
  lua_settop(state, 0);
  test->caller = lua_newthread(state);
  require(luaL_loadstring(test->caller, "return wrapped(8)") == LUA_OK);
  test->armed = 1;
  require(pthread_create(&caller, NULL, invoke, test) == 0);
  require(pthread_mutex_lock(&test->mutex) == 0);
  while (!test->entered)
    require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
  require(pthread_mutex_unlock(&test->mutex) == 0);
  /* Replace the only original owner before the child's first API entry.
     Merely retaining it after resume starts would miss this lifetime gap. */
  lua_getglobal(state, "wrapped");
  replacement = lua_newthread(state);
  require(luaL_loadstring(replacement, "return -1") == LUA_OK);
  require(lua_setupvalue(state, -2, 1) != NULL);
  lua_pop(state, 1);
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  require(test->finalized == 0);
  require(pthread_mutex_lock(&test->mutex) == 0);
  test->release = 1;
  require(pthread_cond_broadcast(&test->changed) == 0);
  require(pthread_mutex_unlock(&test->mutex) == 0);
  require(pthread_join(caller, NULL) == 0);
  require(test->status == LUA_OK && test->results == 2);
  require(lua_touserdata(test->caller, -2) == token &&
          lua_tointeger(test->caller, -1) == 50 && *token == 12345);
  lua_settop(test->caller, 0);
  lua_pushnil(state);
  lua_setglobal(state, "wrapped");
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  require(test->finalized == 1);
  require(luaL_loadstring(
              state,
              "local f=coroutine.wrap(function() return 1 end); "
              "debug.setupvalue(f,1,{}); local ok=pcall(f); assert(not ok); "
              "assert(coroutine.wrap(function() return 42 end)()==42)") ==
          LUA_OK);
  require(lua_pcall(state, 0, 0, 0) == LUA_OK);
  if (facade)
    luthr_lua_runtime_free(test->runtime);
  else
    lua_close(state);
  require(test->finalized == 1);
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
