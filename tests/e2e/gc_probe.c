#define _POSIX_C_SOURCE 200809L
#include "gc_probe.h"
#include "../../src/policy.h"
#include <inttypes.h>
#include <lgc.h>
#include <lmem.h>
#include <lscheduler.h>
#include <lstate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { REASON_OTHER, REASON_GC, REASON_BARRIER };
enum {
  COLLECT_STEP,
  COLLECT_FULL,
  COLLECT_ASSIST,
  COLLECT_POLL,
  COLLECT_SWEEP,
  COLLECT_SWEEP_ASSIST
};
static lua_State *host;
static GcProbe *host_probe;
static unsigned scanners;
static unsigned sweepers;
static __thread GcProbe *worker_probe;
static GcProbe worker_total;
static pthread_mutex_t workers_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t workers_changed = PTHREAD_COND_INITIALIZER;
static unsigned worker_observers;
static int worker_sampling;

/* Each node lives on the waiting invocation's native/fiber stack. The probe's
 * short mutex lets wake observers inspect it from any runtime source file or
 * resource, including releases that the linker cannot interpose directly. */
struct metadata_wait {
  struct metadata_wait *next;
  LuaWaiter **waiters;
  LuaWorld *world;
  void *owner;
  uint64_t granted;
};
static struct metadata_wait *metadata_waiters;
static pthread_mutex_t metadata_probe_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now(void) {
  struct timespec stamp;
  if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0)
    abort();
  return (uint64_t)stamp.tv_sec * 1000000000 + (uint64_t)stamp.tv_nsec;
}

static GcProbe *probe(lua_State *L) {
  /* Ownership already exists here. The public context getter would reenter
   * the stack gate. Only this executable installs these invocation contexts. */
  GcProbe *result = worker_probe != NULL ? worker_probe
                    : L == host          ? host_probe
                                         : luthr_policy_get_context(L->policy);
  return result != NULL && result->enabled ? result : NULL;
}

void gc_probe_bind(lua_State *L, GcProbe *record) {
  /* Called before carriers start; immutable until they have been joined. */
  host = L;
  host_probe = record;
}

void *__real_luaAL_realloc(lua_State *, void *, size_t, size_t);
void *__wrap_luaAL_realloc(lua_State *L, void *block, size_t old, size_t size) {
  GcProbe *record = probe(L);
  if (record != NULL)
    record->allocator_calls++;
  return __real_luaAL_realloc(L, block, old, size);
}

void gc_probe_add(GcProbe *total, const GcProbe *record) {
#define GC_PROBE_ADD(name) total->name += record->name;
  GC_PROBE_COUNTERS(GC_PROBE_ADD)
#undef GC_PROBE_ADD
#define GC_PROBE_MAX(name)                                                     \
  if (record->name > total->name)                                              \
    total->name = record->name;
  GC_PROBE_MAXIMA(GC_PROBE_MAX)
#undef GC_PROBE_MAX
}

void gc_probe_begin_workers(void) {
  luaX_checksync(pthread_mutex_lock(&workers_mutex));
  if (worker_observers != 0 || worker_sampling)
    abort();
  memset(&worker_total, 0, sizeof(worker_total));
  worker_sampling = 1;
  luaX_checksync(pthread_mutex_unlock(&workers_mutex));
}

void gc_probe_end_workers(GcProbe *total) {
  luaX_checksync(pthread_mutex_lock(&workers_mutex));
  worker_sampling = 0;
  while (worker_observers != 0)
    luaX_checksync(pthread_cond_wait(&workers_changed, &workers_mutex));
  gc_probe_add(total, &worker_total);
  luaX_checksync(pthread_mutex_unlock(&workers_mutex));
}

void __real_luaSC_collectidle(LuaCarrier *, LuaExecutionSlot *);
void __wrap_luaSC_collectidle(LuaCarrier *carrier, LuaExecutionSlot *slot) {
  GcProbe record = {0};
  uint64_t started;
  luaX_checksync(pthread_mutex_lock(&workers_mutex));
  if (!worker_sampling) {
    luaX_checksync(pthread_mutex_unlock(&workers_mutex));
    __real_luaSC_collectidle(carrier, slot);
    return;
  }
  worker_observers++;
  luaX_checksync(pthread_mutex_unlock(&workers_mutex));
  record.enabled = 1;
  record.gc_depth = 1;
  worker_probe = &record;
  started = now();
  __real_luaSC_collectidle(carrier, slot);
  record.worker_ns = now() - started;
  worker_probe = NULL;
  record.worker_calls = record.collector_calls = 1;
  if (record.trace_calls != 0 || record.sweep_calls != 0) {
    record.collector_running_calls = 1;
    record.collector_body_ns += record.worker_ns - record.coordination_ns;
  } else {
    record.worker_empty_calls = 1;
    record.collector_disabled_ns += record.worker_ns - record.coordination_ns;
  }
  luaX_checksync(pthread_mutex_lock(&workers_mutex));
  gc_probe_add(&worker_total, &record);
  if (--worker_observers == 0)
    luaX_checksync(pthread_cond_broadcast(&workers_changed));
  luaX_checksync(pthread_mutex_unlock(&workers_mutex));
}

void gc_probe_emit(const GcProbe *record) {
#define GC_PROBE_EMIT(name) printf(",\"gc_" #name "\":%" PRIu64, record->name);
  GC_PROBE_COUNTERS(GC_PROBE_EMIT)
  GC_PROBE_MAXIMA(GC_PROBE_EMIT)
#undef GC_PROBE_EMIT
}

void gc_probe_allocation(uint64_t live) {
  GcProbe *record = host_probe;
  uint64_t *peak, previous;
  global_State *g;
  if (record == NULL || !record->enabled)
    return;
  g = G(host);
  previous =
      __atomic_load_n(&record->logical_live_peak_bytes, __ATOMIC_RELAXED);
  {
    l_mem observed =
        gettotalbytes(g) - __atomic_load_n(&g->GCretired, __ATOMIC_RELAXED);
    uint64_t logical = observed > 0 ? (uint64_t)observed : 0;
    while (previous < logical &&
           !__atomic_compare_exchange_n(&record->logical_live_peak_bytes,
                                        &previous, logical, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
  }
  if (luaC_tracing(host))
    peak = &record->mark_peak_bytes;
  else if (issweepphase(g))
    peak = &record->sweep_peak_bytes;
  else if (luaC_hasreclaimers(host))
    peak = &record->reclaim_peak_bytes;
  else
    peak = &record->idle_peak_bytes;
  previous = __atomic_load_n(peak, __ATOMIC_RELAXED);
  while (previous < live &&
         !__atomic_compare_exchange_n(peak, &previous, live, 0,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

static void coordination_begin(GcProbe *record) {
  if (record->coordination_depth++ == 0)
    record->coordination_start = now();
}
static void coordination_end(GcProbe *record) {
  if (--record->coordination_depth == 0)
    record->coordination_ns += now() - record->coordination_start;
}

void __real_luaW_stop(lua_State *);
void __real_luaW_start(lua_State *);
void __real_luaW_heapenter(lua_State *);
void __real_luaW_collectenter(lua_State *);
int __real_luaW_trycollectenter(lua_State *);
void __real_luaW_heapleave(lua_State *);
void __real_luaQ_wake(pthread_cond_t *, LuaWaiter **);
void __real_luaC_step(lua_State *);
void __real_luaC_fullgc(lua_State *, int);
void __real_luaC_finishtrace(lua_State *);
void __real_luaC_syncbarrier(lua_State *, GCObject *, GCObject *);
lu_mem __real_luaC_sweepbatch(lua_State *, unsigned);
void __real_luaC_scansweep(lua_State *, LuaSweepWork *);
void __real_luaC_pollsweep(lua_State *);
void __real_luaC_assistsweep(lua_State *);
void __real_luaC_publish(lua_State *);
void __real_luaC_drainbarriers(lua_State *);
void __real_luaC_reclaim(lua_State *);
void __real_luaC_assistreclaim(lua_State *);
lu_mem __real_luaC_tracebatch(lua_State *, unsigned);
void __real_luaC_scantrace(lua_State *, LuaTraceWork *, unsigned);
void __real_luaC_assisttrace(lua_State *);
void __real_luaC_polltrace(lua_State *);
void __real_luaC_snapshot(lua_State *, lua_State *);
void __real_luaC_checksnapshot(lua_State *);
void __real_luaW_waitcollector(lua_State *);
void __real_luaM_reclaimpages(lua_State *);
void __real_luaQ_wait(lua_State *, pthread_mutex_t *, pthread_cond_t *,
                      LuaWaiter **);

void __wrap_luaW_stop(lua_State *L) {
  GcProbe *record = probe(L);
  uint64_t started;
  if (record == NULL || L->world_depth != 0) {
    __real_luaW_stop(L);
    return;
  }
  started = now();
  coordination_begin(record);
  record->stop_requests++;
  if (record->reason == REASON_GC)
    record->stop_gc++;
  else if (record->reason == REASON_BARRIER)
    record->stop_barrier++;
  else
    record->stop_other++;
  __real_luaW_stop(L);
  coordination_end(record);
  record->stop_request_ns += now() - started;
}

void __wrap_luaW_start(lua_State *L) {
  GcProbe *record = probe(L);
  uint64_t started;
  if (record == NULL || L->world_depth != 1) {
    __real_luaW_start(L);
    return;
  }
  started = now();
  coordination_begin(record);
  __real_luaW_start(L);
  coordination_end(record);
  record->resume_ns += now() - started;
}

static void heapenter(lua_State *L, void (*enter)(lua_State *)) {
  GcProbe *record = probe(L);
  uint64_t waits = 0, wait_ns = 0;
  int collector = 0;
  if (record != NULL) {
    waits = record->metadata_waits;
    wait_ns = record->metadata_wait_ns;
    collector = record->gc_depth != 0 || enter == __real_luaW_collectenter;
    record->heap_entries++;
    record->heap_gc_entries += collector;
    coordination_begin(record);
  }
  enter(L);
  if (record != NULL) {
    coordination_end(record);
    record->heap_contended_entries += record->metadata_waits != waits;
    if (collector)
      record->heap_gc_wait_ns += record->metadata_wait_ns - wait_ns;
  }
}
void __wrap_luaW_heapenter(lua_State *L) {
  heapenter(L, __real_luaW_heapenter);
}
void __wrap_luaW_collectenter(lua_State *L) {
  heapenter(L, __real_luaW_collectenter);
}
int __wrap_luaW_trycollectenter(lua_State *L) {
  GcProbe *record = probe(L);
  int acquired;
  if (record != NULL)
    coordination_begin(record);
  acquired = __real_luaW_trycollectenter(L);
  if (record != NULL) {
    coordination_end(record);
    record->heap_entries += acquired;
    record->heap_gc_entries += acquired;
    record->heap_deferred_entries += !acquired;
  }
  return acquired;
}
void __wrap_luaW_heapleave(lua_State *L) {
  GcProbe *record = probe(L);
  if (record != NULL)
    coordination_begin(record);
  __real_luaW_heapleave(L);
  if (record != NULL)
    coordination_end(record);
}

void __wrap_luaQ_wake(pthread_cond_t *changed, LuaWaiter **waiters) {
  if (host != NULL) {
    struct metadata_wait *wait;
    luaX_checksync(pthread_mutex_lock(&metadata_probe_mutex));
    for (wait = metadata_waiters; wait != NULL; wait = wait->next) {
      /* Identity matches only this metadata waiter's condition registration.
       * The runtime has assigned its owner before notifying that condition. */
      if (wait->waiters == waiters &&
          wait->owner == __atomic_load_n(&wait->world->heap_owner.token,
                                         __ATOMIC_ACQUIRE) &&
          wait->granted == 0) {
        wait->granted = now();
        break;
      }
    }
    luaX_checksync(pthread_mutex_unlock(&metadata_probe_mutex));
  }
  __real_luaQ_wake(changed, waiters);
}

static void collect(lua_State *L, int operation, int emergency) {
  GcProbe *record = probe(L);
  uint64_t started = 0, excluded = 0, elapsed;
  unsigned reason = REASON_OTHER;
  unsigned previous_operation = 0;
  int outer = 0, running = 0;
  if (record != NULL) {
    previous_operation = record->operation;
    record->operation = operation;
    outer = record->gc_depth++ == 0;
    reason = record->reason;
    record->reason = REASON_GC;
    if (outer) {
      started = now();
      excluded = record->coordination_ns + record->reclaim_ns;
      running = operation == COLLECT_FULL || gcrunning(G(L)) != 0;
      record->collector_calls++;
      record->collector_running_calls += running;
    }
  }
  if (operation == COLLECT_FULL)
    __real_luaC_fullgc(L, emergency);
  else if (operation == COLLECT_STEP)
    __real_luaC_step(L);
  else if (operation == COLLECT_ASSIST)
    __real_luaC_assisttrace(L);
  else if (operation == COLLECT_POLL)
    __real_luaC_polltrace(L);
  else if (operation == COLLECT_SWEEP_ASSIST)
    __real_luaC_assistsweep(L);
  else
    __real_luaC_pollsweep(L);
  if (record != NULL) {
    if (outer) {
      elapsed = now() - started;
      excluded = record->coordination_ns + record->reclaim_ns - excluded;
      if (excluded > elapsed)
        abort();
      if (running)
        record->collector_body_ns += elapsed - excluded;
      else
        record->collector_disabled_ns += elapsed - excluded;
    }
    record->operation = previous_operation;
    record->reason = reason;
    record->gc_depth--;
  }
}
void __wrap_luaC_step(lua_State *L) { collect(L, COLLECT_STEP, 0); }
void __wrap_luaC_fullgc(lua_State *L, int emergency) {
  collect(L, COLLECT_FULL, emergency);
}

void __wrap_luaC_finishtrace(lua_State *L) {
  GcProbe *record = probe(L);
  if (record != NULL)
    record->trace_cycles++;
  __real_luaC_finishtrace(L);
}

void __wrap_luaC_syncbarrier(lua_State *L, GCObject *owner, GCObject *value) {
  GcProbe *record = probe(L);
  unsigned previous = REASON_OTHER;
  if (record != NULL) {
    previous = record->reason;
    record->reason = REASON_BARRIER;
  }
  __real_luaC_syncbarrier(L, owner, value);
  if (record != NULL)
    record->reason = previous;
}

void __wrap_luaC_publish(lua_State *L) {
  GcProbe *record = probe(L);
  uint64_t started = record != NULL ? now() : 0;
  /* World stop invokes publication after every mutator has withdrawn. The
   * first snapshot check on resume ends this interval, before root work. */
  if (record != NULL && L->world_depth == 1 && !record->stopped_start)
    record->stopped_start = started;
  __real_luaC_publish(L);
  if (record != NULL) {
    /* Publication owns quiescence, so these scalar controller observations
     * cannot combine values from two different cycles. */
    if (L->world_depth != 0) {
      LuaGCPacer *pacer = &G(L)->pacer;
      if (record->pacer_parallelism_peak < pacer->parallelism)
        record->pacer_parallelism_peak = pacer->parallelism;
      if (pacer->goal > 0 &&
          record->pacer_goal_peak_bytes < (uint64_t)pacer->goal)
        record->pacer_goal_peak_bytes = (uint64_t)pacer->goal;
      if (pacer->hard > 0 &&
          record->pacer_hard_peak_bytes < (uint64_t)pacer->hard)
        record->pacer_hard_peak_bytes = (uint64_t)pacer->hard;
    }
    record->publish_calls++;
    record->publish_ns += now() - started;
  }
}

void __wrap_luaC_drainbarriers(lua_State *L) {
  GcProbe *record = probe(L);
  uint64_t started;
  if (record != NULL) {
    unsigned shard,
        ready = __atomic_load_n(&G(L)->barrier_ready, __ATOMIC_ACQUIRE);
    started = now();
    for (shard = 0; shard < LUA_BARRIER_SHARDS; shard++) {
      LuaBarrierQueue *queue = &G(L)->barriers[shard];
      GCObject *object;
      if (!(ready & (1u << shard)))
        continue;
      for (object = __atomic_load_n(&queue->forward, __ATOMIC_ACQUIRE);
           object != NULL; object = luaC_barriernext(object))
        record->barrier_records++;
      for (object = __atomic_load_n(&queue->back, __ATOMIC_ACQUIRE);
           object != NULL; object = luaC_barriernext(object)) {
        record->barrier_records++;
        record->back_records++;
      }
    }
    record->inspection_ns += now() - started;
  }
  started = record != NULL ? now() : 0;
  __real_luaC_drainbarriers(L);
  if (record != NULL) {
    record->drain_calls++;
    record->drain_ns += now() - started;
  }
}

void __wrap_luaM_reclaimpages(lua_State *L) {
  GcProbe *record = probe(L);
  uint64_t started = record != NULL ? now() : 0;
  uint64_t excluded = record != NULL ? record->coordination_ns : 0;
  __real_luaM_reclaimpages(L);
  if (record != NULL) {
    uint64_t elapsed = now() - started;
    excluded = record->coordination_ns - excluded;
    if (excluded > elapsed)
      abort();
    record->reclaim_calls++;
    /* Page jobs now rendezvous and yield internally. Their coordination is
     * already accounted separately; an outer collector must exclude it once. */
    record->reclaim_ns += elapsed - excluded;
  }
}

static void reclaim(lua_State *L, int assist) {
  GcProbe *record = probe(L);
  uint64_t started = 0, excluded = 0;
  int outside = 0;
  int objects = luaC_retired(L) != NULL ||
                (assist && __atomic_load_n(&G(L)->world.reclaimers,
                                           __ATOMIC_ACQUIRE) != NULL);
  unsigned reason = REASON_OTHER;
  if (record != NULL) {
    started = now();
    excluded = record->coordination_ns + record->reclaim_ns;
    outside = record->gc_depth == 0;
    record->gc_depth++;
    reason = record->reason;
    record->reason = REASON_GC;
  }
  if (assist)
    __real_luaC_assistreclaim(L);
  else
    __real_luaC_reclaim(L);
  if (record != NULL) {
    uint64_t elapsed = now() - started;
    excluded = record->coordination_ns + record->reclaim_ns - excluded;
    if (excluded > elapsed)
      abort();
    if (objects) {
      record->object_reclaim_calls++;
      record->reclaim_assist_calls += assist;
      record->object_reclaim_ns += elapsed - excluded;
      /* API exits may destroy objects after the collecting call returns. */
      if (outside)
        record->collector_body_ns += elapsed - excluded;
    } else {
      /* Explicit table-buffer returns can leave only pool-page cleanup here.
       * Include its dispatch overhead with page work, not collector body. */
      record->reclaim_ns += elapsed - excluded;
    }
    record->gc_depth--;
    record->reason = reason;
  }
}

void __wrap_luaC_reclaim(lua_State *L) { reclaim(L, 0); }
void __wrap_luaC_assistreclaim(lua_State *L) { reclaim(L, 1); }

lu_mem __wrap_luaC_tracebatch(lua_State *L, unsigned limit) {
  GcProbe *record = probe(L);
  unsigned phase = luaC_tracephase(G(L));
  unsigned busy = __atomic_load_n(&G(L)->trace_busy, __ATOMIC_RELAXED);
  unsigned ready = __atomic_load_n(&G(L)->trace_ready, __ATOMIC_ACQUIRE);
  int pressure = luaC_cyclepressure(L);
  void *metadata =
      __atomic_load_n(&G(L)->world.heap_owner.token, __ATOMIC_ACQUIRE);
  uint64_t started = record != NULL ? now() : 0;
  lu_mem marked = __real_luaC_tracebatch(L, limit);
  if (record != NULL) {
    uint64_t elapsed = now() - started;
    record->trace_calls++;
    record->worker_trace_calls += record == worker_probe;
    record->trace_ns += elapsed;
    record->trace_work_bytes += marked;
    record->trace_zero_progress_batches += marked == 0;
    if (marked == 0) {
      record->trace_empty_soft += !pressure;
      record->trace_empty_busy += busy != 0;
      record->trace_empty_ready += ready != 0;
      record->trace_empty_metadata +=
          metadata != NULL && metadata != luaX_token(L);
      record->trace_empty_quiescing += luaW_stopped(&G(L)->world);
      if (phase == TRACE_INIT)
        record->trace_empty_init++;
      else if (phase == TRACE_MARK)
        record->trace_empty_mark++;
      else
        record->trace_empty_weak++;
      switch (record->operation) {
      case COLLECT_ASSIST:
        record->trace_empty_assist++;
        break;
      case COLLECT_POLL:
        record->trace_empty_poll++;
        break;
      case COLLECT_FULL:
        record->trace_empty_full++;
        break;
      default:
        record->trace_empty_step++;
        break;
      }
    }
    if (!record->gc_depth)
      record->collector_body_ns += elapsed;
  }
  return marked;
}

void __wrap_luaC_scantrace(lua_State *L, LuaTraceWork *work, unsigned limit) {
  GcProbe *record = probe(L);
  uint64_t started = record != NULL ? now() : 0;
  unsigned active = __atomic_add_fetch(&scanners, 1, __ATOMIC_ACQ_REL);
  if (record != NULL && record->trace_peak_workers < active)
    record->trace_peak_workers = active;
  __real_luaC_scantrace(L, work, limit);
  __atomic_fetch_sub(&scanners, 1, __ATOMIC_ACQ_REL);
  if (record != NULL) {
    GCObject *object;
    record->trace_scan_ns += now() - started;
    /* These lists are still private to this admitted batch. Attribute failed
     * payload reservations before publication makes them available again. */
    for (object = work->deferred.head; object != NULL;
         object = *luaC_graylink(object)) {
      switch (object->tt) {
      case LUA_VTABLE:
        record->trace_retry_tables++;
        break;
      case LUA_VTHREAD:
        record->trace_retry_threads++;
        break;
      case LUA_VPROTO:
        record->trace_retry_protos++;
        break;
      case LUA_VUPVAL:
        record->trace_retry_upvalues++;
        break;
      case LUA_VLCL:
      case LUA_VCCL:
        record->trace_retry_closures++;
        break;
      case LUA_VUSERDATA:
        record->trace_retry_userdata++;
        break;
      default:
        abort();
      }
    }
    for (object = work->retry; object != NULL; object = gco2t(object)->weaknext)
      record->trace_retry_tables++;
  }
}

void __wrap_luaC_assisttrace(lua_State *L) {
  GcProbe *record = probe(L);
  if (record != NULL)
    record->trace_assist_calls++;
  collect(L, COLLECT_ASSIST, 0);
}

void __wrap_luaC_polltrace(lua_State *L) {
  GcProbe *record = probe(L);
  if (record != NULL)
    record->trace_poll_calls++;
  collect(L, COLLECT_POLL, 0);
}

lu_mem __wrap_luaC_sweepbatch(lua_State *L, unsigned limit) {
  GcProbe *record = probe(L);
  uint64_t started = record != NULL ? now() : 0;
  lu_mem worked = __real_luaC_sweepbatch(L, limit);
  if (record != NULL) {
    record->sweep_calls++;
    record->sweep_ns += now() - started;
  }
  return worked;
}

lu_mem __real_luaC_sweeppayload(lua_State *, unsigned);
lu_mem __wrap_luaC_sweeppayload(lua_State *L, unsigned limit) {
  GcProbe *record = probe(L);
  uint64_t started = record != NULL ? now() : 0;
  lu_mem worked = __real_luaC_sweeppayload(L, limit);
  if (record != NULL) {
    record->sweep_calls++;
    record->worker_sweep_calls++;
    record->sweep_ns += now() - started;
  }
  return worked;
}

void __wrap_luaC_scansweep(lua_State *L, LuaSweepWork *work) {
  GcProbe *record = probe(L);
  uint64_t started = record != NULL ? now() : 0;
  unsigned active = __atomic_add_fetch(&sweepers, 1, __ATOMIC_ACQ_REL);
  if (record != NULL) {
    if (record->sweep_peak_workers < active)
      record->sweep_peak_workers = active;
    record->sweep_records += work->count;
    if (L->world_depth != 0)
      record->sweep_paused_records += work->count;
  }
  __real_luaC_scansweep(L, work);
  __atomic_fetch_sub(&sweepers, 1, __ATOMIC_ACQ_REL);
  if (record != NULL)
    record->sweep_scan_ns += now() - started;
}

void __wrap_luaC_pollsweep(lua_State *L) {
  GcProbe *record = probe(L);
  if (record != NULL)
    record->sweep_poll_calls++;
  collect(L, COLLECT_SWEEP, 0);
}
void __wrap_luaC_assistsweep(lua_State *L) {
  GcProbe *record = probe(L);
  if (record != NULL)
    record->sweep_assist_calls++;
  collect(L, COLLECT_SWEEP_ASSIST, 0);
}

static unsigned snapshot_needed(lua_State *L, lua_State *thread) {
  return luaC_tracing(L) &&
         __atomic_load_n(&thread->trace_snapshot, __ATOMIC_ACQUIRE) !=
             G(L)->trace_epoch;
}

static void snapshot(lua_State *L, lua_State *thread) {
  GcProbe *record = probe(L);
  uint64_t started = 0;
  unsigned count = 0;
  int outer = 0;
  if (record != NULL) {
    outer = record->root_depth++ == 0;
    if (thread == NULL && record->stopped_start && L->world_depth == 0) {
      record->stopped_ns += now() - record->stopped_start;
      record->stopped_start = 0;
    }
    if (outer) {
      count = snapshot_needed(L, thread != NULL ? thread : L);
      if (thread == NULL && L->stack_peer != NULL)
        count += snapshot_needed(L, L->stack_peer);
      if (count)
        started = now();
    }
  }
  if (thread != NULL)
    __real_luaC_snapshot(L, thread);
  else
    __real_luaC_checksnapshot(L);
  if (record != NULL) {
    if (outer && count) {
      record->root_snapshot_calls += count;
      record->root_snapshot_ns += now() - started;
    }
    record->root_depth--;
  }
}

void __wrap_luaC_snapshot(lua_State *L, lua_State *thread) {
  snapshot(L, thread);
}
void __wrap_luaC_checksnapshot(lua_State *L) { snapshot(L, NULL); }

void __wrap_luaW_waitcollector(lua_State *L) {
  GcProbe *record = probe(L);
  uint64_t started = record != NULL ? now() : 0;
  int tracing = luaC_tracing(L);
  if (record != NULL)
    coordination_begin(record);
  __real_luaW_waitcollector(L);
  if (record != NULL) {
    coordination_end(record);
    if (tracing) {
      record->trace_waits++;
      record->trace_wait_ns += now() - started;
    } else {
      record->reclaim_waits++;
      record->reclaim_wait_ns += now() - started;
    }
  }
}

void __wrap_luaQ_wait(lua_State *L, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters) {
  GcProbe *record = probe(L);
  struct metadata_wait pending = {0};
  uint64_t started = 0;
  int category = 0;
  if (record != NULL && mutex == &G(L)->allocator.mutex) {
    started = now();
    category = 4;
  } else if (record != NULL && mutex == &G(L)->world.mutex) {
    started = now();
    if (luaW_stopped(&G(L)->world) && luaQ_isowner(&G(L)->world.collector))
      category = 1;
    else if (L->heap_waiting)
      category = 2;
    else if (luaW_stopped(&G(L)->world))
      category = 3;
  }
  if (category == 2 && host != NULL && G(L) == G(host)) {
    pending.waiters = waiters;
    pending.world = &G(L)->world;
    pending.owner = luaX_token(L);
    luaX_checksync(pthread_mutex_lock(&metadata_probe_mutex));
    pending.next = metadata_waiters;
    metadata_waiters = &pending;
    luaX_checksync(pthread_mutex_unlock(&metadata_probe_mutex));
  }
  __real_luaQ_wait(L, mutex, changed, waiters);
  if (pending.waiters != NULL) {
    struct metadata_wait **link;
    luaX_checksync(pthread_mutex_lock(&metadata_probe_mutex));
    link = &metadata_waiters;
    while (*link != &pending) {
      if (*link == NULL)
        abort();
      link = &(*link)->next;
    }
    *link = pending.next;
    luaX_checksync(pthread_mutex_unlock(&metadata_probe_mutex));
    if (pending.granted != 0) {
      uint64_t elapsed = now() - pending.granted;
      record->metadata_handoffs++;
      record->metadata_handoff_ns += elapsed;
      if (record->metadata_handoff_max_ns < elapsed)
        record->metadata_handoff_max_ns = elapsed;
    }
  }
  if (category == 1) {
    record->quiesce_waits++;
    record->quiesce_wait_ns += now() - started;
  } else if (category == 2) {
    record->metadata_waits++;
    record->metadata_wait_ns += now() - started;
  } else if (category == 3) {
    record->mutator_stop_waits++;
    record->mutator_stop_wait_ns += now() - started;
  } else if (category == 4) {
    record->allocator_waits++;
    record->allocator_wait_ns += now() - started;
  }
}
