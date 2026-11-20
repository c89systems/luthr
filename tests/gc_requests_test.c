#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  unsigned arrived[2], done;
} fixture = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {0, 0}, 0};
static __thread unsigned role;
static __thread unsigned stops;
static void arrive(void);

void __real_luaW_stop(lua_State *);
void __wrap_luaW_stop(lua_State *L) {
  if (role != 0) {
    stops++;
    arrive();
  }
  __real_luaW_stop(L);
}

void __real_luaW_heapenter(lua_State *);
void __real_luaW_collectenter(lua_State *);
int __real_luaW_trycollectenter(lua_State *);
static void arrive(void) {
  if (role != 0) {
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    fixture.arrived[role - 1]++;
    require(pthread_cond_broadcast(&fixture.changed) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
  }
}
void __wrap_luaW_heapenter(lua_State *L) {
  arrive();
  __real_luaW_heapenter(L);
}
void __wrap_luaW_collectenter(lua_State *L) {
  arrive();
  __real_luaW_collectenter(L);
}
int __wrap_luaW_trycollectenter(lua_State *L) {
  arrive();
  return __real_luaW_trycollectenter(L);
}

struct request {
  lua_State *state;
  unsigned role, stops;
  int phase;
};
static void *request(void *argument) {
  struct request *r = argument;
  role = r->role;
  if (r->phase == 5) {
    require(lua_gc(r->state, LUA_GCSTEP, (size_t)0) == 0);
    r->stops = stops;
    return NULL;
  }
  lua_lock(r->state);
  if (r->phase == 4) {
    luaC_checkGC(r->state);
  } else if (r->phase == 3)
    luaC_assistsweep(r->state);
  else if (r->phase == 2)
    luaC_assisttrace(r->state);
  else if (r->phase == 1)
    luaC_advancetrace(r->state);
  else
    luaC_step(r->state);
  lua_unlock(r->state);
  r->stops = stops;
  if (role == 2 || r->phase) {
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    fixture.done = 1;
    require(pthread_cond_broadcast(&fixture.changed) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
  }
  return NULL;
}

static void explicit_trace_slice(lua_State *L) {
  struct request helper = {0};
  pthread_t worker;
  unsigned steps;
  lu_mem work;
  lua_gc(L, LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  require(luaL_loadstring(L, "local h; for i=1,20000 do h={h} end; return h") ==
          LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  helper.state = lua_newthread(L);
  helper.role = 1;
  helper.phase = 5;
  require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  lua_lock(L);
  require(luaC_tracing(L));
  work = __atomic_load_n(&G(L)->pacer.work, __ATOMIC_RELAXED);
  /* Payload work must finish while another participant holds metadata. */
  luaW_heapenter(L);
  require(pthread_create(&worker, NULL, request, &helper) == 0);
  require(pthread_join(worker, NULL) == 0);
  require(helper.stops == 0 && luaC_tracing(L));
  require(__atomic_load_n(&G(L)->pacer.work, __ATOMIC_RELAXED) > work);
  luaW_heapleave(L);
  lua_unlock(L);
  for (steps = 0; steps < 1000; steps++)
    if (lua_gc(L, LUA_GCSTEP, (size_t)0) == 1)
      break;
  require(steps < 1000 && lua_gc(L, LUA_GCISRUNNING) == 0);
  require(lua_istable(L, 1));
}

static void phase_request(lua_State *L, int generational) {
  struct request helper = {0};
  pthread_t worker;
  unsigned length = 0;
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_loadstring(L, "local head; for i=1,20000 do "
                             "head={head} end; return head") == LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  helper.state = lua_newthread(L);
  helper.role = helper.phase = 1;
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCRESTART);
  lua_lock(L);
  G(L)->pacer.goal = MAX_LMEM;
  luaE_setdebt(G(L), 0);
  luaC_step(L);
  require(luaC_tracing(L));
  luaW_heapenter(L);
  /* A helper observed an empty shared queue, then tried to acquire metadata. A
   * scanner can publish work before that pending request is admitted. */
  __atomic_store_n(&G(L)->trace_ready, 0, __ATOMIC_RELEASE);
  require(pthread_create(&worker, NULL, request, &helper) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (fixture.arrived[0] == 0)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  while (!fixture.done)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  luaC_tracebatch(L, LUA_TRACE_BATCH);
  require(__atomic_load_n(&G(L)->trace_ready, __ATOMIC_ACQUIRE));
  luaW_heapleave(L);
  lua_unlock(L);
  require(pthread_join(worker, NULL) == 0);
  require(helper.stops == 0); /* A stale request must not pause every worker. */
  lua_gc(L, LUA_GCCOLLECT);
  lua_pushvalue(L, 1);
  while (lua_istable(L, -1)) {
    lua_rawgeti(L, -1, 1);
    lua_remove(L, -2);
    length++;
  }
  require(length == 20000);
}

static void boundary_handoff(lua_State *L, const char *kind) {
  LuaWorld *world = &G(L)->world;
  struct request helpers[2] = {{0}};
  pthread_t workers[2];
  unsigned i, length = 0;
  int sweeping = strstr(kind, "sweep") != NULL;
  lua_gc(L, strstr(kind, "generational") != NULL ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  require(luaL_loadstring(L, "local h; for i=1,10000 do h={h} end; return h") ==
          LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  for (i = 0; i < 2; i++) {
    helpers[i].state = lua_newthread(L);
    helpers[i].role = i + 1;
    helpers[i].phase = sweeping ? 3 : 2;
  }
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCRESTART);
  lua_lock(L);
  G(L)->pacer.goal = MAX_LMEM;
  luaE_setdebt(G(L), 0);
  luaC_step(L);
  require(luaC_tracing(L));
  if (sweeping) {
    luaC_waittrace(L);
    require(luaC_sweeping(L));
  }
  /* Both helpers request the complete phase. */
  __atomic_store_n(&G(L)->pacer.hard, 0, __ATOMIC_RELAXED);
  luaW_heapenter(L);
  for (i = 0; i < 2; i++)
    require(pthread_create(&workers[i], NULL, request, &helpers[i]) == 0);
  require(pthread_mutex_lock(&world->mutex) == 0);
  /* The caller retains metadata and active stack participation. A table
   * sweep must finish before it releases either, while tracing needs both. */
  if (sweeping) {
    while (luaC_sweeping(L))
      luaQ_wait(L, &world->mutex, &G(L)->phase_changed, &G(L)->phase_waiters);
  } else {
    while (!helpers[0].state->heap_waiting || !helpers[1].state->heap_waiting)
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  }
  require(!luaW_stopped(world));
  require(pthread_mutex_unlock(&world->mutex) == 0);
  luaW_heapleave(L);
  lua_unlock(L);
  for (i = 0; i < 2; i++)
    require(pthread_join(workers[i], NULL) == 0);
  if (sweeping)
    require(helpers[0].stops == 0 && helpers[1].stops == 0);
  require(!luaC_tracing(L) && (!sweeping || !luaC_sweeping(L)));
  lua_gc(L, LUA_GCCOLLECT);
  lua_pushvalue(L, 1);
  while (lua_istable(L, -1)) {
    lua_rawgeti(L, -1, 1);
    lua_remove(L, -2);
    length++;
  }
  require(length == 10000);
}

static unsigned string_buckets(lua_State *L) {
  unsigned i, count = 0;
  for (i = 0; i < LUA_STRING_SHARDS; i++)
    count += G(L)->strt[i].size;
  return count;
}

static void sweep_string_index(lua_State *L) {
  unsigned i, grown, shrunk;
  char name[32];
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, 4096, 0);
  for (i = 1; i <= 4096; i++) {
    snprintf(name, sizeof(name), "temporary string %u", i);
    lua_pushstring(L, name);
    lua_rawseti(L, -2, i);
  }
  grown = string_buckets(L);
  require(grown >= 4096);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCRESTART);
  lua_lock(L);
  G(L)->pacer.goal = MAX_LMEM;
  luaE_setdebt(G(L), 0);
  luaC_step(L);
  luaC_waittrace(L);
  require(luaC_sweeping(L));
  luaC_waitsweep(L);
  shrunk = string_buckets(L);
  require(shrunk < grown / 4);
  require(getgcbytes(G(L)) == gettotalbytes(G(L)) - G(L)->GCretired);
  lua_unlock(L);
}

static int callbacks;

static int live_finalizer(lua_State *L) {
  (void)L;
  callbacks++;
  return 0;
}

static void push_finalizable(lua_State *L, int index) {
  lua_newtable(L);
  lua_pushvalue(L, 2);
  require(lua_setmetatable(L, -2));
  lua_rawseti(L, 1, index);
}

static void reachable_finalizers(lua_State *L, int late) {
  int live = late ? 16 : 8 * LUA_TRACE_BATCH;
  struct request helper = {0};
  pthread_t worker;
  int i;
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, live, 0);
  lua_newtable(L);
  lua_pushcfunction(L, live_finalizer);
  lua_setfield(L, 2, "__gc");
  for (i = 1; i <= live; i++)
    push_finalizable(L, i);
  helper.state = lua_newthread(L);
  helper.role = 1;
  helper.phase = 2;
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCRESTART);
  lua_lock(L);
  G(L)->pacer.goal = MAX_LMEM;
  luaE_setdebt(G(L), 0);
  luaC_step(L);
  require(luaC_tracing(L));
  if (late) {
    for (i = 0; G(L)->trace_plan.finalizers != NULL && i < 64; i++)
      luaC_tracebatch(L, LUA_TRACE_BATCH);
    require(G(L)->trace_plan.finalizers == NULL);
  }
  lua_unlock(L);
  if (late)
    push_finalizable(L, ++live);
  require(luaC_tracing(L));
  __atomic_store_n(&G(L)->pacer.hard, 0, __ATOMIC_RELAXED);
  require(pthread_create(&worker, NULL, request, &helper) == 0);
  require(pthread_join(worker, NULL) == 0);
  require(!luaC_tracing(L) && helper.stops == 1 && callbacks == 0);
  lua_remove(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  require(callbacks == live);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  struct request requests[2] = {{0}};
  pthread_t workers[2];
  unsigned round;
  int explicit_step, stop, overdue;
  require(L != NULL && argc == 2);
  if (strcmp(argv[1], "explicit_trace_slice") == 0) {
    explicit_trace_slice(L);
    goto done;
  }
  if (strcmp(argv[1], "reachable_finalizers") == 0 ||
      strcmp(argv[1], "late_finalizer") == 0) {
    reachable_finalizers(L, strcmp(argv[1], "late_finalizer") == 0);
    goto done;
  }
  if (strcmp(argv[1], "shrink_strings") == 0) {
    sweep_string_index(L);
    goto done;
  }
  if (strncmp(argv[1], "handoff_", 8) == 0) {
    boundary_handoff(L, argv[1]);
    goto done;
  }
  if (strncmp(argv[1], "phase_", 6) == 0) {
    require(strcmp(argv[1], "phase_incremental") == 0 ||
            strcmp(argv[1], "phase_generational") == 0);
    phase_request(L, strcmp(argv[1], "phase_generational") == 0);
    goto done;
  }
  explicit_step = strcmp(argv[1], "explicit") == 0;
  stop = strcmp(argv[1], "stopped") == 0;
  overdue = strncmp(argv[1], "overdue_", 8) == 0;
  require(explicit_step || stop || overdue ||
          strcmp(argv[1], "incremental") == 0 ||
          strcmp(argv[1], "generational") == 0);
  requests[0].state = lua_newthread(L);
  requests[1].state = lua_newthread(L);
  requests[0].role = 1;
  requests[1].role = 2;
  if (overdue)
    requests[0].phase = requests[1].phase = 4;
  if (strstr(argv[1], "generational") != NULL)
    lua_gc(L, LUA_GCGEN);
  for (round = 0; round < 2; round++) {
    int skipped;
    /* These rounds measure coalescing of new automatic requests. A poll of
     * an existing shared job may simply defer a busy phase boundary. */
    lua_gc(L, LUA_GCCOLLECT);
    require(!luaC_tracing(L) && !luaC_sweeping(L));
    fixture.arrived[0] = fixture.arrived[1] = fixture.done = 0;
    lua_lock(L);
    luaW_heapenter(L);
    luaE_setdebt(G(L), overdue ? MAX_LMEM : 0);
    if (overdue)
      __atomic_store_n(&G(L)->pacer.hard, 0, __ATOMIC_RELAXED);
    require(pthread_create(&workers[0], NULL, request, &requests[0]) == 0);
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    while (fixture.arrived[0] == 0)
      require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
    require(pthread_create(&workers[1], NULL, request, &requests[1]) == 0);
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    while (fixture.arrived[1] == 0 && !fixture.done)
      require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
    skipped = fixture.done && fixture.arrived[1] == 0;
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
    if (explicit_step) {
      unsigned steps;
      /* The public API owns its stack entry. Retain this carrier's metadata
       * reservation while admitting that entry, ahead of the queued request. */
      lua_unlock(L);
      for (steps = 0; steps < 1000; steps++)
        if (lua_gc(L, LUA_GCSTEP, (size_t)0))
          break;
      require(steps < 1000); /* Explicit requests still complete a cycle. */
      lua_lock(L);
    }
    if (stop) {
      lua_unlock(L);
      lua_gc(L, LUA_GCSTOP);
      lua_lock(L);
    }
    luaW_heapleave(L);
    lua_unlock(L);
    require(pthread_join(workers[0], NULL) == 0);
    require(pthread_join(workers[1], NULL) == 0);
    /* Coalesce ordinary requests; a spent hard allowance must instead join
     * admission, even with a positive explicit work quota. */
    require(overdue ? !skipped : skipped);
    if (stop)
      lua_gc(L, LUA_GCRESTART);
  }
done:
  lua_close(L);
  require(pthread_cond_destroy(&fixture.changed) == 0);
  require(pthread_mutex_destroy(&fixture.mutex) == 0);
  return 0;
}
