#define _POSIX_C_SOURCE 200809L
#include "gc_probe.h"
#include "../../src/policy.h"
#include <inttypes.h>
#include <lgc.h>
#include <lmem.h>
#include <lstate.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { REASON_OTHER, REASON_GC, REASON_BARRIER };
static lua_State *host;
static GcProbe *host_probe;
static unsigned scanners;

static uint64_t now(void) {
  struct timespec stamp;
  if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0)
    abort();
  return (uint64_t)stamp.tv_sec * 1000000000 + (uint64_t)stamp.tv_nsec;
}

static GcProbe *probe(lua_State *L) {
  /* Ownership already exists here. The public context getter would reenter
   * the stack gate. Only this executable installs these invocation contexts. */
  GcProbe *result =
      L == host ? host_probe : luthr_policy_get_context(L->policy);
  return result != NULL && result->enabled ? result : NULL;
}

void gc_probe_bind(lua_State *L, GcProbe *record) {
  /* Called before carriers start; immutable until they have been joined. */
  host = L;
  host_probe = record;
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
  if (luaC_tracing(host))
    peak = &record->mark_peak_bytes;
  else if (g->gcstate >= GCSswpallgc && g->gcstate <= GCSswpend)
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
void __real_luaW_heapleave(lua_State *);
void __real_luaC_step(lua_State *);
void __real_luaC_fullgc(lua_State *, int);
void __real_luaC_finishtrace(lua_State *);
void __real_luaC_syncbarrier(lua_State *, GCObject *, GCObject *, int);
void __real_luaC_publish(lua_State *);
void __real_luaC_drainbarriers(lua_State *);
void __real_luaC_reclaim(lua_State *);
void __real_luaC_assistreclaim(lua_State *);
lu_mem __real_luaC_tracebatch(lua_State *, unsigned);
void __real_luaC_scantrace(lua_State *, LuaTraceWork *, unsigned);
void __real_luaC_assisttrace(lua_State *);
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
void __wrap_luaW_heapleave(lua_State *L) {
  GcProbe *record = probe(L);
  if (record != NULL)
    coordination_begin(record);
  __real_luaW_heapleave(L);
  if (record != NULL)
    coordination_end(record);
}

static void collect(lua_State *L, int full, int emergency) {
  GcProbe *record = probe(L);
  uint64_t started = 0, excluded = 0, elapsed;
  unsigned reason = REASON_OTHER;
  int outer = 0, running = 0;
  if (record != NULL) {
    outer = record->gc_depth++ == 0;
    reason = record->reason;
    record->reason = REASON_GC;
    if (outer) {
      started = now();
      excluded = record->coordination_ns + record->reclaim_ns;
      running = full || gcrunning(G(L)) != 0;
      record->collector_calls++;
      record->collector_running_calls += running;
    }
  }
  if (full)
    __real_luaC_fullgc(L, emergency);
  else
    __real_luaC_step(L);
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
    record->reason = reason;
    record->gc_depth--;
  }
}
void __wrap_luaC_step(lua_State *L) { collect(L, 0, 0); }
void __wrap_luaC_fullgc(lua_State *L, int emergency) {
  collect(L, 1, emergency);
}

void __wrap_luaC_finishtrace(lua_State *L) {
  GcProbe *record = probe(L);
  if (record != NULL)
    record->trace_cycles++;
  __real_luaC_finishtrace(L);
}

void __wrap_luaC_syncbarrier(lua_State *L, GCObject *owner, GCObject *value,
                             int back) {
  GcProbe *record = probe(L);
  unsigned previous = REASON_OTHER;
  if (record != NULL) {
    previous = record->reason;
    record->reason = REASON_BARRIER;
  }
  __real_luaC_syncbarrier(L, owner, value, back);
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
    record->publish_calls++;
    record->publish_ns += now() - started;
  }
}

void __wrap_luaC_drainbarriers(lua_State *L) {
  GcProbe *record = probe(L);
  uint64_t started;
  if (record != NULL) {
    LuaParticipation *participant;
    started = now();
    for (participant = G(L)->world.participants; participant != NULL;
         participant = participant->next) {
      GCObject *object;
      for (object = __atomic_load_n(&participant->thread->barrier_forward,
                                    __ATOMIC_ACQUIRE);
           object != NULL; object = luaC_barriernext(object))
        record->barrier_records++;
      for (object = __atomic_load_n(&participant->thread->barrier_back,
                                    __ATOMIC_ACQUIRE);
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
  __real_luaM_reclaimpages(L);
  if (record != NULL) {
    record->reclaim_calls++;
    record->reclaim_ns += now() - started;
  }
}

static void reclaim(lua_State *L, int assist) {
  GcProbe *record = probe(L);
  uint64_t started = 0, excluded = 0;
  int outside = 0;
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
    record->object_reclaim_calls++;
    record->reclaim_assist_calls += assist;
    record->object_reclaim_ns += elapsed - excluded;
    /* Automatic steps include destruction in their body. API/exit boundaries
     * may destroy after the collecting call returns; count that work as well.
     */
    if (outside)
      record->collector_body_ns += elapsed - excluded;
    record->gc_depth--;
    record->reason = reason;
  }
}

void __wrap_luaC_reclaim(lua_State *L) { reclaim(L, 0); }
void __wrap_luaC_assistreclaim(lua_State *L) { reclaim(L, 1); }

lu_mem __wrap_luaC_tracebatch(lua_State *L, unsigned limit) {
  GcProbe *record = probe(L);
  uint64_t started = record != NULL ? now() : 0;
  lu_mem marked = __real_luaC_tracebatch(L, limit);
  if (record != NULL) {
    uint64_t elapsed = now() - started;
    record->trace_calls++;
    record->trace_ns += elapsed;
    record->trace_work_bytes += marked;
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
  if (record != NULL)
    record->trace_scan_ns += now() - started;
}

void __wrap_luaC_assisttrace(lua_State *L) {
  GcProbe *record = probe(L);
  if (record != NULL)
    record->trace_assist_calls++;
  __real_luaC_assisttrace(L);
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
  uint64_t started = 0;
  int category = 0;
  if (record != NULL && mutex == &G(L)->world.mutex) {
    started = now();
    if (G(L)->world.stopped && luaQ_isowner(&G(L)->world.collector))
      category = 1;
    else if (L->heap_waiting)
      category = 2;
    else if (G(L)->world.stopped)
      category = 3;
  }
  __real_luaQ_wait(L, mutex, changed, waiters);
  if (category == 1) {
    record->quiesce_waits++;
    record->quiesce_wait_ns += now() - started;
  } else if (category == 2) {
    record->metadata_waits++;
    record->metadata_wait_ns += now() - started;
  } else if (category == 3) {
    record->mutator_stop_waits++;
    record->mutator_stop_wait_ns += now() - started;
  }
}
