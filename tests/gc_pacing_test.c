#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
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

enum step_mode { AUTOMATIC, EXPLICIT, FULL };
static l_mem progress(l_mem debt, enum step_mode mode) {
  lua_State *L = luaL_newstate();
  l_mem before, marked;
  unsigned i, length = 0;
  require(L != NULL);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_loadstring(L, "local head; for i=1,10000 do "
                             "head={next=head} end; return head") == LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPMUL, 100);
  lua_gc(L, LUA_GCRESTART);
  /* Prime the root scan so a basic step follows the long live chain. */
  for (i = 0; i < 4; i++)
    lua_gc(L, LUA_GCSTEP, (size_t)0);
  if (mode == FULL)
    lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 0);
  lua_lock(L);
  require(G(L)->gcstate == GCSpropagate);
  before = G(L)->GCmarked;
  luaE_setdebt(G(L), debt);
  if (mode == EXPLICIT) {
    lua_unlock(L);
    lua_gc(L, LUA_GCSTEP, (size_t)8192);
    lua_lock(L);
  } else
    luaC_step(L);
  if (mode == FULL)
    require(G(L)->gcstate == GCSpause);
  marked = G(L)->GCmarked - before;
  lua_unlock(L);
  lua_gc(L, LUA_GCCOLLECT);
  while (lua_istable(L, -1)) {
    lua_getfield(L, -1, "next");
    lua_remove(L, -2);
    length++;
  }
  require(length == 10000 && lua_isnil(L, -1));
  lua_close(L);
  return marked;
}

int main(int argc, char **argv) {
  l_mem basic = progress(0, AUTOMATIC), overdue;
  require(argc == 2 && basic > 0);
  if (strcmp(argv[1], "explicit") == 0) {
    overdue = progress(-8192, EXPLICIT);
    require(overdue == basic);
  } else if (strcmp(argv[1], "fast") == 0) {
    overdue = progress(-8192, FULL);
    require(overdue > basic * 8);
  } else {
    require(strcmp(argv[1], "overdue") == 0 ||
            strcmp(argv[1], "overflow") == 0);
    overdue = progress(strcmp(argv[1], "overflow") == 0 ? -MAX_LMEM : -8192,
                       AUTOMATIC);
    fprintf(stderr, "traced bytes: basic %lld, overdue %lld\n",
            (long long)basic, (long long)overdue);
    require(overdue > basic * 8);
  }
  return 0;
}
