#define _POSIX_C_SOURCE 200809L
#include "gc_probe.h"
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

enum { TRANSFERS = 32 };
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *owner;
  unsigned ready;
  int collect;
};

static uint64_t now(void) {
  struct timespec stamp;
  require(clock_gettime(CLOCK_MONOTONIC, &stamp) == 0);
  return (uint64_t)stamp.tv_sec * 1000000000 + (uint64_t)stamp.tv_nsec;
}

static void *release_to_waiter(void *context) {
  struct fixture *fixture = context;
  lua_State *state = fixture->owner;
  LuaWorld *world = &G(state)->world;
  unsigned round;
  luaX_lockstate(state);
  for (round = 1; round <= TRANSFERS; round++) {
    uint64_t deadline = now() + UINT64_C(5000000000);
    int waiting;
    luaW_heapenter(state);
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->ready = round;
    require(pthread_cond_broadcast(&fixture->changed) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
    /* Queue the measured caller before releasing, regardless of thread order.
     */
    do {
      require(pthread_mutex_lock(&world->mutex) == 0);
      waiting = world->heap_head != NULL;
      require(pthread_mutex_unlock(&world->mutex) == 0);
      require(now() < deadline);
      if (!waiting)
        sched_yield();
    } while (!waiting);
    if (fixture->collect) {
      /* Match the collector's nested admission: the final release is inside
       * luaW_start's own source file, invisible to a heapleave-only wrapper. */
      luaW_stop(state);
      luaW_heapleave(state);
      luaW_start(state);
    } else
      luaW_heapleave(state);
  }
  luaX_unlockstate(state);
  return NULL;
}

static void transfer(lua_State *state, struct fixture *fixture) {
  pthread_t worker;
  unsigned round;
  fixture->ready = 0;
  require(pthread_create(&worker, NULL, release_to_waiter, fixture) == 0);
  luaX_lockstate(state);
  for (round = 1; round <= TRANSFERS; round++) {
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    while (fixture->ready < round)
      require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
    luaW_heapenter(state);
    luaW_heapleave(state);
  }
  luaX_unlockstate(state);
  require(pthread_join(worker, NULL) == 0);
}

static void *notify_minor(void *context) {
  struct fixture *fixture = context;
  global_State *g = G(fixture->owner);
  require(pthread_mutex_lock(&g->world.mutex) == 0);
  require(g->minor_waiters != NULL);
  fixture->ready = 1;
  luaQ_wake(&g->phase_changed, &g->minor_waiters);
  require(pthread_mutex_unlock(&g->world.mutex) == 0);
  return NULL;
}

static void minor_notification(lua_State *state, struct fixture *fixture,
                               GcProbe *record) {
  global_State *g = G(state);
  pthread_t worker;
  fixture->ready = 0;
  record->enabled = 1;
  require(pthread_mutex_lock(&g->world.mutex) == 0);
  require(pthread_create(&worker, NULL, notify_minor, fixture) == 0);
  while (!fixture->ready)
    luaQ_wait(state, &g->world.mutex, &g->phase_changed, &g->minor_waiters);
  require(pthread_mutex_unlock(&g->world.mutex) == 0);
  require(pthread_join(worker, NULL) == 0);
  record->enabled = 0;
  require(record->minor_woken == 1 && record->minor_wake_batches == 1);
  require(record->minor_wake_ns == record->minor_during_wake_ns);
  require(record->minor_fiber_woken == 0 && record->minor_dispatch_ns == 0 &&
          record->minor_restore_ns == 0);
}

static void snapshot_while_arming(lua_State *state, GcProbe *record) {
  global_State *g = G(state);
  uint64_t control, epoch, calls = record->root_snapshot_calls;
  uint64_t elapsed = record->root_snapshot_ns;
  uint64_t requests = record->root_snapshot_requests;
  luaX_lockstate(state);
  control = luaW_control(&g->world);
  epoch = luaC_traceepoch(g);
  require(!(control & LUA_MARK_ACTIVE));
  /* The new epoch can be visible before the collector can scan any stack. */
  __atomic_store_n(&g->trace_epoch, epoch + 1, __ATOMIC_RELEASE);
  __atomic_store_n(&g->world.control,
                   control | LUA_MARK_ACTIVE | LUA_MARK_ARMING,
                   __ATOMIC_SEQ_CST);
  record->enabled = 1;
  require(luaC_checksnapshot(state) == 0);
  record->enabled = 0;
  __atomic_store_n(&g->world.control, control, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g->trace_epoch, epoch, __ATOMIC_RELEASE);
  luaX_unlockstate(state);
  require(record->root_snapshot_requests == requests + 1 &&
          record->root_snapshot_calls == calls &&
          record->root_snapshot_ns == elapsed);
}

static unsigned finalized;

static int finalize(lua_State *state) {
  require(lua_type(state, 1) == LUA_TUSERDATA);
  finalized++;
  return 0;
}

static void sweep_finalizer(lua_State *state, GcProbe *record) {
  lua_gc(state, LUA_GCGEN);
  lua_gc(state, LUA_GCSTOP);
  lua_gc(state, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  lua_newuserdatauv(state, 1, 0);
  lua_newtable(state);
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  lua_setmetatable(state, -2);
  lua_pop(state, 1);
  record->enabled = 1;
  lua_gc(state, LUA_GCSTEP, (size_t)0);
  record->enabled = 0;
  require(finalized == 1 && record->sweep_finalizer_calls == 1);
  require(record->sweep_complete_calls != 0 &&
          record->sweep_empty_calls <= record->sweep_calls);
  require(record->sweep_finalizer_ns > 0 &&
          record->sweep_finalizer_ns + record->sweep_scan_ns <=
              record->sweep_ns);
}

int main(void) {
  lua_State *state = luaL_newstate();
  struct fixture fixture = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
                            NULL, 0, 0};
  GcProbe record = {0};
  uint64_t handoff_ns, wait_ns;
  require(state != NULL);
  lua_gc(state, LUA_GCSTOP);
  fixture.owner = lua_newthread(state);
  gc_probe_bind(state, &record);
  record.enabled = 1;
  luaX_lockstate(state);
  luaW_heapenter(state);
  luaW_heapleave(state);
  luaX_unlockstate(state);
  require(record.metadata_handoffs == 0 && record.metadata_waits == 0);
  transfer(state, &fixture);
  require(record.metadata_handoffs == TRANSFERS);
  fixture.collect = 1;
  transfer(state, &fixture);
  record.enabled = 0;
  require(record.metadata_handoffs == 2 * TRANSFERS);
  require(record.metadata_handoffs <= record.metadata_waits);
  require(record.metadata_handoff_ns > 0);
  require(record.metadata_handoff_ns <= record.metadata_wait_ns);
  require(record.metadata_handoff_max_ns > 0 &&
          record.metadata_handoff_max_ns <= record.metadata_handoff_ns);
  handoff_ns = record.metadata_handoff_ns;
  wait_ns = record.metadata_wait_ns;
  transfer(state, &fixture);
  require(record.metadata_handoffs == 2 * TRANSFERS &&
          record.metadata_handoff_ns == handoff_ns &&
          record.metadata_wait_ns == wait_ns);
  minor_notification(state, &fixture, &record);
  snapshot_while_arming(state, &record);
  sweep_finalizer(state, &record);
  gc_probe_bind(NULL, NULL);
  lua_close(state);
  require(pthread_cond_destroy(&fixture.changed) == 0);
  require(pthread_mutex_destroy(&fixture.mutex) == 0);
  return 0;
}
