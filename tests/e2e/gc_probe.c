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
}

void gc_probe_emit(const GcProbe *record) {
#define GC_PROBE_EMIT(name) printf(",\"gc_" #name "\":%" PRIu64, record->name);
  GC_PROBE_COUNTERS(GC_PROBE_EMIT)
#undef GC_PROBE_EMIT
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
void __real_luaW_heapleave(lua_State *);
void __real_luaC_step(lua_State *);
void __real_luaC_fullgc(lua_State *, int);
void __real_luaC_syncbarrier(lua_State *, GCObject *, GCObject *, int);
void __real_luaC_publish(lua_State *);
void __real_luaC_drainbarriers(lua_State *);
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

void __wrap_luaW_heapenter(lua_State *L) {
  GcProbe *record = probe(L);
  uint64_t waits = 0, wait_ns = 0;
  int collector = 0;
  if (record != NULL) {
    waits = record->metadata_waits;
    wait_ns = record->metadata_wait_ns;
    collector = record->gc_depth != 0;
    record->heap_entries++;
    record->heap_gc_entries += collector;
    coordination_begin(record);
  }
  __real_luaW_heapenter(L);
  if (record != NULL) {
    coordination_end(record);
    record->heap_contended_entries += record->metadata_waits != waits;
    if (collector)
      record->heap_gc_wait_ns += record->metadata_wait_ns - wait_ns;
  }
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
      for (object = participant->thread->barrier_forward; object != NULL;
           object = luaC_barriernext(object))
        record->barrier_records++;
      for (object = participant->thread->barrier_back; object != NULL;
           object = luaC_barriernext(object)) {
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
