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

static unsigned stops;
static int measured;
void __real_luaW_stop(lua_State *);
void __wrap_luaW_stop(lua_State *L) {
  if (measured)
    stops++;
  __real_luaW_stop(L);
}

static GCObject *object(lua_State *L, int index) {
  return gcvalue(s2v(L->stack.p + index));
}

static void prepare(lua_State *L) {
  lua_gc(L, LUA_GCCOLLECT);
  lua_lock(L);
  luaW_stop(L);
  if (G(L)->gckind == KGC_INC)
    luaC_runtilstate(L, GCSenteratomic, 0);
  luaW_start(L);
  lua_unlock(L);
}

static void drain(lua_State *L) {
  lua_lock(L);
  luaW_stop(L);
  luaW_start(L);
  lua_unlock(L);
}

static void repeated(lua_State *L) {
  unsigned i;
  lua_newtable(L);
  prepare(L);
  require(isblack(object(L, 1)));
  lua_newtable(L);
  measured = 1;
  for (i = 0; i < 256; i++) {
    lua_pushvalue(L, 2);
    lua_rawseti(L, 1, 1);
  }
  measured = 0;
  require(stops == 0); /* One dirty owner must not fill an edge buffer. */
  drain(L);
  require(isgray(object(L, 1)));
  lua_settop(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(L, 1, 1) == LUA_TTABLE);
}

static int closure(lua_State *L) {
  (void)L;
  return 0;
}

static void forward(lua_State *L) {
  unsigned i;
  lua_pushnil(L);
  lua_pushcclosure(L, closure, 1);
  prepare(L);
  require(isblack(object(L, 1)));
  measured = 1;
  for (i = 0; i < 256; i++) {
    lua_newtable(L);
    require(lua_setupvalue(L, 1, 1) != NULL);
  }
  measured = 0;
  require(stops >=
          4); /* Distinct forward edges must retain bounded draining. */
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_getupvalue(L, 1, 1) != NULL && lua_istable(L, -1));
}

static void mixed(lua_State *L) {
  lua_newtable(L);
  lua_pushnil(L);
  lua_pushcclosure(L, closure, 1);
  prepare(L);
  require(isblack(object(L, 1)) && isblack(object(L, 2)));
  lua_newtable(L);
  lua_newtable(L);
  /* The first edge marks child 3 before the remembered owner is processed.
   * Its later child 4 still needs a rescan, even though child 3 is now gray. */
  lua_pushvalue(L, 3);
  require(lua_setupvalue(L, 2, 1) != NULL);
  lua_pushvalue(L, 3);
  lua_rawseti(L, 1, 1);
  lua_pushvalue(L, 4);
  lua_rawseti(L, 1, 2);
  drain(L);
  require(!iswhite(object(L, 3)) && isgray(object(L, 1)));
  lua_settop(L, 2);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(L, 1, 2) == LUA_TTABLE);
}

static void collisions(lua_State *L, int reset) {
  unsigned round, i;
  /* More owners than cache buckets also exercises collisions and eviction. */
  for (i = 1; i <= 40; i++)
    lua_newtable(L);
  for (round = 0; round < (reset ? 3U : 1U); round++) {
    prepare(L);
    lua_newtable(L);
    for (i = 1; i <= 40; i++) {
      unsigned index = round & 1 ? 41 - i : i;
      require(isblack(object(L, (int)index)));
      lua_pushvalue(L, 41);
      lua_rawseti(L, (int)index, (lua_Integer)round + 1);
      lua_pushvalue(L, 41);
      lua_rawseti(L, (int)index, (lua_Integer)round + 1);
    }
    drain(L);
    for (i = 1; i <= 40; i++)
      require(isgray(object(L, (int)i)));
    lua_settop(L, 40);
  }
  lua_gc(L, LUA_GCCOLLECT);
  for (i = 1; i <= 40; i++) {
    require(lua_rawgeti(L, (int)i, 1) == LUA_TTABLE);
    lua_pop(L, 1);
  }
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  require(L != NULL && argc == 3 && lua_checkstack(L, 100));
  if (strcmp(argv[2], "generational") == 0)
    lua_gc(L, LUA_GCGEN);
  else
    require(strcmp(argv[2], "incremental") == 0);
  lua_gc(L, LUA_GCSTOP);
  if (strcmp(argv[1], "repeated") == 0)
    repeated(L);
  else if (strcmp(argv[1], "forward") == 0)
    forward(L);
  else if (strcmp(argv[1], "mixed") == 0)
    mixed(L);
  else {
    require(strcmp(argv[1], "collisions") == 0 ||
            strcmp(argv[1], "reset") == 0);
    collisions(L, strcmp(argv[1], "reset") == 0);
  }
  lua_close(L);
  return 0;
}
