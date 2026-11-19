/* Bounded idle-capacity payload work. See lua.h for license. */
#define LUA_CORE
#include "lprefix.h"
#include "lscheduler.h"
#include "lapi.h"
#include "lgc.h"

void luaSC_collectidle (LuaCarrier *carrier, LuaExecutionSlot *slot) {
  LuaScheduler *s = carrier->scheduler;
  lua_State *L = slot->collector;
  NativeStackContext *previous;
  LuaGCFrame frame;
  unsigned batches;
  carrier->collector_context.execution = &slot->collector_execution;
  previous = luaE_setnativestackcontext(&carrier->collector_context);
  if (luaX_tryworker(L)) {
    luaC_entergc(L, &frame);
    /* Every range joins the main state's existing destruction queue before
    ** publishing completion. Full GC therefore retains its cleanup contract. */
    frame.reclaimer = mainthread(s->global);
    for (batches = 0; batches < 16 && luaC_workerready(s->global); batches++) {
      lu_mem worked;
      if (luaC_tracing(L)) worked = luaC_tracebatch(L, LUA_TRACE_BATCH);
      else worked = luaC_sweeppayload(L, LUA_TRACE_BATCH);
      if (worked == 0 ||
          __atomic_load_n(&s->global->world.stopped, __ATOMIC_SEQ_CST) ||
          __atomic_load_n(&s->runnable, __ATOMIC_ACQUIRE) != 0 ||
          __atomic_load_n(&s->reentry_waiting, __ATOMIC_ACQUIRE) != 0) break;
    }
    luaC_leavegc(L, &frame);
    lua_assert(luaC_retired(L) == NULL && !L->reclaim_pages);
    luaX_unlockstate(L);
  }
  lua_assert(slot->collector_execution.managed_depth == 0);
  (void)luaE_setnativestackcontext(previous);
}
