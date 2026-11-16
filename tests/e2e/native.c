#define _POSIX_C_SOURCE 200809L
#include <lauxlib.h>
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
                                       {"reference", reference},
                                       {"pump", pump},
                                       {"monotonic", monotonic},
                                       {NULL, NULL}};
  luaL_newlib(state, functions);
  return 1;
}
