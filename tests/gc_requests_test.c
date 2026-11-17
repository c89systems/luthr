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

void __real_luaW_heapenter(lua_State *);
void __wrap_luaW_heapenter(lua_State *L) {
  if (role != 0) {
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    fixture.arrived[role - 1]++;
    require(pthread_cond_broadcast(&fixture.changed) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
  }
  __real_luaW_heapenter(L);
}

struct request {
  lua_State *state;
  unsigned role;
};
static void *request(void *argument) {
  struct request *r = argument;
  role = r->role;
  lua_lock(r->state);
  luaC_step(r->state);
  lua_unlock(r->state);
  if (role == 2) {
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    fixture.done = 1;
    require(pthread_cond_broadcast(&fixture.changed) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
  }
  return NULL;
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  struct request requests[2];
  pthread_t workers[2];
  unsigned round;
  int explicit_step, stop;
  require(L != NULL && argc == 2);
  explicit_step = strcmp(argv[1], "explicit") == 0;
  stop = strcmp(argv[1], "stopped") == 0;
  require(explicit_step || stop || strcmp(argv[1], "incremental") == 0 ||
          strcmp(argv[1], "generational") == 0);
  requests[0].state = lua_newthread(L);
  requests[1].state = lua_newthread(L);
  requests[0].role = 1;
  requests[1].role = 2;
  if (strcmp(argv[1], "generational") == 0)
    lua_gc(L, LUA_GCGEN);
  lua_gc(L, LUA_GCCOLLECT);
  for (round = 0; round < 2; round++) {
    int skipped;
    fixture.arrived[0] = fixture.arrived[1] = fixture.done = 0;
    lua_lock(L);
    luaW_heapenter(L);
    luaE_setdebt(G(L), 0);
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
    require(skipped); /* One pending request must not queue redundant peers. */
    if (stop)
      lua_gc(L, LUA_GCRESTART);
  }
  lua_close(L);
  require(pthread_cond_destroy(&fixture.changed) == 0);
  require(pthread_mutex_destroy(&fixture.mutex) == 0);
  return 0;
}
