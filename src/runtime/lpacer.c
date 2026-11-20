/* Heap targets, trigger runway and shared mutator-assist credit. */
#define lpacer_c
#define LUA_CORE
#include "lprefix.h"

#include "lpacer.h"
#include "lgc.h"
#include "lstate.h"

static l_mem add (l_mem a, l_mem b) {
  return b > MAX_LMEM - a ? MAX_LMEM : a + b;
}

static l_mem live (global_State *g) {
  l_mem bytes = gettotalbytes(g) -
    __atomic_load_n(&g->GCretired, __ATOMIC_RELAXED);
  /* Physical completion updates these words separately. A concurrent sample
  ** may briefly observe the newer charged total with older retirement. */
  return bytes > 0 ? bytes : 0;
}

static void deadline (global_State *g, l_mem bytes) {
  LuaGCPacer *p = &g->pacer;
  l_mem growth = p->goal > g->GCcyclelive ? p->goal - g->GCcyclelive : 0;
  p->hard = add(p->goal, growth);
  __atomic_store_n(&p->debt, p->hard - bytes, __ATOMIC_RELAXED);
}

/* Invert the work/allocated-byte parameter without a floating-point fast path. */
static l_mem runway (global_State *g, lu_mem work) {
  unsigned p = g->gcparams[LUA_GCPSTEPMUL];
  unsigned m = p & 15u, e = p >> 4;
  l_mem bytes = work > cast(lu_mem, MAX_LMEM) ? MAX_LMEM : cast(l_mem, work);
  if (e != 0) { m += 16; e--; }
  if (m == 0) return MAX_LMEM;
  if (e >= 7) return (bytes >> (e - 7)) / m;
  e = 7 - e;
  if (bytes / m > MAX_LMEM >> e) return MAX_LMEM;
  return add((bytes / m) << e, ((bytes % m) << e) / m);
}

void luaP_idle (global_State *g) {
  LuaGCPacer *p = &g->pacer;
  /* Fixed handshake costs need an amortization window on a parallel heap.
  ** Keep the single-mutator footprint and zero parameters unchanged. Each
  ** additional simultaneously admitted mutator contributes a bounded 512-KiB
  ** window; this is independent of task count and total allocation volume. */
  if (p->parallelism > 1 && g->gcparams[g->gckind == KGC_GENMINOR
                                      ? LUA_GCPMINORMUL : LUA_GCPPAUSE] != 0) {
    lu_mem workers = p->parallelism - 1;
    l_mem window = workers > cast(lu_mem, MAX_LMEM) / (8 * LUA_GC_ASSIST_BYTES)
      ? MAX_LMEM : cast(l_mem, workers * (8 * LUA_GC_ASSIST_BYTES));
    l_mem minimum = add(g->GCcyclelive, window);
    if (p->goal < minimum) p->goal = minimum;
  }
  l_mem headroom = runway(g, p->work);
  l_mem floor = g->GCcyclelive;
  l_mem available = p->goal > floor ? p->goal - floor : 0;
  /* Bound the trigger to 45/64 .. 61/64 of the growth allowance. Starting
  ** almost immediately after the last cycle keeps allocation black forever;
  ** starting at the goal leaves no ordinary concurrent runway. Keep Lua's
  ** heap allowances, including small embedded heaps, rather than reserving a
  ** fixed process-sized minimum. Quotient/remainder arithmetic cannot wrap. */
  l_mem minimum = (available / 64) * 3 + ((available % 64) * 3) / 64;
  l_mem maximum = (available / 64) * 19 + ((available % 64) * 19) / 64;
  if (p->work != 0) p->previous = p->work;
  if (headroom < minimum) headroom = minimum;
  if (headroom > maximum) headroom = maximum;
  p->trigger = p->goal - headroom;
  deadline(g, live(g));
  {
    l_mem debt = p->trigger - live(g);
    luaE_setdebt(g, debt > 0 ? debt : 0);
  }
  p->parallelism = 0;  /* the next completed cycle observes its own demand */
}

void luaP_init (global_State *g) {
  LuaGCPacer *p = &g->pacer;
  p->trigger = p->hard = p->debt = p->start = 0;
  p->expected = p->work = p->previous = 0;
  p->nursery_scans = 0;
  p->parallelism = 0;
  p->goal = cast(l_mem, sizeof(global_State));
  luaP_idle(g);
}

void luaP_begin (global_State *g) {
  LuaGCPacer *p = &g->pacer;
  p->start = live(g);
  /* A growing live graph can consume the following cycle's allowance. The
  ** soft goal paces ordinary work; only this hard guard drains an overdue job. */
  deadline(g, p->start);
  p->expected = cast(lu_mem, add(p->start, p->start));
  if (p->previous > p->expected) p->expected = p->previous;
  p->work = 0;
  __atomic_store_n(&p->nursery_scans, 0, __ATOMIC_RELAXED);
}

void luaP_account (global_State *g, l_mem change) {
  l_mem current = __atomic_load_n(&g->pacer.debt, __ATOMIC_RELAXED), next;
  if (change == 0) return;
  do {
    if (change > 0 && current > MAX_LMEM - change) next = MAX_LMEM;
    else if (change < 0 && current < -MAX_LMEM - change) next = -MAX_LMEM;
    else next = current + change;
  } while (!__atomic_compare_exchange_n(&g->pacer.debt, &current, next, 0,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

void luaP_work (global_State *g, lu_mem work) {
  lu_mem current = __atomic_load_n(&g->pacer.work, __ATOMIC_RELAXED);
  lu_mem next;
  do {
    next = work > cast(lu_mem, MAX_LMEM) - current
      ? cast(lu_mem, MAX_LMEM) : current + work;
  } while (!__atomic_compare_exchange_n(&g->pacer.work, &current, next, 0,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

lu_mem luaP_budget (lua_State *L, lu_mem requested) {
  global_State *g = G(L);
  LuaGCPacer *p = &g->pacer;
  l_mem bytes = live(g);
  l_mem distance = p->goal > p->start ? p->goal - p->start : 0;
  lu_mem done = __atomic_load_n(&p->work, __ATOMIC_RELAXED);
  lu_mem target;
  if (L->allocation_debt < 0) {
    lu_mem pending = cast(lu_mem, 0) - cast(lu_mem, L->allocation_debt);
    bytes = pending > cast(lu_mem, MAX_LMEM - bytes)
      ? MAX_LMEM : bytes + cast(l_mem, pending);
  }
  else bytes = L->allocation_debt < bytes ? bytes - L->allocation_debt : 0;
  if (distance == 0 || bytes >= p->goal) target = p->expected;
  else if (bytes <= p->start) target = 0;
  else {
    /* Only the slow assist path computes the work/remaining-runway ratio.
    ** A wide intermediate prevents overflow and needs no hot-path atomics. */
    long double fraction = cast(long double, bytes - p->start) / distance;
    target = cast(lu_mem, fraction * p->expected);
  }
  if (target <= done) return 0;  /* other helpers already paid this allocation */
  target -= done;
  if (requested < LUA_GC_ASSIST_BYTES) requested = LUA_GC_ASSIST_BYTES;
  return target < requested ? target : requested;
}
