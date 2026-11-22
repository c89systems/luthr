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
  debt = luaE_getdebt(g);
  bytes = gettotalbytes(g) - g->GCretired;
  g->GCcyclelive = bytes;
  g->pacer.goal = bytes + 64 * 65536;
  g->pacer.work = 0;
  luaP_idle(g);
  require(g->pacer.trigger > bytes && g->pacer.trigger < g->pacer.goal);
  require(luaE_getdebt(g) == g->pacer.trigger - bytes);
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
  require(g->pacer.trigger == bytes && luaE_getdebt(g) == 0);
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
  require(g->pacer.trigger == 0 && luaE_getdebt(g) == 0);
  g->pacer.goal = MAX_LMEM;
  luaP_begin(g);
  require(g->pacer.hard == MAX_LMEM && luaP_debt(g) == MAX_LMEM - bytes);
  require(g->pacer.start == bytes && g->pacer.expected == (lu_mem)MAX_LMEM);
  luaE_setdebt(g, MAX_LMEM);
  require(luaE_getdebt(g) == MAX_LMEM - bytes);
  luaE_setdebt(g, -MAX_LMEM);
  require(luaE_getdebt(g) == -MAX_LMEM);
  g->pacer.goal = bytes + 65536;
  g->pacer.expected = 65536;
  require(luaP_budget(L, 65536) == 0);
  L->allocation_debt.batch.total = -32768;
  L->allocation_debt.batch.shared = -16384;
  require(luaP_budget(L, 65536) == 16384);
  L->allocation_debt.batch.shared = -32768;
  require(luaP_budget(L, 65536) == 32768);
  luaP_work(g, 16384);
  require(luaP_budget(L, 65536) == 16384);
  luaP_work(g, 16384);
  require(luaP_budget(L, 65536) == 0);
  /* Pending local allocation cannot wrap a hard limit into negative space. */
  g->pacer.goal = MAX_LMEM;
  g->pacer.expected = MAX_LMEM;
  g->pacer.work = 0;
  __atomic_store_n(&g->GCused, MAX_LMEM - 16384, __ATOMIC_RELAXED);
  require(luaP_budget(L, MAX_LMEM) == (lu_mem)MAX_LMEM);
  L->allocation_debt.pending = 0;
  __atomic_store_n(&g->GCused, bytes, __ATOMIC_RELAXED);
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
  allowance = luaP_debt(G(L));
  lua_unlock(L);
  if (strcmp(kind, "pair") == 0) {
    luaX_lockpair(L, peer);
    first = luaM_alloccontrol(L, 1024);
    second = luaM_alloccontrol(peer, 2048);
    require(first != NULL && second != NULL);
    require(luaM_shareddebt(L) == -1024 && luaM_shareddebt(peer) == -2048);
    luaX_unlockpair(L, peer);
    require(peer->allocation_debt.pending == 0);
  } else {
    lua_lock(L);
    first = luaM_alloccontrol(L, 1024);
    require(first != NULL && luaM_shareddebt(L) == -1024);
    if (strcmp(kind, "withdrawal") == 0) {
      require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
      luaW_deactivate(L);
      require(L->allocation_debt.pending == 0 &&
              gettotalbytes(G(L)) == before + 1024);
      luaW_resume(L);
      require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
    } else
      require(strcmp(kind, "gate") == 0);
    lua_unlock(L);
  }
  require(L->allocation_debt.pending == 0);
  require(gettotalbytes(G(L)) == before + (second != NULL ? 3072 : 1024));
  require(luaP_debt(G(L)) == allowance - (second != NULL ? 3072 : 1024));
  lua_lock(L);
  luaM_free_(L, first, 1024);
  if (second != NULL)
    luaM_free_(L, second, 2048);
  lua_unlock(L);
  require(gettotalbytes(G(L)) == before);
  require(luaP_debt(G(L)) == allowance);
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
