#include <luthrum/lua.h>
#include <luthrum/lua_runtime.h>
#include <stdio.h>

static const char *module_lua_dir;
static const char *module_native_dir;
static const unsigned char setup[] =
    "package.path=module_lua_dir..'/?.lua;'..module_lua_dir..'/?/init.lua'; "
    "package.cpath=module_native_dir..'/?.so'";
static int configure_paths(void *pointer) {
  luthrum_lua_state *state = (luthrum_lua_state *)pointer;
  luthrum_lua_pushstring(state, module_lua_dir);
  luthrum_lua_setglobal(state, "module_lua_dir");
  luthrum_lua_pushstring(state, module_native_dir);
  luthrum_lua_setglobal(state, "module_native_dir");
  return 0;
}
int main(int argc, char **argv) {
  luthrum_lua_state *state;
  luthrum_lua_runtime *runtime;
  if (argc != 4)
    return 1;
  module_lua_dir = argv[1];
  module_native_dir = argv[2];
  state = luthrum_lua_l_newstate();
  if (state == NULL)
    return 2;
  luthrum_lua_l_openlibs(state);
  (void)configure_paths(state);
  if (luthrum_lua_l_dostring(state, (const char *)setup) != LUTHRUM_LUA_OK ||
      luthrum_lua_l_dofile(state, argv[3]) != LUTHRUM_LUA_OK) {
    fprintf(stderr, "installed native module: %s\n",
            luthrum_lua_tostring(state, -1));
    return 3;
  }
  luthrum_lua_close(state);
  if (luthrum_lua_runtime_new(&runtime) != LUTHRUM_LUA_RUNTIME_OK)
    return 4;
  if (luthrum_lua_runtime_openlibs(runtime) != LUTHRUM_LUA_RUNTIME_OK ||
      luthrum_lua_runtime_register_c_module(
          runtime, "paths", configure_paths) != LUTHRUM_LUA_RUNTIME_OK ||
      luthrum_lua_runtime_require(runtime, "paths") != LUTHRUM_LUA_RUNTIME_OK ||
      luthrum_lua_runtime_run_buffer(runtime, setup, sizeof(setup) - 1, "paths",
                                     0, NULL, 0) != LUTHRUM_LUA_RUNTIME_OK ||
      luthrum_lua_runtime_run_file(runtime, argv[3], 0, NULL, 0) !=
          LUTHRUM_LUA_RUNTIME_OK) {
    fprintf(stderr, "installed native module policy: %s\n",
            luthrum_lua_runtime_error(runtime));
    return 5;
  }
  luthrum_lua_runtime_free(runtime);
  puts("installed native module operations/callbacks/error/reuse: passed");
  return 0;
}
