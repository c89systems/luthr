#define _POSIX_C_SOURCE 200809L
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <time.h>

static int monotonic(lua_State *state) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return luaL_error(state, "monotonic clock unavailable");
  lua_pushinteger(state, (lua_Integer)now.tv_sec * 1000000000 + now.tv_nsec);
  return 1;
}

static int callback(lua_State *state) {
  luaL_checktype(state, 1, LUA_TFUNCTION);
  lua_settop(state, 2);
  lua_pushvalue(state, 1);
  lua_pushvalue(state, 2);
  lua_call(state, 1, 1);
  return 1;
}

static int call(lua_State *state) {
  lua_Integer wanted = luaL_checkinteger(state, 1);
  luaL_argcheck(state, wanted >= LUA_MULTRET && wanted <= 4, 1,
                "result count out of range");
  lua_remove(state, 1);
  lua_call(state, lua_gettop(state) - 1, (int)wanted);
  return lua_gettop(state);
}

static int frame(lua_State *state) {
  lua_Integer growth = luaL_checkinteger(state, 3);
  luaL_checktype(state, 2, LUA_TFUNCTION);
  luaL_argcheck(state, growth >= 0 && growth <= INT_MAX, 3,
                "invalid stack growth");
  lua_toclose(state, 1);
  luaL_checkstack(state, (int)growth, "native frame");
  lua_pushvalue(state, 2);
  lua_call(state, 0, LUA_MULTRET);
  return lua_gettop(state) - 3;
}

static int reference(lua_State *state) {
  int ref;
  lua_settop(state, 1);
  lua_pushvalue(state, 1);
  ref = luaL_ref(state, LUA_REGISTRYINDEX);
  if (ref == LUA_REFNIL)
    lua_pushnil(state);
  else
    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
  luaL_unref(state, LUA_REGISTRYINDEX, ref);
  return 1;
}

static int rebox(lua_State *state) {
  lua_CFunction function = lua_tocfunction(state, 1);
  luaL_argcheck(state, function != NULL, 1, "C function expected");
  lua_pushcfunction(state, function);
  return 1;
}

static int new_userdata(lua_State *state) {
  luaL_checktype(state, 1, LUA_TTABLE);
  lua_newuserdatauv(state, 1, 0);
  lua_pushvalue(state, 1);
  lua_setmetatable(state, -2);
  return 1;
}

static int test_userdata(lua_State *state) {
  const char *name = luaL_checkstring(state, 2);
  int top = lua_gettop(state);
  void *expected = lua_touserdata(state, 1);
  void *value = lua_toboolean(state, 3) ? luaL_checkudata(state, 1, name)
                                        : luaL_testudata(state, -top, name);
  if (lua_gettop(state) != top || (value != NULL && value != expected))
    return luaL_error(state, "invalid userdata check result");
  lua_pushboolean(state, value != NULL);
  return 1;
}

static int finalizer_payload(lua_State *state) {
  lua_Integer identifier = luaL_checkinteger(state, 1);
  lua_Integer *payload = lua_newuserdatauv(state, 1200, 1);
  *payload = identifier;
  return 1;
}

static int bind_finalizer(lua_State *state) {
  const char *name = luaL_checkstring(state, 2);
  luaL_checktype(state, 1, LUA_TUSERDATA);
  lua_settop(state, 1);
  luaL_setmetatable(state, name);
  return 0;
}

static int finalizer_id(lua_State *state) {
  const lua_Integer *payload =
      luaL_checkudata(state, 1, luaL_checkstring(state, 2));
  luaL_argcheck(state, lua_rawlen(state, 1) >= sizeof(*payload), 1,
                "missing finalizer payload");
  lua_pushinteger(state, *payload);
  return 1;
}

static unsigned finalizer_barrier;

static int block_finalizer(lua_State *state) {
  const struct timespec delay = {0, 1000000};
  (void)state;
  __atomic_store_n(&finalizer_barrier, 1, __ATOMIC_RELEASE);
  while (__atomic_load_n(&finalizer_barrier, __ATOMIC_ACQUIRE) == 1)
    nanosleep(&delay, NULL);
  __atomic_store_n(&finalizer_barrier, 0, __ATOMIC_RELEASE);
  return 0;
}

static int finalizer_waiting(lua_State *state) {
  unsigned phase = __atomic_load_n(&finalizer_barrier, __ATOMIC_ACQUIRE);
  if (lua_toboolean(state, 1)) {
    luaL_argcheck(state, phase == 1, 1, "finalizer must be blocked");
    __atomic_store_n(&finalizer_barrier, 2, __ATOMIC_RELEASE);
  }
  lua_pushboolean(state, phase == 1);
  return 1;
}

static int pump(lua_State *state) {
  lua_Integer capacity = luaL_checkinteger(state, 3);
  lua_Integer total = 0;
  luaL_checktype(state, 1, LUA_TFUNCTION);
  luaL_checktype(state, 2, LUA_TFUNCTION);
  luaL_argcheck(state, capacity > 0 && capacity <= 1024, 3,
                "capacity must be between 1 and 1024");
  lua_settop(state, 3);
  for (;;) {
    size_t size;
    lua_pushvalue(state, 1);
    lua_pushinteger(state, capacity);
    lua_call(state, 1, 1);
    if (lua_isnil(state, -1))
      break;
    luaL_checktype(state, -1, LUA_TSTRING);
    (void)lua_tolstring(state, -1, &size);
    if (size == 0)
      break;
    if (size > (size_t)capacity)
      return luaL_error(state, "reader exceeded capacity");
    lua_pushvalue(state, 2);
    lua_pushvalue(state, -2);
    lua_call(state, 1, 0);
    total += (lua_Integer)size;
    lua_pop(state, 1);
  }
  lua_pushinteger(state, total);
  return 1;
}

int luaopen_compat_fixture(lua_State *state);
int luaopen_compat_fixture(lua_State *state) {
  static const luaL_Reg functions[] = {{"callback", callback},
                                       {"call", call},
                                       {"reference", reference},
                                       {"rebox", rebox},
                                       {"pump", pump},
                                       {"monotonic", monotonic},
                                       {"frame", frame},
                                       {"new_userdata", new_userdata},
                                       {"test_userdata", test_userdata},
                                       {"finalizer_payload", finalizer_payload},
                                       {"bind_finalizer", bind_finalizer},
                                       {"finalizer_id", finalizer_id},
                                       {"block_finalizer", block_finalizer},
                                       {"finalizer_waiting", finalizer_waiting},
                                       {NULL, NULL}};
  luaL_newlib(state, functions);
  return 1;
}
