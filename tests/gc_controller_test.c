#include <lapi.h>
#include <lauxlib.h>
#include <lmem.h>
#include <lpacer.h>
#include <lstate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static void controller(lua_State *L) {
  global_State *g = G(L);
  LuaGCPacer saved;
  l_mem cohort, debt, bytes;
  lua_lock(L);
  luaW_stop(L);
  saved = g->pacer;
  cohort = g->GCcyclelive;
  debt = g->GCdebt;
  bytes = gettotalbytes(g) - g->GCretired;
  g->GCcyclelive = bytes;
  g->pacer.goal = bytes + 64 * 65536;
  g->pacer.work = 0;
  luaP_idle(g);
  require(g->pacer.trigger > bytes && g->pacer.trigger < g->pacer.goal);
  require(g->GCdebt == g->pacer.trigger - bytes);
  luaP_work(g, MAX_LMEM);
  luaP_work(g, MAX_LMEM);
  require(g->pacer.work == (lu_mem)MAX_LMEM);
  luaP_idle(g);
  require(g->pacer.trigger >= bytes + 45 * 65536);
  require(g->pacer.trigger <= bytes + 61 * 65536);
  require(g->pacer.goal == bytes + 64 * 65536);
  g->pacer.parallelism = 8;
  g->pacer.goal = bytes;
  luaP_idle(g);
  require(g->pacer.goal == bytes + 7 * 512 * 1024);
  require(g->pacer.parallelism == 0);
  /* A later single-owner cycle need not retain a former parallel window. */
  g->pacer.goal = bytes;
  luaP_idle(g);
  require(g->pacer.trigger == bytes && g->GCdebt == 0);
  g->pacer.goal = 0;
  {
    lu_byte pause = g->gcparams[LUA_GCPPAUSE];
    g->gcparams[LUA_GCPPAUSE] = 0;
    g->pacer.parallelism = 8;
    luaP_idle(g);
    require(g->pacer.goal == 0);
    g->gcparams[LUA_GCPPAUSE] = pause;
  }
  luaP_idle(g);
  require(g->pacer.trigger == 0 && g->GCdebt == 0);
  g->pacer.goal = MAX_LMEM;
  luaP_begin(g);
  require(g->pacer.hard == MAX_LMEM && g->pacer.debt == MAX_LMEM - bytes);
  require(g->pacer.start == bytes && g->pacer.expected == (lu_mem)MAX_LMEM);
  g->pacer.debt = MAX_LMEM;
  luaP_account(g, MAX_LMEM);
  require(g->pacer.debt == MAX_LMEM);
  luaP_account(g, -MAX_LMEM);
  luaP_account(g, -MAX_LMEM);
  luaP_account(g, -1);
  require(g->pacer.debt == -MAX_LMEM);
  luaP_account(g, MAX_LMEM);
  require(g->pacer.debt == 0);
  g->pacer.goal = bytes + 65536;
  g->pacer.expected = 65536;
  require(luaP_budget(L, 65536) == 0);
  L->allocation_debt = -32768;
  require(luaP_budget(L, 65536) == 32768);
  luaP_work(g, 16384);
  require(luaP_budget(L, 65536) == 16384);
  luaP_work(g, 16384);
  require(luaP_budget(L, 65536) == 0);
  /* Pending local allocation cannot wrap a hard limit into negative space. */
  g->pacer.goal = MAX_LMEM;
  g->pacer.expected = MAX_LMEM;
  g->pacer.work = 0;
  L->allocation_debt = -MAX_LMEM;
  require(luaP_budget(L, MAX_LMEM) == (lu_mem)MAX_LMEM);
  L->allocation_debt = 0;
  g->pacer = saved;
  g->GCcyclelive = cohort;
  luaE_setdebt(g, debt);
  luaW_start(L);
  lua_unlock(L);
}

static void publication(lua_State *L, const char *kind) {
  lua_State *peer = lua_newthread(L);
  void *first, *second = NULL;
  l_mem before, allowance;
  lua_lock(L);
  before = gettotalbytes(G(L));
  allowance = G(L)->pacer.debt;
  lua_unlock(L);
  if (strcmp(kind, "pair") == 0) {
    luaX_lockpair(L, peer);
    first = luaM_alloccontrol(L, 1024);
    second = luaM_alloccontrol(peer, 2048);
    require(first != NULL && second != NULL);
    require(L->allocation_debt == -1024 && peer->allocation_debt == -2048);
    luaX_unlockpair(L, peer);
    require(peer->allocation_debt == 0);
  } else {
    lua_lock(L);
    first = luaM_alloccontrol(L, 1024);
    require(first != NULL && L->allocation_debt == -1024);
    if (strcmp(kind, "withdrawal") == 0) {
      require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
      luaW_deactivate(L);
      require(L->allocation_debt == 0 && gettotalbytes(G(L)) == before + 1024);
      luaW_resume(L);
      require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
    } else
      require(strcmp(kind, "gate") == 0);
    lua_unlock(L);
  }
  require(L->allocation_debt == 0);
  require(gettotalbytes(G(L)) == before + (second != NULL ? 3072 : 1024));
  require(G(L)->pacer.debt == allowance - (second != NULL ? 3072 : 1024));
  lua_lock(L);
  luaM_free_(L, first, 1024);
  if (second != NULL)
    luaM_free_(L, second, 2048);
  lua_unlock(L);
  require(gettotalbytes(G(L)) == before);
  require(G(L)->pacer.debt == allowance);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  require(argc == 2 && L != NULL);
  lua_gc(L, LUA_GCSTOP);
  if (strcmp(argv[1], "controller") == 0)
    controller(L);
  else
    publication(L, argv[1]);
  lua_close(L);
  return 0;
}
