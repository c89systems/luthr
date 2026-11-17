/* Allocation-cache leases. No cache lease survives a managed resource wait.
** See Copyright Notice in lua.h
*/
#define lmemcache_c
#define LUA_CORE
#include "lprefix.h"

#include "lmempage.h"
#include "lstate.h"
#include "ltask.h"

static void notify (global_State *g) {
  if (__atomic_load_n(&g->allocation_waiters, __ATOMIC_SEQ_CST) != 0) {
    luaX_checksync(pthread_mutex_lock(&g->allocation_mutex[0]));
    luaX_checksync(pthread_cond_broadcast(&g->allocation_changed));
    luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[0]));
  }
}

void luaMC_acquire (LuaAllocationCache *cache) {
  global_State *g = cache->global;
  void **permission = cache->permission != NULL ? cache->permission : &cache->owner;
  void *expected = NULL, *token = luaQ_ownertoken();
  if (__atomic_compare_exchange_n(permission, &expected, token, 0,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
  lua_assert(expected != token);
  /* Only a short nonparking page lease can compete with the legal driver.
  ** Block without managed admission; a helper needs no carrier from this task. */
  luaX_checksync(pthread_mutex_lock(&g->allocation_mutex[0]));
  __atomic_fetch_add(&g->allocation_waiters, 1, __ATOMIC_SEQ_CST);
  for (;;) {
    expected = NULL;
    if (__atomic_compare_exchange_n(permission, &expected, token, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
    luaX_checksync(pthread_cond_wait(&g->allocation_changed,
                                    &g->allocation_mutex[0]));
  }
  __atomic_fetch_sub(&g->allocation_waiters, 1, __ATOMIC_SEQ_CST);
  luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[0]));
}

void luaMC_release (LuaAllocationCache *cache) {
  global_State *g = cache->global;
  void **permission = cache->permission != NULL ? cache->permission : &cache->owner;
  lua_assert(__atomic_load_n(permission, __ATOMIC_RELAXED) == luaQ_ownertoken());
  __atomic_store_n(permission, NULL, __ATOMIC_SEQ_CST);
  notify(g);  /* g, not the possibly retired cache, owns the wait channel */
}

void luaMC_join (LuaAllocationCache *cache) {
  global_State *g = cache->global;
  luaX_checksync(pthread_mutex_lock(&g->allocation_mutex[0]));
  __atomic_fetch_add(&g->allocation_waiters, 1, __ATOMIC_SEQ_CST);
  while (__atomic_load_n(&cache->borrowers, __ATOMIC_SEQ_CST) != 0)
    luaX_checksync(pthread_cond_wait(&g->allocation_changed,
                                    &g->allocation_mutex[0]));
  __atomic_fetch_sub(&g->allocation_waiters, 1, __ATOMIC_SEQ_CST);
  luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[0]));
}

void luaMC_enter (LuaExecution *execution) {
  LuaAllocationCache *cache = execution->allocation_cache;
  lua_assert(execution->memory_cache == NULL);
  if (cache == NULL) {
    LuaTaskRoot *task = __atomic_load_n(&execution->task, __ATOMIC_ACQUIRE);
    if (task != NULL) cache = &task->allocation;
  }
  if (cache != NULL && cache->global != NULL && cache->permission == NULL) {
    luaMC_acquire(cache);
    execution->memory_cache = cache;
  }
}

void luaMC_leave (LuaExecution *execution) {
  LuaAllocationCache *cache = execution->memory_cache;
  execution->memory_cache = NULL;
  if (cache != NULL) luaMC_release(cache);
}

int luaMC_borrow (lua_State *L, LuaAllocationCache *cache,
                  LuaCacheBorrow *borrow, int exclusive) {
  LuaExecution *execution = luaQ_current();
  void *expected = NULL;
  borrow->cache = cache;
  borrow->global = cache->global;
  borrow->thread = NULL;
  borrow->reservation = borrow->permission = 0;
  borrow->returned = NULL;
  borrow->context = NULL;
  __atomic_fetch_add(&cache->borrowers, 1, __ATOMIC_RELAXED);
  if (exclusive || (execution != NULL &&
      (execution->memory_cache == cache ||
       (execution->allocation_cache == cache && cache->permission != NULL))))
    return 1;
  if (cache->thread != NULL) {
    borrow->reservation = luaX_tryreserve(cache->thread);
    if (borrow->reservation != 0) borrow->thread = cache->thread;
    return borrow->reservation != 0;
  }
  if (!__atomic_compare_exchange_n(cache->permission != NULL
        ? cache->permission : &cache->owner, &expected, luaQ_ownertoken(), 0,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
  borrow->permission = 1;
  borrow->returned = cache->returned;
  borrow->context = cache->context;
  UNUSED(L);
  return 1;
}

void luaMC_return (LuaCacheBorrow *borrow) {
  LuaAllocationCache *cache = borrow->cache;
  if (cache == NULL) return;
  if (borrow->permission)
    __atomic_store_n(cache->permission != NULL ? cache->permission : &cache->owner,
                      NULL, __ATOMIC_SEQ_CST);
  /* This is the last cache access. Retirement may release its record before
  ** the caller drops the pool mutex and sends saved scheduler notifications. */
  __atomic_fetch_sub(&cache->borrowers, 1, __ATOMIC_SEQ_CST);
}

void luaMC_notify (LuaCacheBorrow *borrow) {
  if (borrow->cache == NULL) return;
  /* Gate release may wake a managed waiter. It must not nest that waiter's
  ** gate/scheduler bookkeeping beneath a page-pool mutex. */
  if (borrow->reservation != 0)
    luaX_endreserve(borrow->thread, borrow->reservation);
  notify(borrow->global);
  if (borrow->returned != NULL) borrow->returned(borrow->context);
}
