#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lgc.h>
#include <lstate.h>
#include <ltable.h>
#include <ltask.h>
#include <pthread.h>
#include <sched.h>
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
  unsigned arrived[2], startup_waits, trace_waits, done;
  int driver_parked, release_driver, finalized;
} fixture = {.mutex = PTHREAD_MUTEX_INITIALIZER,
             .changed = PTHREAD_COND_INITIALIZER};
static __thread unsigned role;
static __thread unsigned stops;
enum { MINOR_NO_GATE, MINOR_UNWIND, MINOR_FINALIZER, MINOR_ARM, ARM_RELEASE };
static int minor_case;
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
void __real_luaW_heapleave(lua_State *);
void __wrap_luaW_heapleave(lua_State *L) {
  int arm = role == 1 && minor_case == ARM_RELEASE &&
            (luaW_control(&G(L)->world) & LUA_WORLD_ARM_TRACE);
  __real_luaW_heapleave(L);
  if (arm) {
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    fixture.driver_parked = 1;
    require(pthread_cond_broadcast(&fixture.changed) == 0);
    while (!fixture.release_driver)
      require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
  }
}
void __wrap_luaW_collectenter(lua_State *L) {
  arrive();
  __real_luaW_collectenter(L);
}
int __wrap_luaW_trycollectenter(lua_State *L) {
  arrive();
  return __real_luaW_trycollectenter(L);
}

void __real_luaW_waitcollector(lua_State *, int);
void __wrap_luaW_waitcollector(lua_State *L, int reason) {
  if (role != 0 &&
      (reason == LUA_WAIT_START ||
       (reason == LUA_WAIT_TRACE &&
        __atomic_load_n(&G(L)->trace_advance, __ATOMIC_ACQUIRE)))) {
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    if (reason == LUA_WAIT_START)
      fixture.startup_waits++;
    else
      fixture.trace_waits++;
    require(pthread_cond_broadcast(&fixture.changed) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
  }
  __real_luaW_waitcollector(L, reason);
}

lu_mem __real_luaC_tracebatch(lua_State *, unsigned);
lu_mem __wrap_luaC_tracebatch(lua_State *L, unsigned limit) {
  if (role == 1 && minor_case != MINOR_NO_GATE && minor_case != ARM_RELEASE &&
      !fixture.driver_parked) {
    lu_mem marked = 0;
    if (minor_case == MINOR_ARM) {
      require(stops == 0 && !luaW_stopped(&G(L)->world));
      marked = __real_luaC_tracebatch(L, limit);
      require(marked != 0);
    }
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    fixture.driver_parked = 1;
    require(pthread_cond_broadcast(&fixture.changed) == 0);
    while (!fixture.release_driver)
      require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
    if (minor_case == MINOR_UNWIND)
      luaD_throw(L, LUA_ERRRUN);
    if (minor_case == MINOR_ARM)
      return marked;
  }
  return __real_luaC_tracebatch(L, limit);
}

struct request {
  lua_State *state;
  unsigned role, stops;
  int phase, status;
};

static void markdone(void) {
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  fixture.done = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
}

static void protected_step(lua_State *L, void *argument) {
  (void)argument;
  require(luaC_userstep(L, 0) == 0);
}

static void *request(void *argument) {
  struct request *r = argument;
  role = r->role;
  if (r->phase == 5) {
    require(lua_gc(r->state, LUA_GCSTEP, (size_t)0) == 0);
    r->stops = stops;
    if (role == 2)
      markdone();
    return NULL;
  }
  lua_lock(r->state);
  if (r->phase == 6)
    r->status = luaD_rawrunprotected(r->state, protected_step, NULL);
  else if (r->phase == 4) {
    luaC_checkGC(r->state);
  } else if (r->phase == 7) {
    luaW_heapenter(r->state);
    require(luaC_scanready(G(r->state)));
    luaW_heapleave(r->state);
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
  if (role == 2 || (r->phase && r->phase != 6))
    markdone();
  return NULL;
}

static int minor_finalizer(lua_State *L) {
  (void)L;
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.done)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  fixture.finalized++;
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  return 0;
}

static void arm_release(lua_State *L, int young, int withdraw) {
  LuaWorld *world = &G(L)->world;
  struct request driver = {0}, waiter = {0};
  pthread_t workers[2];
  lua_State *mutator, *ready, *fresh;
  lua_gc(L, young ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  if (!young) {
    require(luaL_loadstring(
                L, "local t; for i=1,4096 do t={t} end; return t") == LUA_OK);
    require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  }
  mutator = lua_newthread(L);
  ready = lua_newthread(L);
  driver.state = lua_newthread(L);
  waiter.state = lua_newthread(L);
  driver.role = 1;
  driver.phase = 5;
  waiter.role = 2;
  waiter.phase = 7;
  minor_case = ARM_RELEASE;
  lua_lock(mutator);
  require(pthread_create(&workers[0], NULL, request, &driver) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.driver_parked)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(!luaC_scanready(G(L)) && !luaW_trycollectenter(mutator));
  lua_lock(ready);
  lua_unlock(ready); /* acknowledge the in-flight epoch */
  fresh = lua_newthread(L);
  require(pthread_mutex_lock(&world->mutex) == 0);
  /* An acknowledged participant and a new state can enter and leave while
   * another participant still owes the collector its old allocation debt. */
  lua_lock(ready);
  require(luaW_active(ready->admission));
  lua_unlock(ready);
  lua_lock(fresh);
  require(luaW_active(fresh->admission));
  lua_unlock(fresh);
  require(pthread_mutex_unlock(&world->mutex) == 0);
  require(!luaC_scanready(G(L)));
  require(pthread_create(&workers[1], NULL, request, &waiter) == 0);
  require(pthread_mutex_lock(&world->mutex) == 0);
  while (world->heap_head == NULL)
    luaQ_wait(mutator, &world->mutex, &world->changed, &world->waiters);
  require(!luaW_heapheld(world));
  require(pthread_mutex_unlock(&world->mutex) == 0);
  if (withdraw)
    lua_unlock(mutator);
  else {
    luaW_acktrace(mutator);
    require(luaW_active(mutator->admission));
  }
  require(luaC_scanready(G(L)) && !luaW_stopped(world));
  require(pthread_join(workers[1], NULL) == 0);
  if (!withdraw)
    lua_unlock(mutator);
  lua_lock(L);
  require(luaC_tracebatch(L, LUA_TRACE_BATCH) != 0);
  lua_unlock(L);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  fixture.release_driver = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_join(workers[0], NULL) == 0);
  if (young)
    require(!luaC_tracing(L) && !luaC_sweeping(L));
  else {
    require(driver.stops == 0 && luaC_tracing(L));
    lua_gc(L, LUA_GCCOLLECT);
    lua_pushvalue(L, 1);
    for (unsigned i = 0; i < 4096; i++) {
      require(lua_istable(L, -1));
      lua_rawgeti(L, -1, 1);
      lua_remove(L, -2);
    }
    require(lua_isnil(L, -1));
    lua_pop(L, 1);
  }
}

static void late_task_root(lua_State *L) {
  struct request driver = {0};
  pthread_t worker;
  LuaTaskRoot root = {0};
  LuaExecution execution = {0};
  NativeStackContext native, *previous;
  lua_State *mutator, *task;
  int status, results;

  lua_gc(L, LUA_GCGEN);
  lua_gc(L, LUA_GCSTOP);
  lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, 1);
  mutator = lua_newthread(L);
  driver.state = lua_newthread(L);
  task = lua_newthread(L);
  require(luaL_loadstring(task,
                          "local t={value=42}; "
                          "return function() return t.value end") == LUA_OK);
  require(lua_pcall(task, 0, 1, 0) == LUA_OK);
  lua_pushvalue(L, -1);
  lua_rawseti(L, 1, 1);

  driver.role = 1;
  driver.phase = 5;
  minor_case = ARM_RELEASE;
  lua_lock(mutator);
  require(pthread_create(&worker, NULL, request, &driver) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.driver_parked)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  /* Admission transfers the last strong reference after initial root discovery
   * and before scanners or entering mutators can take stack snapshots. */
  require(!luaC_scanready(G(L)) && iswhite(G(L), task));
  require(luaTK_admit(task, &root, &execution) == LUA_OK);
  lua_pop(L, 1);
  lua_unlock(mutator);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  fixture.release_driver = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_join(worker, NULL) == 0);
  require(!luaC_tracing(L) && !luaC_sweeping(L));
  require(lua_rawgeti(L, 1, 1) == LUA_TTHREAD && lua_tothread(L, -1) == task);
  lua_pop(L, 1);

  native = *luaE_getnativestackcontext();
  native.execution = &execution;
  previous = luaE_setnativestackcontext(&native);
  status = luaTK_resume(&root, 0, &results);
  require(status == LUA_OK && results == 1);
  require(luaTK_finish(&root, &status, &results) == LUA_OK);
  luaE_setnativestackcontext(previous);
  require(luaTK_collect(L, &root, &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1 && lua_tointeger(L, -1) == 42);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(L, 1, 1) == LUA_TNIL);
}

static void minor_arm(lua_State *L, int busy_registry) {
  LuaWorld *world = &G(L)->world;
  LuaObjectScope registry;
  struct request driver = {0};
  lua_State *mutator;
  pthread_t worker;
  lua_gc(L, LUA_GCGEN);
  lua_gc(L, LUA_GCSTOP);
  lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  mutator = lua_newthread(L);
  require(luaL_loadstring(mutator, "local h; for i=1,4096 do h={h} end; "
                                   "return h") == LUA_OK);
  require(lua_pcall(mutator, 0, 1, 0) == LUA_OK);
  driver.state = lua_newthread(L);
  driver.role = 1;
  driver.phase = 5;
  minor_case = MINOR_ARM;
  if (busy_registry) {
    /* This young table is reachable only through the replaceable root slot. */
    lua_newtable(L);
    lua_pushinteger(L, 918);
    lua_setfield(L, -2, "arm_root");
    lua_copy(L, -1, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
  }
  lua_lock(mutator);
  if (busy_registry)
    luaA_beginlock(mutator, &G(L)->registry_access, &registry);
  require(pthread_create(&worker, NULL, request, &driver) == 0);
  while (!(luaW_control(world) & LUA_WORLD_ARM_TRACE))
    sched_yield();
  require(!luaW_stopped(world) && !luaC_scanready(G(L)));
  luaW_acktrace(mutator);
  require(luaW_active(mutator->admission));
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.driver_parked)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(luaW_active(mutator->admission) && !luaW_stopped(world));
  if (busy_registry) {
    require(iswhite(G(L), hvalue(&G(L)->l_registry)));
    luaA_end(mutator, &registry);
  }
  require(mutator->trace_snapshot != luaC_traceepoch(G(L)));
  require(luaD_reallocstack(mutator, stacksize(mutator) + 128, 1));
  require(mutator->trace_snapshot == luaC_traceepoch(G(L)));
  lua_unlock(mutator);
  fixture.release_driver = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_join(worker, NULL) == 0);
  require(!luaC_tracing(L) && !luaC_sweeping(L));
  if (busy_registry) {
    require(lua_getfield(L, LUA_REGISTRYINDEX, "arm_root") == LUA_TNUMBER);
    require(lua_tointeger(L, -1) == 918);
    lua_pop(L, 1);
  } else {
    require(driver.stops == 1);
  }
  for (unsigned i = 0; i < 4096; i++) {
    require(lua_istable(mutator, -1));
    lua_rawgeti(mutator, -1, 1);
    lua_remove(mutator, -2);
  }
  require(lua_isnil(mutator, -1));
}

static void explicit_minor_followers(lua_State *L, int unwind) {
  LuaWorld *world = &G(L)->world;
  struct request driver = {0}, follower = {0};
  pthread_t workers[2];
  lua_gc(L, LUA_GCGEN);
  lua_gc(L, LUA_GCSTOP);
  lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  require(luaL_loadstring(L, "local h; for i=1,10000 do h={h} end; return h") ==
          LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  if (!unwind) {
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, minor_finalizer);
    lua_setfield(L, -2, "__gc");
    require(lua_setmetatable(L, -2));
    lua_pop(L, 1);
  }
  driver.state = lua_newthread(L);
  follower.state = lua_newthread(L);
  driver.role = 1;
  follower.role = 2;
  follower.phase = 5;
  driver.phase = unwind ? 6 : 5;
  minor_case = unwind ? MINOR_UNWIND : MINOR_FINALIZER;
  require(pthread_create(&workers[0], NULL, request, &driver) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.driver_parked)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_create(&workers[1], NULL, request, &follower) == 0);
  require(pthread_mutex_lock(&world->mutex) == 0);
  while (G(L)->minor_waiters == NULL) {
    require(pthread_mutex_unlock(&world->mutex) == 0);
    sched_yield();
    require(pthread_mutex_lock(&world->mutex) == 0);
  }
  require(G(L)->minor_request & 1);
  require(pthread_mutex_unlock(&world->mutex) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  fixture.release_driver = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_join(workers[0], NULL) == 0);
  require(pthread_join(workers[1], NULL) == 0);
  require(!luaC_tracing(L) && !luaC_sweeping(L) && !(G(L)->minor_request & 1));
  require(G(L)->minor_waiters == NULL && fixture.done);
  if (unwind)
    require(driver.status == LUA_ERRRUN);
  else
    require(fixture.finalized == 1 && follower.stops == 0);
}

static void explicit_trace_slice(lua_State *L) {
  struct request helper = {0};
  LuaWorld *world = &G(L)->world;
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
  require(pthread_mutex_lock(&world->mutex) == 0);
  /* Owner-local nesting completes while the world mutex is occupied. */
  luaW_heapenter(L);
  require(luaW_trycollectenter(L));
  luaW_heapleave(L);
  luaW_heapleave(L);
  require(luaW_ownsheap(L) && luaW_scope(L).heap == 1);
  require(pthread_mutex_unlock(&world->mutex) == 0);
  require(pthread_create(&worker, NULL, request, &helper) == 0);
  require(pthread_join(worker, NULL) == 0);
  require(helper.stops == 0 && luaC_tracing(L));
  require(__atomic_load_n(&G(L)->pacer.work, __ATOMIC_RELAXED) > work);
  luaW_heapleave(L);
  require(!luaW_heapheld(world));
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

static void major_after_minor(lua_State *L) {
  global_State *g = G(L);
  GCObject *root;
  unsigned steps = 0;
  lua_gc(L, LUA_GCGEN);
  lua_gc(L, LUA_GCSTOP);
  lua_newtable(L);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, 1);
  require(luaL_loadstring(L, "local t; for i=1,4096 do t={t} end; return t") ==
          LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  do {
    require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
    require(++steps < 8);
  } while (luaC_kind(g) == KGC_GENMINOR);
  lua_lock(L);
  require(luaC_kind(g) == KGC_GENMAJOR && luaC_state(g) == GCSpause &&
          (luaC_markstate(g) & LUA_MARK_YOUNG));
  root = gcvalue(s2v(L->top.p - 2));
  require(isold(root) && !isgray(root));
  /* A writer that observed young mode can still publish a remembered link
   * across the mode change. The next major must drain it before marking. */
  l_setbit(root->marked, QUEUEDBIT);
  luaC_publishbarrier(L, root, 1);
  role = 1;
  require(luaC_userstep(L, 0) == 0);
  require(stops == 1 && luaC_tracing(L));
  role = 0;
  lua_unlock(L);
  lua_gc(L, LUA_GCCOLLECT);
  for (unsigned i = 0; i < 4096; i++) {
    require(lua_istable(L, -1));
    lua_rawgeti(L, -1, 1);
    lua_remove(L, -2);
  }
  require(lua_isnil(L, -1));
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
  if (!sweeping) {
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    while (fixture.trace_waits == 0)
      require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
  }
  require(pthread_mutex_lock(&world->mutex) == 0);
  /* The caller retains metadata and active stack participation. A table
   * sweep can finish without it. During tracing, only one helper needs the
   * metadata grant while the other waits for progress. */
  if (sweeping) {
    while (luaC_sweeping(L))
      luaQ_wait(L, &world->mutex, &G(L)->phase_changed, &G(L)->phase_waiters);
  } else {
    while (!helpers[0].state->heap_waiting && !helpers[1].state->heap_waiting)
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
    require(helpers[0].state->heap_waiting + helpers[1].state->heap_waiting ==
            1);
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

static void empty_minor(lua_State *L) {
  uint64_t epoch;
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  require(lua_setmetatable(L, 1));
  lua_newtable(L);
  lua_pushcfunction(L, live_finalizer);
  lua_setfield(L, 2, "__gc");
  lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  lua_gc(L, LUA_GCGEN);
  lua_gc(L, LUA_GCSTOP);
  epoch = luaC_traceepoch(G(L));
  for (unsigned i = 0; i < 8; i++)
    require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  require(luaC_traceepoch(G(L)) == epoch);

  lua_pushinteger(L, 42);
  lua_pop(L, 1); /* Ordinary stack changes leave the cohort empty. */
  lua_newtable(L);
  lua_pushvalue(L, -1);
  lua_rawseti(L, 1, 1);
  require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  require(luaC_traceepoch(G(L)) != epoch && lua_istable(L, -1));
  lua_pop(L, 1); /* The surviving table has only its weak edge left. */
  require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  require(lua_rawgeti(L, 1, 1) == LUA_TNIL);
  lua_pop(L, 1);

  push_finalizable(L, 1);
  require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  require(callbacks == 1);

  lua_createtable(L, 2048, 0);
  for (int i = 1; i <= 2048; i++) {
    lua_newtable(L);
    lua_rawseti(L, -2, i);
  }
  for (unsigned i = 0; i < 4; i++)
    require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  epoch = luaC_traceepoch(G(L));
  require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  require(luaC_traceepoch(G(L)) == epoch);
  lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, 1);
  require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  require(luaC_kind(G(L)) == KGC_GENMAJOR);
}

static void minor_sweep_birth(lua_State *L) {
  uint64_t epoch;
  Table *child;
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  require(lua_setmetatable(L, 1));
  lua_gc(L, LUA_GCGEN);
  lua_lock(L);
  (void)luaH_new(L, 0, 0); /* Dead at the initial snapshot. */
  G(L)->pacer.goal = MAX_LMEM;
  luaE_setdebt(G(L), 0);
  luaC_step(L);
  luaC_waittrace(L);
  require(luaC_sweeping(L));
  child = luaH_new(L, 0, 0);
  sethvalue2s(L, L->top.p, child);
  api_incr_top(L);
  luaC_waitsweep(L);
  require(!luaC_sweeping(L));
  epoch = luaC_traceepoch(G(L));
  lua_unlock(L);

  lua_gc(L, LUA_GCSTOP);
  require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  require(luaC_traceepoch(G(L)) != epoch);
  lua_rawseti(L, 1, 1);
  require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  require(lua_rawgeti(L, 1, 1) == LUA_TNIL);
  lua_pop(L, 1);
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
  if (strcmp(argv[1], "empty_minor") == 0) {
    empty_minor(L);
    goto done;
  }
  if (strcmp(argv[1], "minor_sweep_birth") == 0) {
    minor_sweep_birth(L);
    goto done;
  }
  if (strcmp(argv[1], "minor_unwind") == 0 ||
      strcmp(argv[1], "minor_finalizer") == 0) {
    explicit_minor_followers(L, strcmp(argv[1], "minor_unwind") == 0);
    goto done;
  }
  if (strcmp(argv[1], "explicit_trace_slice") == 0) {
    explicit_trace_slice(L);
    goto done;
  }
  if (strcmp(argv[1], "major_after_minor") == 0) {
    major_after_minor(L);
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
  if (strcmp(argv[1], "minor_arm") == 0 ||
      strcmp(argv[1], "minor_arm_registry") == 0) {
    minor_arm(L, strcmp(argv[1], "minor_arm_registry") == 0);
    goto done;
  }
  if (strcmp(argv[1], "arm_ack") == 0 || strcmp(argv[1], "arm_withdraw") == 0 ||
      strcmp(argv[1], "arm_incremental") == 0) {
    arm_release(L, strcmp(argv[1], "arm_incremental") != 0,
                strcmp(argv[1], "arm_withdraw") == 0);
    goto done;
  }
  if (strcmp(argv[1], "late_task_root") == 0) {
    late_task_root(L);
    goto done;
  }
  explicit_step = strcmp(argv[1], "explicit") == 0;
  stop = strstr(argv[1], "stopped") != NULL;
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
    fixture.arrived[0] = fixture.arrived[1] = fixture.startup_waits =
        fixture.done = 0;
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
    while (fixture.arrived[1] == 0 && fixture.startup_waits == 0 &&
           !fixture.done)
      require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
    if (overdue)
      require(fixture.startup_waits != 0 && fixture.arrived[1] == 0 &&
              !fixture.done);
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
    /* Ordinary requests can return. An overdue request waits for the shared
     * startup decision even with a positive explicit work quota. */
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
