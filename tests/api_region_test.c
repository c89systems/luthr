#include <lauxlib.h>
#include <ldo.h>
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

/* An attached activation borrows its existing admission. A foreign call
 * still needs admission to read shared pseudo-indices. */
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
  if (foreign) {
    require(lua_type(state, LUA_REGISTRYINDEX) == LUA_TTABLE);
    require(admissions > before);
  } else
    require(admissions == before);
  checks++;
}

static int owned(lua_State *state) {
  check_api(state, 0);
  return 0;
}

static int foreign_without_metadata(lua_State *state) {
  require(luaE_getnativestackcontext()->api_region == NULL);
  check_api(state, 1);
  lua_pushinteger(state, 42);
  return 1;
}

static int owned_without_metadata(lua_State *state) {
  LuaWorldScope scope = luaW_scope(state);
  require(scope.heap == 0 && scope.collection == 0);
  check_api(state, 0);
  lua_pushinteger(state, 42);
  return 1;
}

static void metadata(lua_State *state, int foreign) {
  unsigned stops;
  for (stops = 0; stops <= 2; stops += 2) {
    LuaWorldScope restored;
    if (foreign)
      lua_pushcfunction(state, foreign_without_metadata);
    else
      luaQ_pushmanaged(state, owned_without_metadata, 0);
    luaX_lockstate(state);
    luaW_heapenter(state);
    luaW_heapenter(state);
    if (stops != 0) {
      luaW_stop(state);
      luaW_stop(state);
    }
    luaD_call(state, state->top.p - 1, 1);
    restored = luaW_scope(state);
    require(restored.heap == 2 && restored.collection == stops);
    if (stops != 0) {
      luaW_start(state);
      luaW_start(state);
    }
    luaW_heapleave(state);
    luaW_heapleave(state);
    luaX_unlockstate(state);
    require(lua_tointeger(state, -1) == 42);
    lua_pop(state, 1);
  }
}

static int foreign(lua_State *state) {
  check_api(state, 1);
  if (lua_gettop(state) != 0) {
    unsigned ticks = (unsigned)lua_tointeger(state, 3);
    int i;
    for (i = 0; i < 3; i++) {
      unsigned before;
      if (i == 1) {
        /* The warm call fits once before the next required poll. */
        luaX_lockstate(state);
        state->poll_remaining = 2 * ticks;
        luaX_unlockstate(state);
      }
      lua_pushvalue(state, 1);
      lua_pushinteger(state, 41);
      lua_pushinteger(state, 1);
      before = admissions;
      lua_call(state, 2, 1);
      if (i != 0)
        require(admissions == before + (i == 2));
      require(lua_rawequal(state, 2, -1));
      require(lua_isinteger(state, -1) == lua_isinteger(state, 2));
      lua_pop(state, 1);
    }
  }
  return 0;
}

static void request_slot(LuaExecution *execution, lua_State *state) {
  unsigned *requests = execution->context;
  (void)state;
  (*requests)++;
}

static void native_values(lua_State *state) {
  unsigned requests = 0;
  LuaExecution execution = {
      .context = &requests, .managed_enter = request_slot, .native_depth = 1};
  NativeStackContext context = {.execution = &execution,
                                .limit = luaE_initnativestack()};
  NativeStackContext *previous;
  const char *string;
  size_t length;
  lua_gc(state, LUA_GCSTOP);
  lua_pushliteral(state, "rooted across a native entry");
  previous = luaE_setnativestackcontext(&context);
  check_api(state, 0);
  lua_pushvalue(state, 1);
  string = lua_tolstring(state, -1, &length);
  require(length == strlen("rooted across a native entry") &&
          strcmp(string, "rooted across a native entry") == 0);
  lua_pop(state, 1);
  require(requests == 0);
  require(lua_type(state, LUA_REGISTRYINDEX) == LUA_TTABLE && requests == 1);
  lua_pushinteger(state, 1234);
  require(strcmp(lua_tostring(state, -1), "1234") == 0 && requests == 2);
  lua_pop(state, 1);
  require(requests == 2 && execution.managed_depth == 0);
  require(luaL_dostring(state,
                        "probe(function(x) return x end,41,1); "
                        "probe(function(x) return x+1 end,42,2); "
                        "probe(function(x) return x-1000 end,-959,2); "
                        "probe(function(x,y) return x+y end,42,2); "
                        "probe(function(x,y) return x*y end,41,2); "
                        "probe(function() return 42 end,42,2); "
                        "probe(function() return 42.0 end,42.0,2); "
                        "probe(function() return 'literal' end,'literal',2); "
                        "probe(function() return true end,true,2); "
                        "probe(function() return false end,false,2); "
                        "probe(function() return nil end,nil,2); "
                        "probe(function() end,nil,1)") == LUA_OK);
  require(execution.managed_depth == 0);
  require(luaE_setnativestackcontext(previous) == &context);
  lua_gc(state, LUA_GCCOLLECT);
  require(strcmp(string, "rooted across a native entry") == 0);
  lua_pop(state, 1);
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
  if (strncmp(argv[1], "metadata_", 9) == 0) {
    metadata(state, 0);
    metadata(state, 1);
    require(checks == 4 && state->native_depth == 0);
    require(luaE_getnativestackcontext()->api_region == NULL);
    lua_close(state);
    return 0;
  }
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
  if (is_foreign)
    native_values(state);
  require(luaE_getnativestackcontext()->api_region == NULL);
  lua_close(state);
  return 0;
}
