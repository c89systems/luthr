#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

enum { OBJECTS = 4096 };
static struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *collector;
  unsigned phase, finalized, selected, rooted, batches, expected, postponed;
  unsigned old_finalized, finalizer_stops;
  int entered, release, generational, overlap, old_entered, old_release;
} *fixture;

void __real_luaW_restore(lua_State *, LuaWorldScope, unsigned);
void __wrap_luaW_restore(lua_State *L, LuaWorldScope scope, unsigned native) {
  if (fixture != NULL && scope.collection != 0 &&
      (L->ci->callstatus & CIST_FIN))
    __atomic_fetch_add(&fixture->finalizer_stops, 1, __ATOMIC_RELAXED);
  __real_luaW_restore(L, scope, native);
}

void __real_luaC_claimtrace(lua_State *, LuaTraceWork *, unsigned);
void __wrap_luaC_claimtrace(lua_State *state, LuaTraceWork *work,
                            unsigned limit) {
  struct fixture *test = fixture;
  GCObject **tail = G(state)->trace_plan.finalizer_newtail;
  GCObject *pending = G(state)->trace_plan.pending;
  unsigned phase = luaC_tracephase(G(state)), count = 0;
  __real_luaC_claimtrace(state, work, limit);
  if (test == NULL)
    return;
  if (phase == TRACE_FINALIZERS) {
    GCObject *object;
    require(tail != NULL);
    for (object = *tail; object != NULL; object = object->next)
      count++;
    require(count <= limit);
    test->selected += count;
  } else if (phase == TRACE_PENDING) {
    while (pending != G(state)->trace_plan.pending) {
      require(pending != NULL);
      pending = pending->next;
      count++;
    }
    require(count <= limit);
    test->rooted += count;
  }
}

lu_mem __real_luaC_tracebatch(lua_State *, unsigned);
lu_mem __wrap_luaC_tracebatch(lua_State *state, unsigned limit) {
  struct fixture *test = fixture;
  unsigned phase = luaC_tracephase(G(state));
  lu_mem worked = __real_luaC_tracebatch(state, limit);
  if (test != NULL && state == test->collector && phase == test->phase &&
      worked != 0) {
    require(state->world_depth == 0 && !luaW_stopped(&G(state)->world));
    test->batches++;
    require(pthread_mutex_lock(&test->mutex) == 0);
    if (!test->entered) {
      require(test->phase == TRACE_PENDING
                  ? G(state)->trace_plan.pending != NULL
                  : G(state)->trace_plan.finalizers != NULL);
      if (test->overlap) {
        state->native_depth++;
        require(pthread_mutex_lock(&G(state)->world.mutex) == 0);
        luaW_deactivate(state);
        require(pthread_mutex_unlock(&G(state)->world.mutex) == 0);
      }
      test->entered = 1;
      require(pthread_cond_broadcast(&test->changed) == 0);
      while (!test->release)
        require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
      if (test->overlap) {
        LuaWorld *world = &G(state)->world;
        require(pthread_mutex_lock(&world->mutex) == 0);
        while (luaW_stopped(world))
          luaQ_wait(state, &world->mutex, &world->changed, &world->waiters);
        luaW_resume(state);
        require(pthread_mutex_unlock(&world->mutex) == 0);
        state->native_depth--;
        luaC_checksnapshot(state);
      }
    }
    require(pthread_mutex_unlock(&test->mutex) == 0);
  }
  return worked;
}

static int finalize(lua_State *state) {
  lua_Integer id;
  lua_getfield(state, 1, "id");
  id = lua_tointeger(state, -1);
  if (id <= 0) {
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    if (id == 0) {
      fixture->old_entered = 1;
      require(pthread_cond_broadcast(&fixture->changed) == 0);
      while (!fixture->old_release)
        require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
    }
    fixture->old_finalized++;
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
    return 0;
  }
  require(!luaC_tracing(state));
  if (fixture->postponed != 0 && fixture->expected == fixture->postponed)
    fixture->expected--;
  require(id == fixture->expected);
  fixture->expected--;
  fixture->finalized++;
  return 0;
}

static void *older_collection(void *argument) {
  lua_gc(argument, LUA_GCCOLLECT);
  return NULL;
}

static void *collect(void *argument) {
  struct fixture *test = argument;
  unsigned rounds;
  unsigned target = OBJECTS - (test->postponed != 0);
  for (rounds = 0; rounds < 64 && test->finalized < target; rounds++) {
    if (test->generational)
      lua_gc(test->collector, LUA_GCSTEP, (size_t)0);
    else
      lua_gc(test->collector, LUA_GCCOLLECT);
  }
  require(test->finalized == target);
  return NULL;
}

static lua_Integer expected_order;

static int ordered_finalizer(lua_State *L) {
  require(lua_getfield(L, 1, "id") == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == expected_order);
  expected_order--;
  return 0;
}

static void finalize_across_coroutines(lua_State *L, int generational) {
  lua_State *owners[2];
  int i;
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  owners[0] = lua_newthread(L);
  owners[1] = lua_newthread(L);
  expected_order = 4;
  for (i = 1; i <= 4; i++) {
    lua_State *owner = owners[i % 2];
    lua_newtable(owner);
    lua_pushinteger(owner, i);
    lua_setfield(owner, -2, "id");
    lua_newtable(owner);
    lua_pushcfunction(owner, ordered_finalizer);
    lua_setfield(owner, -2, "__gc");
    require(lua_setmetatable(owner, -2));
    lua_pop(owner, 1);
  }
  lua_gc(L, LUA_GCCOLLECT);
  require(expected_order == 0);
  lua_close(L);
}

enum { PRODUCERS = 4, REGISTRATIONS = 512 };

struct finalizer_registrations {
  pthread_barrier_t start;
  lua_State *collector;
  unsigned remaining;
};

struct finalizer_producer {
  lua_State *L;
  struct finalizer_registrations *registrations;
  unsigned index;
};

static unsigned finalizer_remaining[PRODUCERS];

static int registered_finalizer(lua_State *L) {
  lua_Integer id;
  unsigned producer, index;
  require(lua_getfield(L, 1, "id") == LUA_TNUMBER);
  id = lua_tointeger(L, -1);
  require(id >= 0 && id < PRODUCERS * REGISTRATIONS);
  producer = (unsigned)id / REGISTRATIONS;
  index = (unsigned)id % REGISTRATIONS;
  require(finalizer_remaining[producer] == index + 1);
  finalizer_remaining[producer]--;
  return 0;
}

static void *register_objects(void *argument) {
  struct finalizer_producer *producer = argument;
  struct finalizer_registrations *registrations = producer->registrations;
  lua_State *L = producer->L;
  unsigned index;
  int ready;
  lua_newtable(L);
  lua_newtable(L);
  lua_pushvalue(L, 1);
  lua_setmetatable(L, 2);
  lua_pop(L, 1);
  /* The previous binding had no finalizer. Adding the field only affects
   * objects bound from this point on. */
  lua_pushcfunction(L, registered_finalizer);
  lua_setfield(L, 1, "__gc");
  lua_newtable(L); /* keep registrations live until all producers finish */
  ready = pthread_barrier_wait(&registrations->start);
  require(ready == 0 || ready == PTHREAD_BARRIER_SERIAL_THREAD);
  for (index = 0; index < REGISTRATIONS; index++) {
    lua_newtable(L);
    lua_pushinteger(L, producer->index * REGISTRATIONS + index);
    lua_setfield(L, 3, "id");
    lua_pushvalue(L, 1);
    lua_setmetatable(L, 3);
    lua_rawseti(L, 2, (lua_Integer)index + 1);
  }
  __atomic_fetch_sub(&registrations->remaining, 1, __ATOMIC_RELAXED);
  return NULL;
}

static void *collect_registrations(void *argument) {
  struct finalizer_registrations *registrations = argument;
  int ready = pthread_barrier_wait(&registrations->start);
  require(ready == 0 || ready == PTHREAD_BARRIER_SERIAL_THREAD);
  do {
    lua_gc(registrations->collector, LUA_GCCOLLECT);
  } while (__atomic_load_n(&registrations->remaining, __ATOMIC_RELAXED) != 0);
  return NULL;
}

static void finalize_concurrent_registrations(lua_State *L, int generational) {
  struct finalizer_registrations registrations;
  struct finalizer_producer producers[PRODUCERS];
  pthread_t threads[PRODUCERS], collector;
  unsigned i, round;
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  registrations.collector = lua_newthread(L);
  require(pthread_barrier_init(&registrations.start, NULL, PRODUCERS + 1) == 0);
  for (i = 0; i < PRODUCERS; i++)
    producers[i] =
        (struct finalizer_producer){lua_newthread(L), &registrations, i};
  for (round = 0; round < 2; round++) {
    __atomic_store_n(&registrations.remaining, PRODUCERS, __ATOMIC_RELAXED);
    for (i = 0; i < PRODUCERS; i++) {
      finalizer_remaining[i] = REGISTRATIONS;
      require(pthread_create(&threads[i], NULL, register_objects,
                             &producers[i]) == 0);
    }
    require(pthread_create(&collector, NULL, collect_registrations,
                           &registrations) == 0);
    for (i = 0; i < PRODUCERS; i++)
      require(pthread_join(threads[i], NULL) == 0);
    require(pthread_join(collector, NULL) == 0);
    for (i = 0; i < PRODUCERS; i++)
      lua_settop(producers[i].L, 0);
    lua_gc(L, LUA_GCCOLLECT);
    for (i = 0; i < PRODUCERS; i++)
      require(finalizer_remaining[i] == 0);
  }
  require(pthread_barrier_destroy(&registrations.start) == 0);
  lua_close(L);
}

int main(int argc, char **argv) {
  struct fixture test = {0};
  lua_State *state = luaL_newstate(), *worker;
  pthread_t collector, older;
  unsigned before, i;
  require(state != NULL && argc == 2);
  if (strncmp(argv[1], "order_", 6) == 0) {
    finalize_across_coroutines(state,
                               strcmp(argv[1], "order_generational") == 0);
    return 0;
  }
  if (strncmp(argv[1], "register_", 9) == 0) {
    finalize_concurrent_registrations(
        state, strcmp(argv[1], "register_generational") == 0);
    return 0;
  }
  test.phase = strstr(argv[1], "pending") ? TRACE_PENDING : TRACE_FINALIZERS;
  test.generational = strstr(argv[1], "generational") != NULL;
  test.overlap = strstr(argv[1], "overlap") != NULL;
  test.expected = OBJECTS;
  if (strstr(argv[1], "promote") != NULL)
    test.postponed = OBJECTS / 2;
  require(pthread_mutex_init(&test.mutex, NULL) == 0);
  require(pthread_cond_init(&test.changed, NULL) == 0);
  fixture = &test;
  luaL_openlibs(state);
  if (test.generational || test.overlap)
    lua_gc(state, LUA_GCGEN);
  lua_gc(state, LUA_GCSTOP);
  lua_pushcfunction(state, finalize);
  lua_setglobal(state, "finalize");
  test.collector = lua_newthread(state);
  worker = lua_newthread(state);
  if (test.overlap) {
    lua_State *old = lua_newthread(state);
    require(luaL_loadstring(state, "local mt={__gc=finalize}; for i=1,129 do "
                                   "setmetatable({id=-1},mt) end; "
                                   "setmetatable({id=0},mt)") == LUA_OK);
    require(lua_pcall(state, 0, 0, 0) == LUA_OK);
    require(pthread_create(&older, NULL, older_collection, old) == 0);
    require(pthread_mutex_lock(&test.mutex) == 0);
    while (!test.old_entered)
      require(pthread_cond_wait(&test.changed, &test.mutex) == 0);
    require(pthread_mutex_unlock(&test.mutex) == 0);
    lua_gc(state, LUA_GCINC);
    /* Finish the mode change's initial sweep without dispatching the queued
     * older callbacks. The next ordinary cycle must scan those roots. */
    lua_lock(state);
    luaC_fullgc(state, 1);
    lua_unlock(state);
    test.rooted = test.selected = 0;
  }
  if (test.postponed != 0) {
    require(luaL_loadstring(state, "rescue=setmetatable({}, {__mode='k'})") ==
            LUA_OK);
    require(lua_pcall(state, 0, 0, 0) == LUA_OK);
  }
  require(luaL_loadstring(
              state, "progress={value=0}; local previous; "
                     "local mt={__gc=finalize}; for id=1,4096 do "
                     "local object={id=id,previous=previous}; "
                     "if rescue and id==2048 then "
                     "object.previous=nil; rescue[object]=true end; "
                     "setmetatable(object,mt); previous=object end") == LUA_OK);
  require(lua_pcall(state, 0, 0, 0) == LUA_OK);
  require(luaL_loadstring(worker, "if rescue then survivor=next(rescue); "
                                  "assert(survivor.id==2048) end; "
                                  "for i=1,20000 do progress.value=i end; "
                                  "return progress.value") == LUA_OK);
  if (test.phase == TRACE_PENDING && !test.overlap) {
    lua_lock(state);
    luaC_fullgc(state, 1);
    require(G(state)->tobefnz != NULL && test.finalized == 0);
    lua_unlock(state);
  }
  require(pthread_create(&collector, NULL, collect, &test) == 0);
  require(pthread_mutex_lock(&test.mutex) == 0);
  while (!test.entered)
    require(pthread_cond_wait(&test.changed, &test.mutex) == 0);
  require(pthread_mutex_unlock(&test.mutex) == 0);

  /* Normal Lua mutation proceeds while a finalizer batch's original driver
   * is parked. Another admitted execution can also advance the same walk. */
  require(lua_pcall(worker, 0, 1, 0) == LUA_OK);
  require(lua_tointeger(worker, -1) == 20000);
  lua_lock(worker);
  before = test.phase == TRACE_PENDING ? test.rooted : test.selected;
  for (i = 0; i < 4; i++)
    luaC_tracebatch(worker, 16);
  require((test.phase == TRACE_PENDING ? test.rooted : test.selected) > before);
  lua_unlock(worker);
  if (test.overlap) {
    require(pthread_mutex_lock(&test.mutex) == 0);
    test.old_release = 1;
    require(pthread_cond_broadcast(&test.changed) == 0);
    require(pthread_mutex_unlock(&test.mutex) == 0);
    require(pthread_join(older, NULL) == 0);
    require(test.old_finalized == 130);
    if (test.phase == TRACE_PENDING)
      require(test.rooted <= 129);
  }
  require(pthread_mutex_lock(&test.mutex) == 0);
  test.release = 1;
  require(pthread_cond_broadcast(&test.changed) == 0);
  require(pthread_mutex_unlock(&test.mutex) == 0);
  require(pthread_join(collector, NULL) == 0);
  require(test.finalized == OBJECTS - (test.postponed != 0) &&
          test.batches > (test.overlap ? 0u : 1u));
  if (strncmp(argv[1], "discovery_", 10) == 0)
    require(test.finalizer_stops == 0);
  if (test.postponed != 0) {
    test.expected = test.postponed;
    test.postponed = 0;
    test.collector = NULL;
    lua_pushnil(state);
    lua_setglobal(state, "survivor");
    lua_gc(state, LUA_GCCOLLECT);
    require(test.finalized == OBJECTS);
  }
  lua_close(state);
  fixture = NULL;
  require(pthread_cond_destroy(&test.changed) == 0);
  require(pthread_mutex_destroy(&test.mutex) == 0);
  return 0;
}
