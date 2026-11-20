#include <lapi.h>
#include <lauxlib.h>
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
  lua_State *main, *child;
  unsigned stopped, release_gc;
  unsigned entry_waiting, notified, release_entry;
  unsigned collector_waiting, done;
} fixture = {PTHREAD_MUTEX_INITIALIZER,
             PTHREAD_COND_INITIALIZER,
             NULL,
             NULL,
             0,
             0,
             0,
             0,
             0,
             0,
             0};

enum { MUTATOR = 1, COLLECTOR };
static __thread int role;

static void signal_flag(unsigned *flag) {
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  *flag = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
}

static void wait_flag(unsigned *flag) {
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!*flag)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
}

void __real_luaW_stop(lua_State *);
void __wrap_luaW_stop(lua_State *state) {
  __real_luaW_stop(state);
  if (role == COLLECTOR && !fixture.stopped) {
    signal_flag(&fixture.stopped);
    wait_flag(&fixture.release_gc);
  }
}

void __real_luaQ_wait(lua_State *, pthread_mutex_t *, pthread_cond_t *,
                      LuaWaiter **);
void __wrap_luaQ_wait(lua_State *state, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters) {
  LuaWorld *world = &G(state)->world;
  int entering = role == MUTATOR && mutex == &world->mutex;
  if (entering && luaW_stopped(world))
    signal_flag(&fixture.entry_waiting);
  if (role == COLLECTOR && mutex == &world->mutex && !luaW_stopped(world) &&
      world->entry_waiting != 0 && luaW_ownsheap(state))
    signal_flag(&fixture.collector_waiting);
  __real_luaQ_wait(state, mutex, changed, waiters);
  if (entering && !luaW_stopped(world) && !fixture.notified) {
    /* Let the collector run first after notification. The next stop must
     * give this entrant its admission before making the VM unavailable. */
    require(pthread_mutex_unlock(mutex) == 0);
    signal_flag(&fixture.notified);
    wait_flag(&fixture.release_entry);
    require(pthread_mutex_lock(mutex) == 0);
  }
}

static void *collect(void *unused) {
  (void)unused;
  role = COLLECTOR;
  lua_gc(fixture.main, LUA_GCCOLLECT);
  lua_gc(fixture.main, LUA_GCCOLLECT);
  signal_flag(&fixture.done);
  return NULL;
}

static void *enter(void *unused) {
  (void)unused;
  role = MUTATOR;
  lua_pushinteger(fixture.child, 42);
  return NULL;
}

int main(int argc, char **argv) {
  pthread_t collector, mutator;
  require(argc == 2);
  fixture.main = luaL_newstate();
  require(fixture.main != NULL);
  if (strcmp(argv[1], "generational") == 0)
    lua_gc(fixture.main, LUA_GCGEN);
  else
    require(strcmp(argv[1], "incremental") == 0);
  fixture.child = lua_newthread(fixture.main);
  require(pthread_create(&collector, NULL, collect, NULL) == 0);
  wait_flag(&fixture.stopped);
  require(pthread_create(&mutator, NULL, enter, NULL) == 0);
  wait_flag(&fixture.entry_waiting);
  signal_flag(&fixture.release_gc);
  wait_flag(&fixture.notified);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.collector_waiting && !fixture.done)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(fixture.collector_waiting && !fixture.done);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  signal_flag(&fixture.release_entry);
  require(pthread_join(mutator, NULL) == 0);
  require(pthread_join(collector, NULL) == 0);
  require(lua_gettop(fixture.child) == 1 &&
          lua_tointeger(fixture.child, -1) == 42);
  lua_close(fixture.main);
  return 0;
}
