/*
** Shared collection phases. No execution owns a suspended continuation.
** See Copyright Notice in lua.h
*/
#define lcollector_c
#define LUA_CORE
#include "lprefix.h"

#include "lgc.h"
#include "lscheduler.h"
#include "lstate.h"

static void ephemerons (global_State *g, unsigned after) {
  LuaTracePlan *plan = &g->trace_plan;
  plan->after = after;
  plan->next = g->ephemeron;
  g->ephemeron = NULL;
  plan->deferred = plan->tail = NULL;
  plan->changed = 0;
  plan->round_changes = __atomic_load_n(&g->trace_changes, __ATOMIC_ACQUIRE);
  __atomic_store_n(&plan->phase, TRACE_EPHEMERONS, __ATOMIC_RELEASE);
}

/* Starting or repeating a round only shades additional live objects. An
** empty round still needs quiescence to close roots and pending barriers
** before the collector makes any weak or finalization decisions. */
static int continueephemerons (global_State *g) {
  LuaTracePlan *plan = &g->trace_plan;
  if (g->ephemeron == NULL) return 0;
  if (plan->phase == TRACE_MARK)
    ephemerons(g, TRACE_VALUES);
  else if (plan->phase == TRACE_EPHEMERONS &&
           (plan->changed || plan->round_changes !=
            __atomic_load_n(&g->trace_changes, __ATOMIC_ACQUIRE))) {
    plan->direction = !plan->direction;
    ephemerons(g, plan->after);
  }
  else return 0;
  return 1;
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

/* The coordinator only moves queues. Keep it compact while leaving the
** graph work in the scanner. */
#if defined(__GNUC__) && !defined(__clang__) && defined(__OPTIMIZE__)
__attribute__((optimize("Os")))
#endif
static void advanceclosedtrace (lua_State *L) {
  global_State *g = G(L);
  LuaTracePlan *plan = &g->trace_plan;
  luaC_finishroots(L);
  while (luaC_tracing(L) && plan->finish != 0) {
    if (plan->phase == TRACE_INIT)
      luaC_initialtrace(L);
    if (g->gray != NULL || g->trace_deferred != NULL ||
        g->trace_stacks != NULL) break;
    if (plan->again != NULL) {
      g->gray = plan->again;
      plan->again = NULL;
      break;
    }
    if (plan->next != NULL || plan->deferred != NULL || plan->pending != NULL ||
        (plan->phase == TRACE_FINALIZERS && plan->finalizers != NULL)) break;
    if (continueephemerons(g)) break;
    switch (plan->phase) {
      case TRACE_PENDING:
        plan->phase = TRACE_MARK;
        break;
      case TRACE_MARK:
        ephemerons(g, TRACE_VALUES);
        break;
      case TRACE_EPHEMERONS:
        if (plan->after == TRACE_VALUES || plan->after == TRACE_CLEAR)
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
    luaC_readytrace(g);
}

static int canadvance (lua_State *L) {
  global_State *g = G(L);
  return luaC_scanready(g) && g->trace_plan.finish != 0 &&
    __atomic_load_n(&g->trace_busy, __ATOMIC_RELAXED) == 0 &&
    (luaC_tracephase(g) == TRACE_INIT || !luaC_tracepending(g));
}

/* Quiescence closes the claim-to-publication gap before a weak decision.
** Reuse an existing closed boundary when available. Competing helpers
** recheck their observation before requesting a new rendezvous. */
#if defined(__GNUC__) && !defined(__clang__) && defined(__OPTIMIZE__)
__attribute__((optimize("Os")))
#endif
void luaC_advancetrace (lua_State *L) {
  global_State *g = G(L);
  int ready, continued;
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
  continued = ready && g->trace_plan.again == NULL && continueephemerons(g);
  if (continued) {
    luaC_readytrace(g);
    luaQ_wake(&g->trace_changed, &g->trace_waiters);
  }
  luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
  if (!ready || continued) {
    luaW_heapleave(L);
    if (continued) luaSC_notifygc(g);
    return;
  }
  luaW_stop(L);  /* also publishes allocations and drains barriers */
  luaW_heapleave(L);  /* retain exactly the stop's metadata ownership */
  advanceclosedtrace(L);
  luaW_start(L);
}
