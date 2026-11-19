#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>

static void *allocate(void *ud, void *ptr, size_t old_size, size_t size) {
  (void)ud;
  (void)old_size;
  if (size == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, size);
}

int main(int argc, char **argv) {
  lua_State *L;
  int i, status;
  if (argc < 2) {
    fputs("usage: benchmark_host SCRIPT [ARG ...]\n", stderr);
    return EXIT_FAILURE;
  }
  /* A different hash seed can turn every two-field row into a collision.
   * Both runtimes use the same public constructor and allocator here. */
  L = lua_newstate(allocate, NULL, 0);
  if (L == NULL) {
    fputs("benchmark_host: cannot create Lua state\n", stderr);
    return EXIT_FAILURE;
  }
  luaL_openlibs(L);
  lua_createtable(L, argc - 1, 0);
  for (i = 1; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - 1);
  }
  lua_setglobal(L, "arg");
  status = luaL_loadfile(L, argv[1]);
  if (status == LUA_OK)
    status = lua_pcall(L, 0, 0, 0);
  if (status != LUA_OK) {
    const char *error = lua_tostring(L, -1);
    fprintf(stderr, "benchmark_host: %s\n", error ? error : "Lua error");
  }
  lua_close(L);
  return status == LUA_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
