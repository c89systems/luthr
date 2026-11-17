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
static void *release_string(void *context, void *pointer, size_t old,
                            size_t size) {
  unsigned *freed = context;
  require(size == 0 && old == 65);
  (*freed)++;
  free(pointer);
  return NULL;
}

static l_mem progress(l_mem debt, enum step_mode mode) {
  lua_State *L = luaL_newstate();
  l_mem reclaimed;
  unsigned i, length = 0, freed = 0;
  require(L != NULL);
  lua_gc(L, LUA_GCINC);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_loadstring(L, "local head; for i=1,10000 do "
                             "head={next=head} end; return head") == LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  for (i = 0; i < 5000; i++) {
    char *text = malloc(65);
    require(text != NULL);
    memset(text, 'x', 64);
    text[64] = '\0';
    lua_pushexternalstring(L, text, 64, release_string, &freed);
    lua_pop(L, 1);
  }
  /* A newest live object lets sweeping reach the garbage through a stable
   * interior cursor, rather than retiring the whole dead head while entering
   * the sweep phase. The long chain remains a separate graph safety check. */
  lua_newtable(L);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPMUL, 100);
  lua_gc(L, LUA_GCRESTART);
  lua_gc(L, LUA_GCSTEP, (size_t)0);
  require(freed == 0);
  if (mode == FULL)
    lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 0);
  lua_lock(L);
  require(G(L)->gcstate == GCSswpallgc);
  luaE_setdebt(G(L), debt);
  if (mode == EXPLICIT) {
    lua_unlock(L);
    lua_gc(L, LUA_GCSTEP, (size_t)8192);
    lua_lock(L);
  } else
    luaC_step(L);
  if (mode == FULL)
    require(G(L)->gcstate == GCSpause);
  reclaimed = (l_mem)freed;
  lua_unlock(L);
  lua_gc(L, LUA_GCCOLLECT);
  lua_pop(L, 1); /* newest live anchor */
  while (lua_istable(L, -1)) {
    lua_getfield(L, -1, "next");
    lua_remove(L, -2);
    length++;
  }
  require(length == 10000 && lua_isnil(L, -1));
  lua_close(L);
  return reclaimed;
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
    fprintf(stderr, "reclaimed strings: basic %lld, overdue %lld\n",
            (long long)basic, (long long)overdue);
    require(overdue > basic * 8);
  }
  return 0;
}
