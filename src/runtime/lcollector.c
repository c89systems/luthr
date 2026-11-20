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

/* Phase changes consume closed input queues and publish their next work
** before any mutator can resume. */
static void advanceclosedtrace (lua_State *L) {
  global_State *g = G(L);
  LuaTracePlan *plan = &g->trace_plan;
  while (luaC_tracing(L) && plan->finish != 0) {
    if (plan->phase == TRACE_INIT)
      luaC_initialtrace(L);
    if (g->gray != NULL || g->trace_deferred != NULL) break;
    if (plan->again != NULL) {
      g->gray = plan->again;
      plan->again = NULL;
      break;
    }
    if (plan->next != NULL || plan->deferred != NULL || plan->pending != NULL ||
        (plan->phase == TRACE_FINALIZERS && plan->finalizers != NULL)) break;
    switch (plan->phase) {
      case TRACE_PENDING:
        plan->phase = TRACE_MARK;
        break;
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
        else {
          plan->phase = plan->after;
          if (plan->phase == TRACE_FINALIZERS) {
            unsigned skipped = luaC_skiplivefinalizers(g, LUA_TRACE_BATCH);
            if (skipped != 0) luaP_work(g, skipped * sizeof(GCObject));
          }
        }
        break;
      case TRACE_VALUES:
        if (plan->weak_list++ == 0)
          plan->next = g->allweak;
        else
          ephemerons(g, TRACE_FINALIZERS);
        break;
      case TRACE_FINALIZERS:
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
          return;
        }
        break;
      default: lua_assert(0); break;
    }
  }
  if (luaC_tracing(L))
    luaC_readytrace(L);
}

static int canadvance (lua_State *L) {
  global_State *g = G(L);
  return luaC_tracing(L) && g->trace_plan.finish != 0 &&
    __atomic_load_n(&g->trace_busy, __ATOMIC_RELAXED) == 0 &&
    (luaC_tracephase(g) == TRACE_INIT || !luaC_tracepending(g));
}

/* Quiescence closes the claim-to-publication gap before a phase transition.
** Reuse an existing closed boundary when starting a trace. Competing helpers
** recheck their observation before requesting a new rendezvous. */
void luaC_advancetrace (lua_State *L) {
  global_State *g = G(L);
  int ready;
  if (L->world_depth != 0) {
    if (!canadvance(L)) return;
    luaC_drainbarriers(L);  /* the stopped owner can still publish root edges */
    advanceclosedtrace(L);
    return;
  }
  if (__atomic_load_n(&g->trace_busy, __ATOMIC_ACQUIRE) != 0) return;
  if (!luaW_trycollectenter(L)) return;
  luaX_checksync(pthread_mutex_lock(&g->trace_mutex));
  ready = canadvance(L);
  luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
  if (!ready) {
    luaW_heapleave(L);
    return;
  }
  luaW_stop(L);  /* also publishes allocations and drains barriers */
  luaW_heapleave(L);  /* retain exactly the stop's metadata ownership */
  advanceclosedtrace(L);
  luaW_start(L);
}
