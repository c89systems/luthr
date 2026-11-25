/*
** Shared sweep ownership. Payload work never owns a global heap cursor.
** See Copyright Notice in lua.h
*/
#define lsweep_c
#define LUA_CORE
#include "lprefix.h"

#include <string.h>
#include "ldo.h"
#include "lgc.h"
#include "ltablestorage.h"
#include "lscheduler.h"
#include "ltable.h"

/* The collector visits pages that have held collectable objects. The pool
** mutex serializes cursor advancement against return of the backing page. */
static void takepage (global_State *g, LuaSweepWork *work, unsigned limit) {
  LuaSweepPlan *plan = &g->sweep_plan;
  unsigned inspected = 0;
  while (plan->page_pool < LUA_ALLOC_BINS && inspected < limit) {
    unsigned pool = plan->page_pool;
    luaX_checksync(pthread_mutex_lock(&g->allocation_mutex[pool]));
    while (plan->pages[pool] != NULL && inspected < limit) {
      LuaAllocPage *page = plan->pages[pool];
      uint64_t epoch = __atomic_load_n(&page->sweep_epoch, __ATOMIC_RELAXED);
      plan->pages[pool] = page->sweep_next;
      inspected++;
      if (page->state == LUA_PAGE_OPEN && epoch != 0 &&
          (plan->kind != SWEEP_YOUNG || epoch >= plan->epoch)) {
        page->state = LUA_PAGE_SWEEPING;
        work->page = page;
        work->full_page = page->owner == NULL &&
          page->available_previous == NULL && page->local == NULL &&
          __atomic_load_n(&page->remote, __ATOMIC_ACQUIRE) == 0;
        break;
      }
    }
    if (plan->pages[pool] == NULL) plan->page_pool++;
    luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[pool]));
    if (work->page != NULL) break;
  }
  work->count = inspected;
}

int luaC_sweepready (global_State *g) {
  LuaSweepPlan *plan = &g->sweep_plan;
  return plan->phase == SWEEP_OBJECTS &&
    (plan->next != plan->limit || plan->page_pool < LUA_ALLOC_BINS);
}

void luaC_beginsweep (lua_State *L, unsigned kind, l_mem marked) {
  global_State *g = G(L);
  LuaSweepPlan *plan = &g->sweep_plan;
  uint64_t epoch = plan->epoch + 1;
  GCObject *main = obj2gco(mainthread(g));
  lua_assert(luaW_iscollector(L) && !luaC_tracing(L) && !luaC_sweeping(L));
  memset(plan, 0, sizeof(*plan));
  /* The stopped cohort contains every shared birth before this boundary.
  ** Allocations during sweeping set the birth bit for the next cycle. */
  __atomic_store_n(&g->young_work, LUA_YOUNG_SURVIVOR, __ATOMIC_RELEASE);
  plan->epoch = epoch;
  plan->kind = kind;
  for (unsigned pool = 0; pool < LUA_ALLOC_BINS; pool++)
    plan->pages[pool] = g->sweep_pages[pool];
  __atomic_fetch_and(&g->world.control, ~cast(uint64_t, AGEBITS),
                   __ATOMIC_RELAXED);
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
  luaC_setstate(g, GCSswpallgc);
  if (kind == SWEEP_MAJOR) {
    g->grayagain = g->weak = g->allweak = g->ephemeron = NULL;
  }
  else if (kind == SWEEP_OLD) {
    LuaSweepWork work = {0};
    int result;
    luaC_setkind(g, KGC_GENMINOR);
    result = luaC_sweepobject(L, main, kind, &work);
    if (result == SWEEP_WATCH) {
      plan->watch.head = main;
      plan->watch.tail = luaC_graylink(main);
      *plan->watch.tail = NULL;
    }
  }
  __atomic_fetch_add(&g->world.work_epoch, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&plan->active, 1, __ATOMIC_RELEASE);
  /* The mark driver can still need to restore its enclosing scope. Admit
  ** sweep helpers only when a sweep driver has entered its payload phase. */
  __atomic_store_n(&g->sweep_workers, 0, __ATOMIC_RELEASE);
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
  if (plan->phase == SWEEP_OBJECTS && plan->next == plan->limit) {
    takepage(g, work, limit);
    if (work->count == 0 && plan->page_retry != NULL)
      take(work, &plan->page_retry, NULL, NULL, limit, 0);
    work->paged = work->count != 0;
  }
  if (work->count == 0 && plan->next == plan->limit && plan->retry[0] == NULL &&
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
  __atomic_store_n(&g->sweep_workers, luaC_sweepready(g), __ATOMIC_RELEASE);
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
    GCObject **retry = work->paged ? &plan->page_retry : &plan->retry[region];
    *work->retry_tail = *retry;
    *retry = work->retry;
  }
  if (work->threads != NULL) {
    *work->threads_tail = plan->threads;
    plan->threads = work->threads;
  }
  if (work->watch.head != NULL) {
    /* Keep promoted and filtered owners separate from the gray input. */
    *work->watch.tail = plan->watch.head;
    if (plan->watch.head == NULL) plan->watch.tail = work->watch.tail;
    plan->watch.head = work->watch.head;
  }
  plan->addedold += work->addedold;
  plan->minor_work |= work->minor_work;
  lua_assert(plan->busy != 0);
  plan->busy--;
  if (plan->busy == 0 || work->retry != NULL)
    luaQ_wake(&g->phase_changed, &g->phase_waiters);
}

/* All claims have published before the sweep reaches DONE. Mutators' new
** objects stay on their private lists until the next existing world stop. */
static int publishfinish (lua_State *L) {
  global_State *g = G(L);
  LuaSweepPlan *plan = &g->sweep_plan;
  int finalize;
  *plan->tail[1] = plan->limit;
  *plan->tail[0] = plan->live[1];
  *plan->append = plan->live[0];
  if (plan->watch.head != NULL) {
    *plan->watch.tail = g->grayagain;
    g->grayagain = plan->watch.head;
  }
  luaC_endsweep(L);
  finalize = plan->kind != SWEEP_MAJOR && luaC_kind(g) == KGC_GENMINOR &&
             !g->gcemergency && luaC_hasfinalizers(g);
  if (plan->kind != SWEEP_MAJOR && !plan->minor_work &&
      !luaC_hasfinalizers(g) && g->finobj == g->finobjold1)
    __atomic_fetch_and(&g->young_work, ~cast(unsigned, LUA_YOUNG_SURVIVOR),
                       __ATOMIC_RELEASE);
  __atomic_store_n(&plan->active, 0, __ATOMIC_RELEASE);
  if (plan->kind == SWEEP_YOUNG) luaC_publishminor(g);
  luaQ_wake(&g->phase_changed, &g->phase_waiters);
  return finalize;
}

enum { SWEEP_NO_MAINTENANCE, SWEEP_RESIZE_STRINGS, SWEEP_DRAIN_FINALIZERS };

/* The last claimant can publish the completed cycle under the plan mutex.
** The finalizer queue is already rooted and callbacks use ordinary mutator
** admission. Only resizing intern tables needs to join all mutators here. */
static int finishready (lua_State *L) {
  global_State *g = G(L);
  LuaSweepPlan *plan = &g->sweep_plan;
  if (!luaC_sweeping(L) || plan->phase != SWEEP_DONE) return SWEEP_NO_MAINTENANCE;
  lua_assert(plan->busy == 0);
  if (!g->gcemergency &&
      __atomic_load_n(&g->shrink_strings, __ATOMIC_RELAXED))
    return SWEEP_RESIZE_STRINGS;
  return publishfinish(L) ? SWEEP_DRAIN_FINALIZERS : SWEEP_NO_MAINTENANCE;
}

static int finishmaintenance (lua_State *L, uint64_t epoch, int complete) {
  global_State *g = G(L);
  LuaWorld *world = &g->world;
  LuaSweepPlan *plan = &g->sweep_plan;
  int finalize = 0;
  if (complete) luaW_collectenter(L);
  else {
    unsigned expected = 0;
    if (!luaW_ownsheap(L) &&
        !__atomic_compare_exchange_n(&plan->finishing, &expected, 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    if (!luaW_trycollectenter(L)) {
      __atomic_store_n(&plan->finishing, 0, __ATOMIC_RELEASE);
      return 0;
    }
  }
  if (!luaC_sweeping(L) || plan->epoch != epoch) {
    if (plan->epoch == epoch)
      __atomic_store_n(&plan->finishing, 0, __ATOMIC_RELEASE);
    luaW_heapleave(L);
    return 0;
  }
  luaW_stop(L);
  luaW_heapleave(L);
  if (luaC_sweeping(L) && plan->epoch == epoch) {
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    finalize = publishfinish(L);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    luaC_shrinkstrings(L);
  }
  luaW_start(L);
  return finalize;
}

static lu_mem sweepbatch (lua_State *L, unsigned limit, int advance,
                           int complete) {
  LuaSweepWork work = {0};
  LuaGCFrame frame;
  LuaWorld *world = &G(L)->world;
  uint64_t epoch = G(L)->sweep_plan.epoch;
  int finish, finalize;
  unsigned previous, scanned;
  if (!luaC_sweeping(L) || limit == 0) return 0;
  luaC_entergc(L, &frame);
  frame.batch = 1;
  if (limit > LUA_TRACE_BATCH) limit = LUA_TRACE_BATCH;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  previous = __atomic_load_n(&G(L)->sweep_workers, __ATOMIC_RELAXED);
  claim(L, &work, limit);
  finish = advance ? finishready(L) : SWEEP_NO_MAINTENANCE;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  if (!previous && luaC_workerready(G(L))) luaSC_notifygc(G(L));
  scanned = work.count;
  if (work.count != 0) {
    luaHS_scanbegin(L);
    luaC_scansweep(L, &work);
    luaHS_scanend(L);
    if (work.count != 0) scanned = work.count;
    if (work.page != NULL) {
      unsigned pool = work.page->pool;
      luaX_checksync(pthread_mutex_lock(&G(L)->allocation_mutex[pool]));
      work.page->state = LUA_PAGE_OPEN;
      luaX_checksync(pthread_mutex_unlock(&G(L)->allocation_mutex[pool]));
    }
    frame.batch = 0;
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    luaC_publishretired(L, &frame);
    publish(L, &work);
    luaP_work(G(L), cast(lu_mem, scanned) * sizeof(GCObject));
    finish = advance ? finishready(L) : SWEEP_NO_MAINTENANCE;
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
  frame.batch = 0;
  finalize = finish == SWEEP_RESIZE_STRINGS
           ? finishmaintenance(L, epoch, complete)
           : finish == SWEEP_DRAIN_FINALIZERS;
  if (finalize && luaD_checkminstack(L)) luaC_runfinalizers(L);
  luaC_leavegc(L, &frame);
  return cast(lu_mem, scanned) * sizeof(GCObject);
}

lu_mem luaC_sweepbatch (lua_State *L, unsigned limit, int complete) {
  return sweepbatch(L, limit, 1, complete);
}

/* Background capacity never runs finalizers or changes collector phases. */
lu_mem luaC_sweeppayload (lua_State *L, unsigned limit) {
  return sweepbatch(L, limit, 0, 0);
}
