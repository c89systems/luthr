#include <luthrum/lua.h>
#include <luthrum/lua_runtime.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  luthrum_lua_state *state;
  luthrum_lua_integer value;
  luthrum_lua_runtime *runtime;
  const unsigned char source[] = "assert(6*7==42)";
  state = luthrum_lua_l_newstate();
  if (state == NULL)
    return 1;
  luthrum_lua_pushinteger(state,
                          luthrum_lua_integer_make(0x12345678U, 0x9abcdef0U));
  value = luthrum_lua_tointeger(state, -1);
  if (luthrum_lua_integer_high(value) != 0x12345678U ||
      luthrum_lua_integer_low(value) != 0x9abcdef0U)
    return 2;
  luthrum_lua_close(state);
  if (argc == 2) {
    state = luthrum_lua_l_newstate();
    if (state == NULL)
      return 5;
    luthrum_lua_l_openlibs(state);
    luthrum_lua_pushstring(state, argv[1]);
    luthrum_lua_setglobal(state, "module_dir");
    if (luthrum_lua_l_dostring(
            state, "package.cpath=module_dir..'/?.so'; "
                   "local m=require('native_fixture'); "
                   "assert(m.callback(function(n) return n+1 end)==43)") !=
        LUTHRUM_LUA_OK) {
      fprintf(stderr, "%s\n", luthrum_lua_tostring(state, -1));
      return 6;
    }
    luthrum_lua_close(state);
  }
  if (luthrum_lua_runtime_new(&runtime) != LUTHRUM_LUA_RUNTIME_OK)
    return 3;
  if (luthrum_lua_runtime_run_buffer(
          runtime, source, sizeof(source) - 1, "installed-consumer", 0, NULL,
          LUTHRUM_LUA_RUNTIME_OPEN_LIBS) != LUTHRUM_LUA_RUNTIME_OK) {
    fprintf(stderr, "%s\n", luthrum_lua_runtime_error(runtime));
    return 4;
  }
  luthrum_lua_runtime_free(runtime);
  puts("installed C89 consumer: passed");
  return 0;
}
