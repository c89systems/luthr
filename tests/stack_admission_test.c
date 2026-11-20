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

enum { FIFO, RETIRE };
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *state;
  unsigned queued[4];
  unsigned paused, resume;
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
void __real_luaW_leave(LuaWorld *, int);

static void pause_owner(struct fixture *fixture) {
  require(pthread_mutex_lock(&fixture->mutex) == 0);
  fixture->paused = 1;
  require(pthread_cond_broadcast(&fixture->changed) == 0);
  while (!fixture->resume)
    require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
  require(pthread_mutex_unlock(&fixture->mutex) == 0);
}

void __wrap_luaW_leave(LuaWorld *world, int queued) {
  if (role == 1 && current != NULL && current->mode == RETIRE)
    pause_owner(current);
  __real_luaW_leave(world, queued);
}

void __wrap_luaQ_wait(lua_State *state, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters) {
  struct fixture *fixture = role > 0 ? current : NULL;
  int measured = fixture != NULL && fixture->mode == FIFO && role > 0 &&
                 state == fixture->state && mutex == &G(state)->world.mutex;
  if (measured) {
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->queued[role] = 1;
    require(pthread_cond_broadcast(&fixture->changed) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
  }
  __real_luaQ_wait(state, mutex, changed, waiters);
  if (measured && role == 1 && !fixture->paused &&
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
  require(pthread_create(&thread, NULL, invoke, &call) == 0);
  wait_flag(fixture, &fixture->paused);
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

static void reuse_frames(lua_State *state) {
  luaL_openlibs(state);
  lua_pushcfunction(state, one);
  lua_setglobal(state, "one");
  lua_pushcfunction(state, defer_trim);
  lua_setglobal(state, "defer_trim");
  int status = luaL_dostring(
      state,
      "collectgarbage('stop'); "
      "local function call(n) "
      "  if n==0 then local value=one(); return value end; "
      "  return 1+call(n-1) "
      "end; "
      "assert(call(0)==1); defer_trim(); "
      "local warm=collectgarbage('count'); assert(call(0)==1); "
      "assert(collectgarbage('count')==warm, 'warm frame was discarded'); "
      "assert(call(48)==49); local peak=collectgarbage('count'); "
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
  else {
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
