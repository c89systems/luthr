#include <lauxlib.h>
#include <lexec.h>
#include <lstate.h>
#include <lualib.h>
#include <pthread.h>
#include <sched.h>
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

static unsigned checks;
static unsigned entered, observed;
static __thread unsigned admissions;

void __real_luaW_enter(lua_State *state);
void __wrap_luaW_enter(lua_State *state) {
  admissions++;
  __real_luaW_enter(state);
}

struct invocation {
  lua_State *state;
  int status;
};

/* A batch in an attached activation must not transact on its physical gate.
 * Ordinary foreign bindings must continue to acquire it for their API calls. */
static void check_api(lua_State *state, int foreign) {
  unsigned before = admissions;
  int top = lua_gettop(state);
  unsigned i;
  for (i = 0; i < 128; i++) {
    lua_pushinteger(state, i);
    require(lua_tointeger(state, -1) == (lua_Integer)i);
    lua_pop(state, 1);
  }
  require(lua_gettop(state) == top);
  if (foreign)
    require(admissions > before);
  else
    require(admissions == before);
  checks++;
}

static int owned(lua_State *state) {
  check_api(state, 0);
  return 0;
}

static int foreign(lua_State *state) {
  check_api(state, 1);
  return 0;
}

static int failing(lua_State *state) {
  check_api(state, 0);
  return luaL_error(state, "expected region failure");
}

static int nested(lua_State *state) {
  check_api(state, 0);
  lua_pushcfunction(state, foreign);
  lua_call(state, 0, 0);
  check_api(state, 0);
  luaQ_pushmanaged(state, failing, 0);
  require(lua_pcall(state, 0, 0, 0) == LUA_ERRRUN);
  require(strstr(lua_tostring(state, -1), "expected region failure") != NULL);
  lua_pop(state, 1);
  check_api(state, 0);
  lua_gc(state, LUA_GCCOLLECT);
  check_api(state, 0);
  return 0;
}

static int continued(lua_State *state, int status, lua_KContext context) {
  require(status == LUA_YIELD && context == 17);
  check_api(state, 0);
  lua_pushinteger(state, 42);
  return 1;
}

static int yielding(lua_State *state) {
  check_api(state, 0);
  lua_pushinteger(state, 7);
  return lua_yieldk(state, 1, 17, continued);
}

static int handoff(lua_State *state) {
  lua_pushliteral(state, "retained across collection");
  __atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&observed, __ATOMIC_ACQUIRE)) {
    lua_pushinteger(state, 42);
    lua_pop(state, 1);
  }
  require(strcmp(lua_tostring(state, -1), "retained across collection") == 0);
  lua_pop(state, 1);
  checks++;
  return 0;
}

static void *invoke(void *context) {
  struct invocation *call = context;
  call->status = luaL_dostring(call->state, "probe()");
  require(luaE_getnativestackcontext()->api_region == NULL);
  return NULL;
}

int main(int argc, char **argv) {
  lua_State *state = luaL_newstate();
  const char *source;
  lua_CFunction function;
  int is_foreign = 0;
  int is_handoff = 0;
  int is_collection = 0;
  require(state != NULL && argc == 2);
  luaL_openlibs(state);
  if (strstr(argv[1], "generational") != NULL)
    lua_gc(state, LUA_GCGEN);
  if (strncmp(argv[1], "managed_", 8) == 0) {
    function = owned;
    source = "probe(); probe(); collectgarbage('collect'); probe()";
  } else if (strncmp(argv[1], "foreign_", 8) == 0) {
    function = foreign;
    is_foreign = 1;
    source = "probe(); probe(); collectgarbage('collect'); probe()";
  } else if (strncmp(argv[1], "nested_", 7) == 0) {
    function = nested;
    source = "probe(); probe()";
  } else if (strncmp(argv[1], "handoff_", 8) == 0 ||
             strncmp(argv[1], "collection_", 11) == 0) {
    function = handoff;
    is_handoff = 1;
    is_collection = strncmp(argv[1], "collection_", 11) == 0;
    source = "probe()";
  } else {
    require(strncmp(argv[1], "yield_", 6) == 0);
    function = yielding;
    source = "local co=coroutine.create(function() return probe() end); "
             "local ok,value=coroutine.resume(co); assert(ok and value==7); "
             "collectgarbage('collect'); "
             "ok,value=coroutine.resume(co); assert(ok and value==42); "
             "assert(coroutine.status(co)=='dead')";
  }
  if (is_foreign)
    lua_pushcfunction(state, function);
  else
    luaQ_pushmanaged(state, function, 0);
  lua_setglobal(state, "probe");
  if (is_handoff) {
    pthread_t worker;
    struct invocation call = {is_collection ? lua_newthread(state) : state, -1};
    require(pthread_create(&worker, NULL, invoke, &call) == 0);
    while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE))
      sched_yield();
    if (is_collection)
      lua_gc(state, LUA_GCCOLLECT);
    else
      require(lua_gettop(state) >= 0);
    __atomic_store_n(&observed, 1, __ATOMIC_RELEASE);
    require(pthread_join(worker, NULL) == 0 && call.status == LUA_OK);
  } else
    require(luaL_dostring(state, source) == LUA_OK);
  require(checks != 0 && state->native_depth == 0);
  require(luaE_getnativestackcontext()->api_region == NULL);
  lua_close(state);
  return 0;
}
