/*
** Shared sweep ownership. Payload work never owns a global heap cursor.
** See Copyright Notice in lua.h
*/
#define lsweep_c
#define LUA_CORE
#include "lprefix.h"

#include <string.h>
#include "lgc.h"

void luaC_beginsweep (lua_State *L, unsigned kind, l_mem marked) {
  global_State *g = G(L);
  LuaSweepPlan *plan = &g->sweep_plan;
  uint64_t epoch = plan->epoch + 1;
  GCObject *main = obj2gco(mainthread(g));
  lua_assert(luaW_iscollector(L) && !luaC_tracing(L) && !luaC_sweeping(L));
  memset(plan, 0, sizeof(*plan));
  plan->epoch = epoch;
  plan->kind = kind;
  g->new_mark = cast_byte(g->major_mark | g->minor_mark);
  plan->base_marked = marked;
  plan->next = g->allgc;
  plan->middle = kind == SWEEP_YOUNG ? g->survival : NULL;
  plan->limit = kind == SWEEP_YOUNG ? g->old1 : main;
  plan->finhead = g->finobj;
  plan->finsurvival = g->finobjsur;
  plan->tail[0] = &plan->live[0];
  plan->tail[1] = &plan->live[1];
  g->allgc = plan->limit;
  plan->append = &g->allgc;
  g->gcstate = GCSswpallgc;
  if (kind == SWEEP_MAJOR) {
    g->grayagain = g->weak = g->allweak = g->ephemeron = NULL;
  }
  else if (kind == SWEEP_OLD) {
    int result;
    g->gckind = KGC_GENMINOR;
    result = luaC_sweepobject(L, main, kind, &plan->addedold, &plan->promoted);
    if (result == SWEEP_WATCH) {
      *luaC_graylink(main) = g->grayagain;
      g->grayagain = main;
    }
  }
  __atomic_fetch_add(&g->world.work_epoch, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&plan->active, 1, __ATOMIC_RELEASE);
}

/* world.mutex protects claims and short list publication. A batch remains
** admitted until all of its objects and discoveries have been returned. */
static void take (LuaSweepWork *work, GCObject **source, GCObject *limit,
                  GCObject *middle, unsigned count, int gray) {
  GCObject *first = *source, *object = first, **tail = NULL;
  while (object != limit && work->count < count) {
    lua_assert(object != NULL);
    if (object == middle && work->count != 0) break;
    tail = gray ? luaC_graylink(object) : luaC_alloclink(object);
    object = *tail;
    work->count++;
  }
  if (tail != NULL) {
    *source = object;
    work->next = first;
    *tail = NULL;  /* the range now belongs exclusively to this batch */
  }
}

static void claim (lua_State *L, LuaSweepWork *work, unsigned limit) {
  global_State *g = G(L);
  LuaSweepPlan *plan = &g->sweep_plan;
  unsigned region;
  if (!luaC_sweeping(L)) return;
  if (plan->next == plan->limit && plan->retry[0] == NULL &&
      plan->retry[1] == NULL && plan->busy == 0 &&
      plan->phase == SWEEP_OBJECTS)
    plan->phase = SWEEP_THREADS;
  if (plan->phase == SWEEP_THREADS && plan->threads == NULL && plan->busy == 0) {
    luaC_drainbarriers(L);
    plan->gray[0] = g->grayagain; g->grayagain = NULL;
    plan->gray[1] = g->weak; g->weak = NULL;
    plan->gray[2] = g->allweak; g->allweak = NULL;
    plan->gray[3] = g->ephemeron; g->ephemeron = NULL;
    plan->phase = plan->kind == SWEEP_MAJOR ? SWEEP_DONE : SWEEP_GRAY;
  }
  work->kind = plan->kind;
  work->phase = plan->phase;
  if (plan->phase == SWEEP_OBJECTS && plan->next != plan->limit) {
    if (plan->next == plan->middle) plan->region = 1;
    work->region = plan->region;
    take(work, &plan->next, plan->limit,
         plan->region == 0 ? plan->middle : NULL, limit, 0);
  }
  for (region = 0; region < 2 && work->count == 0; region++) {
    if (plan->phase == SWEEP_OBJECTS && plan->retry[region] != NULL) {
      work->region = region;
      take(work, &plan->retry[region], NULL, NULL, limit, 0);
    }
  }
  if (plan->phase == SWEEP_THREADS && plan->threads != NULL)
    take(work, &plan->threads, NULL, NULL, limit, 0);
  while (plan->phase == SWEEP_GRAY && work->count == 0) {
    if (plan->gray_index == 4) {
      if (plan->busy == 0 && work->count == 0) plan->phase = SWEEP_DONE;
      break;
    }
    if (plan->gray[plan->gray_index] == NULL) {
      plan->gray_index++;
      continue;
    }
    take(work, &plan->gray[plan->gray_index], NULL, NULL, limit, 1);
  }
  if (work->count != 0) plan->busy++;
}

static void publish (lua_State *L, LuaSweepWork *work) {
  global_State *g = G(L);
  LuaSweepPlan *plan = &g->sweep_plan;
  unsigned region = work->region;
  if (work->live != NULL) {
    *work->tail = plan->live[region];
    if (plan->live[region] == NULL) plan->tail[region] = work->tail;
    plan->live[region] = work->live;
  }
  if (work->retry != NULL) {
    *work->retry_tail = plan->retry[region];
    plan->retry[region] = work->retry;
  }
  if (work->threads != NULL) {
    *work->threads_tail = plan->threads;
    plan->threads = work->threads;
  }
  if (work->watch.head != NULL) {
    *work->watch.tail = g->grayagain;
    g->grayagain = work->watch.head;
  }
  plan->addedold += work->addedold;
  plan->promoted |= work->promoted;
  lua_assert(plan->busy != 0);
  plan->busy--;
  luaQ_wake(&g->world.changed, &g->world.waiters);
}

static void finish (lua_State *L) {
  global_State *g = G(L);
  LuaSweepPlan *plan = &g->sweep_plan;
  uint64_t epoch = plan->epoch;
  unsigned expected = 0;
  /* One poll may attempt this boundary. Defer busy metadata without parking;
  ** explicit/debt-paying callers join the owner in their shared work loop. */
  if (!luaW_iscollector(L) &&
      !__atomic_compare_exchange_n(&plan->finishing, &expected, 1, 0,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
  if (!luaW_trycollectenter(L)) {
    __atomic_store_n(&plan->finishing, 0, __ATOMIC_RELEASE);
    return;
  }
  if (!luaC_sweeping(L) || plan->epoch != epoch ||
      plan->phase != SWEEP_DONE || plan->busy != 0) {
    if (plan->epoch == epoch)
      __atomic_store_n(&plan->finishing, 0, __ATOMIC_RELEASE);
    luaW_heapleave(L);
    return;
  }
  luaW_stop(L);
  luaW_heapleave(L);
  if (luaC_sweeping(L) && plan->epoch == epoch) {
    *plan->tail[1] = plan->limit;
    *plan->tail[0] = plan->live[1];
    *plan->append = plan->live[0];
    __atomic_store_n(&plan->active, 0, __ATOMIC_RELEASE);
    luaC_endsweep(L);
  }
  luaW_start(L);
}

lu_mem luaC_sweepbatch (lua_State *L, unsigned limit) {
  LuaSweepWork work = {0};
  LuaGCFrame frame;
  LuaWorld *world = &G(L)->world;
  int done;
  if (!luaC_sweeping(L) || limit == 0) return 0;
  luaC_entergc(L, &frame);
  frame.batch = 1;
  if (limit > LUA_TRACE_BATCH) limit = LUA_TRACE_BATCH;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  claim(L, &work, limit);
  done = G(L)->sweep_plan.phase == SWEEP_DONE;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  if (work.count != 0) {
    luaC_scansweep(L, &work);
    frame.batch = 0;
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    luaC_publishretired(L, &frame);
    publish(L, &work);
    luaP_work(G(L), cast(lu_mem, work.count) * sizeof(GCObject));
    done = G(L)->sweep_plan.phase == SWEEP_DONE;
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
  frame.batch = 0;
  if (done) finish(L);
  luaC_leavegc(L, &frame);
  return cast(lu_mem, work.count) * sizeof(GCObject);
}
