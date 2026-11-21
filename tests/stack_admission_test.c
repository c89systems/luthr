#include <lauxlib.h>
#include <lgate.h>
#include <lstate.h>
#include <lualib.h>
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

enum { FIFO, RETIRE, WORLD_STOP, RENDEZVOUS, GROW };
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *state;
  LuaWaiter **waiters;
  unsigned queued[4];
  unsigned paused, resume;
  unsigned grown;
  int mode;
};
struct invocation {
  struct fixture *fixture;
  int role;
};
static struct fixture *current;
static __thread int role;

void __real_luaQ_wait(lua_State *, pthread_mutex_t *, pthread_cond_t *,
                      LuaWaiter **);
__typeof__(luaW_leave) __real_luaW_leave;

static void pause_owner(struct fixture *fixture) {
  require(pthread_mutex_lock(&fixture->mutex) == 0);
  fixture->paused = 1;
  require(pthread_cond_broadcast(&fixture->changed) == 0);
  while (!fixture->resume)
    require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
  require(pthread_mutex_unlock(&fixture->mutex) == 0);
}

void __wrap_luaW_leave(global_State *g, uintptr_t stack, unsigned flags,
                       uint64_t control) {
  if (role == 1 && current != NULL && current->mode == RETIRE)
    pause_owner(current);
  __real_luaW_leave(g, stack, flags, control);
}

void __wrap_luaQ_wait(lua_State *state, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters) {
  struct fixture *fixture = role > 0 ? current : NULL;
  int measured =
      fixture != NULL && fixture->mode != RETIRE &&
      mutex == &G(state)->world.mutex &&
      (state == fixture->state || (fixture->mode == RENDEZVOUS && role == 2));
  if (measured) {
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->waiters = waiters;
    fixture->queued[role] = 1;
    require(pthread_cond_broadcast(&fixture->changed) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
  }
  __real_luaQ_wait(state, mutex, changed, waiters);
  if (measured && fixture->mode == FIFO && role == 1 && !fixture->paused &&
      __atomic_load_n(&state->stack_gate.word, __ATOMIC_ACQUIRE) ==
          LUA_GATE_WAITING) {
    /* Delay the selected recipient after wakeup, before it can claim its
     * reservation. A fresh API caller must queue behind it and its successor.
     */
    require(pthread_mutex_unlock(mutex) == 0);
    pause_owner(fixture);
    require(pthread_mutex_lock(mutex) == 0);
  }
}

static void wait_flag(struct fixture *fixture, unsigned *flag) {
  require(pthread_mutex_lock(&fixture->mutex) == 0);
  while (!*flag)
    require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
  require(pthread_mutex_unlock(&fixture->mutex) == 0);
}

static void resume_owner(struct fixture *fixture) {
  require(pthread_mutex_lock(&fixture->mutex) == 0);
  fixture->resume = 1;
  require(pthread_cond_broadcast(&fixture->changed) == 0);
  require(pthread_mutex_unlock(&fixture->mutex) == 0);
}

static void *invoke(void *argument) {
  struct invocation *call = argument;
  role = call->role;
  lua_pushinteger(call->fixture->state, role);
  return NULL;
}

static void *grow(void *argument) {
  struct fixture *fixture = argument;
  role = 1;
  require(lua_checkstack(fixture->state, 512));
  require(pthread_mutex_lock(&fixture->mutex) == 0);
  fixture->grown = 1;
  require(pthread_cond_broadcast(&fixture->changed) == 0);
  require(pthread_mutex_unlock(&fixture->mutex) == 0);
  return NULL;
}

static void grow_during_metadata(lua_State *parent, struct fixture *fixture) {
  pthread_t worker;
  unsigned independent;
  lua_gc(parent, LUA_GCSTOP);
  fixture->state = lua_newthread(parent);
  lua_pushinteger(fixture->state, 42);
  luaX_lockstate(parent);
  luaW_heapenter(parent);
  require(pthread_create(&worker, NULL, grow, fixture) == 0);
  require(pthread_mutex_lock(&fixture->mutex) == 0);
  while (!fixture->grown && !fixture->queued[1])
    require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
  independent = fixture->grown && !fixture->queued[1];
  require(pthread_mutex_unlock(&fixture->mutex) == 0);
  luaW_heapleave(parent);
  luaX_unlockstate(parent);
  require(pthread_join(worker, NULL) == 0);
  require(independent && lua_gettop(fixture->state) == 1 &&
          lua_tointeger(fixture->state, 1) == 42);
}

static void fifo(lua_State *parent, struct fixture *fixture) {
  pthread_t threads[3];
  struct invocation calls[3];
  unsigned i;
  fixture->state = lua_newthread(parent);
  luaX_lockstate(fixture->state);
  for (i = 0; i < 2; i++) {
    calls[i].fixture = fixture;
    calls[i].role = (int)i + 1;
    require(pthread_create(&threads[i], NULL, invoke, &calls[i]) == 0);
    wait_flag(fixture, &fixture->queued[i + 1]);
  }
  luaX_unlockstate(fixture->state);
  wait_flag(fixture, &fixture->paused);
  require(luaX_tryreserve(fixture->state) == 0);
  calls[2].fixture = fixture;
  calls[2].role = 3;
  require(pthread_create(&threads[2], NULL, invoke, &calls[2]) == 0);
  wait_flag(fixture, &fixture->queued[3]);
  resume_owner(fixture);
  for (i = 0; i < 3; i++)
    require(pthread_join(threads[i], NULL) == 0);
  require(lua_gettop(fixture->state) == 3);
  for (i = 1; i <= 3; i++)
    require(lua_tointeger(fixture->state, (int)i) == (lua_Integer)i);
}

static void retire(lua_State *parent, struct fixture *fixture) {
  pthread_t thread;
  struct invocation call = {fixture, 1};
  unsigned i;
  lua_newtable(parent);
  lua_newtable(parent);
  lua_pushliteral(parent, "v");
  lua_setfield(parent, -2, "__mode");
  lua_setmetatable(parent, -2);
  fixture->state = lua_newthread(parent);
  lua_pushvalue(parent, -1);
  lua_rawseti(parent, 1, 1);
  /* Advertise a stack-root check so this release has a notification to delay.
   */
  __atomic_fetch_or(&G(parent)->world.control, LUA_WORLD_STACK_ROOTS,
                    __ATOMIC_SEQ_CST);
  require(pthread_create(&thread, NULL, invoke, &call) == 0);
  wait_flag(fixture, &fixture->paused);
  __atomic_fetch_and(&G(parent)->world.control,
                     ~(uint64_t)LUA_WORLD_STACK_ROOTS, __ATOMIC_SEQ_CST);
  /* The departing API has released its gate and admission. Its delayed
   * notification must not dereference the now unreachable coroutine again. */
  require(__atomic_load_n(&fixture->state->stack_gate.word, __ATOMIC_ACQUIRE) ==
          0);
  lua_pop(parent, 1);
  lua_gc(parent, LUA_GCCOLLECT);
  lua_rawgeti(parent, 1, 1);
  require(lua_isnil(parent, -1));
  lua_pop(parent, 1);
  for (i = 0; i < 8; i++) {
    lua_newthread(parent); /* exercise reuse before the old caller retires */
    lua_pop(parent, 1);
  }
  lua_gc(parent,
         LUA_GCCOLLECT); /* reclaim reused storage before notification */
  resume_owner(fixture);
  require(pthread_join(thread, NULL) == 0);
}

static void *enter_managed(void *argument) {
  struct fixture *fixture = argument;
  role = 1;
  luaX_lockstate(fixture->state);
  luaX_unlockstate(fixture->state);
  lua_pushinteger(fixture->state, 1);
  return NULL;
}

static void world_stop(lua_State *parent, struct fixture *fixture) {
  LuaWorld *world = &G(parent)->world;
  pthread_t thread;
  fixture->state = lua_newthread(parent);
  luaX_lockstate(parent);
  luaW_stop(parent);
  require(pthread_create(&thread, NULL, enter_managed, fixture) == 0);
  wait_flag(fixture, &fixture->queued[1]);
  require(pthread_mutex_lock(&world->mutex) == 0);
  require(luaW_stopped(world) && *fixture->waiters != NULL);
  luaQ_wake(&world->changed, &world->waiters);
  /* An unrelated notification must leave this parked entry registered. */
  require(*fixture->waiters != NULL);
  require(pthread_mutex_unlock(&world->mutex) == 0);
  luaW_start(parent);
  luaX_unlockstate(parent);
  require(pthread_join(thread, NULL) == 0);
  require(lua_gettop(fixture->state) == 1 &&
          lua_tointeger(fixture->state, 1) == 1);
}

static void *hold_gate(void *argument) {
  struct fixture *fixture = argument;
  lua_State *state = fixture->state;
  uintptr_t reservation = luaX_valueenter(state, 1);
  require(__atomic_load_n(&state->stack_gate.word, __ATOMIC_ACQUIRE) ==
          LUA_GATE_STACK);
  pause_owner(fixture);
  setivalue(s2v(state->top.p), 42);
  state->top.p++;
  luaX_valueleave(state, reservation);
  return NULL;
}

static void *stop_world(void *argument) {
  lua_State *state = argument;
  role = 2;
  luaX_lockstate(state);
  luaW_stop(state);
  luaW_start(state);
  luaX_unlockstate(state);
  return NULL;
}

static void rendezvous(lua_State *parent, struct fixture *fixture) {
  LuaWorld *world = &G(parent)->world;
  pthread_t holder, collector;
  lua_State *last = lua_newthread(parent);
  fixture->state = lua_newthread(parent);
  luaX_lockstate(last);
  require(pthread_create(&holder, NULL, hold_gate, fixture) == 0);
  wait_flag(fixture, &fixture->paused);
  require(pthread_create(&collector, NULL, stop_world, parent) == 0);
  wait_flag(fixture, &fixture->queued[2]);
  require(pthread_mutex_lock(&world->mutex) == 0);
  require(luaW_stopped(world) && *fixture->waiters != NULL);
  luaQ_wake(&world->changed, &world->waiters);
  require(*fixture->waiters != NULL);
  require(pthread_mutex_unlock(&world->mutex) == 0);
  resume_owner(fixture);
  require(pthread_join(holder, NULL) == 0);
  require(pthread_mutex_lock(&world->mutex) == 0);
  require(luaW_activecount(world) == 1 && *fixture->waiters != NULL);
  require(pthread_mutex_unlock(&world->mutex) == 0);
  luaX_unlockstate(last);
  require(pthread_join(collector, NULL) == 0);
  require(!luaW_stopped(world));
  require(lua_gettop(fixture->state) == 1 &&
          lua_tointeger(fixture->state, 1) == 42);
}

static int one(lua_State *state) {
  lua_pushinteger(state, 1);
  return 1;
}

static int defer_trim(lua_State *state) {
  luaX_lockstate(state);
  /* Model a root snapshot whose trim reaches the owner after it unwinds. */
  state->trace_trimmed = state->trace_snapshot ^ 1;
  state->poll_remaining = 1;
  luaX_unlockstate(state);
  return 0;
}

static int through_native(lua_State *state) {
  lua_Debug frame;
  require(lua_getstack(state, 0, &frame));
  lua_pushvalue(state, 1);
  lua_call(state, 0, 1);
  require(lua_getinfo(state, "S", &frame) && strcmp(frame.what, "C") == 0);
  return 1;
}

static void reuse_frames(lua_State *state) {
  luaL_openlibs(state);
  lua_pushcfunction(state, one);
  lua_setglobal(state, "one");
  lua_pushcfunction(state, defer_trim);
  lua_setglobal(state, "defer_trim");
  lua_pushcfunction(state, through_native);
  lua_setglobal(state, "through_native");
  int status = luaL_dostring(
      state,
      "collectgarbage('stop'); "
      "local function call(n) "
      "  if n==0 then local value=one(); return value end; "
      "  return 1+call(n-1) "
      "end; "
      "assert(call(4)==5); defer_trim(); "
      "local warm=collectgarbage('count'); assert(call(4)==5); "
      "assert(collectgarbage('count')==warm, 'warm frame was discarded'); "
      "assert(through_native(function() return call(48) end)==49); "
      "local peak=collectgarbage('count'); "
      "defer_trim(); assert(collectgarbage('count')<peak)");
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}

int main(int argc, char **argv) {
  struct fixture fixture = {0};
  lua_State *state = luaL_newstate();
  require(argc == 2 && state != NULL);
  require(pthread_mutex_init(&fixture.mutex, NULL) == 0);
  require(pthread_cond_init(&fixture.changed, NULL) == 0);
  if (strstr(argv[1], "generational") != NULL)
    lua_gc(state, LUA_GCGEN);
  current = &fixture;
  if (strncmp(argv[1], "fifo_", 5) == 0)
    fifo(state, &fixture);
  else if (strncmp(argv[1], "reuse_", 6) == 0)
    reuse_frames(state);
  else if (strncmp(argv[1], "grow_", 5) == 0) {
    fixture.mode = GROW;
    grow_during_metadata(state, &fixture);
  } else if (strncmp(argv[1], "world_stop_", 11) == 0) {
    fixture.mode = WORLD_STOP;
    world_stop(state, &fixture);
  } else if (strncmp(argv[1], "rendezvous_", 11) == 0) {
    fixture.mode = RENDEZVOUS;
    rendezvous(state, &fixture);
  } else {
    require(strncmp(argv[1], "retire_", 7) == 0);
    fixture.mode = RETIRE;
    retire(state, &fixture);
  }
  current = NULL;
  lua_close(state);
  require(pthread_cond_destroy(&fixture.changed) == 0);
  require(pthread_mutex_destroy(&fixture.mutex) == 0);
  return 0;
}
