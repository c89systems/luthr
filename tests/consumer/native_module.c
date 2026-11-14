#include <lauxlib.h>
#include <lua.h>

static int callback(lua_State *state) {
  luaL_checktype(state, 1, LUA_TFUNCTION);
  lua_pushvalue(state, 1);
  lua_pushinteger(state, 42);
  lua_call(state, 1, 1);
  return 1;
}

int luaopen_native_fixture(lua_State *state);
int luaopen_native_fixture(lua_State *state) {
  static const luaL_Reg functions[] = {{"callback", callback}, {NULL, NULL}};
  luaL_newlib(state, functions);
  return 1;
}
