/* Reclaimers borrow allocation caches through stack gates or scheduler slots.
** See Copyright Notice in lua.h
*/
#define lmemcache_c
#define LUA_CORE
#include "lprefix.h"

#include "lmempage.h"
#include "lstate.h"

int luaMC_borrow (lua_State *L, LuaAllocationCache *cache,
                  LuaCacheBorrow *borrow, int exclusive) {
  LuaExecution *execution = luaX_execution(L);
  void *expected = NULL;
  if (exclusive || (execution != NULL && execution->allocation_cache == cache))
    return 1;
  if (cache->thread != NULL) {
    borrow->reservation = luaX_tryreserve(cache->thread);
    if (borrow->reservation != 0) borrow->thread = cache->thread;
    return borrow->reservation != 0;
  }
  lua_assert(cache->permission != NULL);
  if (!__atomic_compare_exchange_n(cache->permission,
        &expected, luaQ_ownertoken(), 0,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
  borrow->permission = cache->permission;
  borrow->returned = cache->returned;
  borrow->context = cache->context;
  return 1;
}

void luaMC_return (LuaCacheBorrow *borrow) {
  if (borrow->permission != NULL)
    __atomic_store_n(borrow->permission, NULL, __ATOMIC_SEQ_CST);
}

void luaMC_notify (LuaCacheBorrow *borrow) {
  /* Gate release may wake a managed waiter. It must not nest that waiter's
  ** gate/scheduler bookkeeping beneath a page-pool mutex. */
  if (borrow->reservation != 0)
    luaX_endreserve(borrow->thread, borrow->reservation);
  if (borrow->returned != NULL) borrow->returned(borrow->context);
}
