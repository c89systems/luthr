/*
** Shared collection phases. No execution owns a suspended continuation.
** See Copyright Notice in lua.h
*/
#define lcollector_c
#define LUA_CORE
#include "lprefix.h"

#include "lgc.h"
#include "lstate.h"

static void ephemerons (global_State *g, unsigned after) {
  LuaTracePlan *plan = &g->trace_plan;
  plan->phase = TRACE_EPHEMERONS;
  plan->after = after;
  plan->next = g->ephemeron;
  g->ephemeron = NULL;
  plan->deferred = plan->tail = NULL;
  plan->changed = 0;
  plan->round_changes = __atomic_load_n(&g->trace_changes, __ATOMIC_RELAXED);
}

static void clearing (global_State *g, unsigned phase) {
  LuaTracePlan *plan = &g->trace_plan;
  plan->phase = phase;
  plan->weak_list = 0;
  plan->next = g->weak;
  plan->deferred = plan->tail = NULL;
  if (phase == TRACE_CLEAR) {
    plan->clear_changes = __atomic_load_n(&g->trace_changes, __ATOMIC_RELAXED);
    __atomic_store_n(&g->trace_weakcutoff, 3, __ATOMIC_RELEASE);
  }
  else
    __atomic_store_n(&g->trace_weakcutoff, 1, __ATOMIC_RELEASE);
}

/* Quiescence closes the claim-to-publication gap before a phase transition.
** All payload cursors are stored in the plan before any worker can park.
** Competing helpers recheck the phase after acquiring this boundary. */
void luaC_advancetrace (lua_State *L) {
  global_State *g = G(L);
  LuaTracePlan *plan = &g->trace_plan;
  /* Several helpers may observe the same empty queue. Metadata admission
  ** orders phase requests; recheck after it, before stopping every worker. */
  luaW_collectenter(L);
  if (!luaC_tracing(L) || plan->finish == 0 ||
      (plan->phase != TRACE_INIT &&
       __atomic_load_n(&g->trace_ready, __ATOMIC_ACQUIRE))) {
    luaW_heapleave(L);
    return;
  }
  luaW_stop(L);
  luaW_heapleave(L);  /* retain exactly the stop's metadata ownership */
  luaC_publish(L);
  luaC_drainbarriers(L);
  while (luaC_tracing(L) && plan->finish != 0) {
    if (plan->phase == TRACE_INIT)
      luaC_initialtrace(L);
    if (g->gray != NULL || g->trace_deferred != NULL) break;
    if (plan->again != NULL) {
      g->gray = plan->again;
      plan->again = NULL;
      break;
    }
    if (plan->next != NULL || plan->deferred != NULL) break;
    switch (plan->phase) {
      case TRACE_MARK:
        ephemerons(g, TRACE_VALUES);
        break;
      case TRACE_EPHEMERONS:
        if ((plan->changed || plan->round_changes !=
             __atomic_load_n(&g->trace_changes, __ATOMIC_RELAXED)) &&
            g->ephemeron != NULL) {
          plan->direction = !plan->direction;
          ephemerons(g, plan->after);
        }
        else if (plan->after == TRACE_VALUES || plan->after == TRACE_CLEAR)
          clearing(g, plan->after);
        else
          plan->phase = plan->after;
        break;
      case TRACE_VALUES:
        if (plan->weak_list++ == 0)
          plan->next = g->allweak;
        else
          ephemerons(g, TRACE_FINALIZERS);
        break;
      case TRACE_FINALIZERS:
        luaC_finalizetrace(L);
        ephemerons(g, TRACE_CLEAR);
        break;
      case TRACE_CLEAR:
        if (plan->weak_list == 0) {
          plan->weak_list = 1;
          plan->next = g->allweak;
        }
        else if (plan->weak_list == 1) {
          plan->weak_list = 2;
          plan->next = g->ephemeron;
        }
        else
          ephemerons(g, TRACE_VERIFY);
        break;
      case TRACE_VERIFY:
        if (plan->clear_changes !=
            __atomic_load_n(&g->trace_changes, __ATOMIC_RELAXED))
          clearing(g, TRACE_CLEAR);
        else {
          luaC_finishtrace(L);
          luaW_start(L);
          return;  /* finalizers may have allowed a later cycle to start */
        }
        break;
      default: lua_assert(0); break;
    }
  }
  if (luaC_tracing(L))
    __atomic_store_n(&g->trace_ready,
      g->gray != NULL || g->trace_deferred != NULL ||
      plan->next != NULL || plan->deferred != NULL, __ATOMIC_RELEASE);
  luaW_start(L);
}
