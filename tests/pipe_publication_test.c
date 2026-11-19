#include <laccess.h>
#include <lauxlib.h>
#include <lpipe.h>
#include <lstring.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *first;
  int entered, release;
} probe = {.mutex = PTHREAD_MUTEX_INITIALIZER,
           .changed = PTHREAD_COND_INITIALIZER};

Udata *__real_luaS_newudata(lua_State *, size_t, unsigned short);
Udata *__wrap_luaS_newudata(lua_State *L, size_t size, unsigned short values) {
  if (L == probe.first) {
    /* The metatable is globally cached, but the first pipe has not yet
     * registered a finalizer. A second constructor can already use that cache.
     * GC is stopped so this allocator pause isolates publication from marking.
     */
    require(__atomic_load_n(&G(L)->pipe_meta, __ATOMIC_ACQUIRE) != NULL);
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.entered = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    while (!probe.release)
      require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  return __real_luaS_newudata(L, size, values);
}

static void *construct(void *unused) {
  (void)unused;
  luaP_newpipe(probe.first, 1);
  return NULL;
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate(), *second;
  pthread_t thread;
  struct timespec deadline;
  Table *meta;
  require(argc == 2 && L != NULL);
  lua_gc(L, strcmp(argv[1], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  probe.first = lua_newthread(L);
  second = lua_newthread(L);
  require(G(L)->pipe_meta == NULL);
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  require(pthread_create(&thread, NULL, construct, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.entered)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  luaP_newpipe(second, 1);
  require(lua_gettop(second) == 1 && lua_isuserdata(second, 1));
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_join(thread, NULL) == 0);
  require(lua_gettop(probe.first) == 1 && lua_isuserdata(probe.first, 1));
  meta = __atomic_load_n(&G(L)->pipe_meta, __ATOMIC_ACQUIRE);
  require((__atomic_load_n(&meta->sharing, __ATOMIC_ACQUIRE) &
           LUA_OBJECT_SHARED) != 0);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_getmetatable(probe.first, 1));
  require(lua_getmetatable(second, 1));
  require(lua_topointer(probe.first, -1) == lua_topointer(second, -1));
  lua_close(L);
  require(pthread_cond_destroy(&probe.changed) == 0);
  require(pthread_mutex_destroy(&probe.mutex) == 0);
  return 0;
}
