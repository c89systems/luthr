#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
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
  lua_State *source;
  GCObject *root;
  int armed, entered, release;
} probe = {.mutex = PTHREAD_MUTEX_INITIALIZER,
           .changed = PTHREAD_COND_INITIALIZER};

void __real_luaA_end(lua_State *, LuaObjectScope *);
void __wrap_luaA_end(lua_State *L, LuaObjectScope *scope) {
  int hold = probe.armed && L == probe.source &&
             iscollectable(&scope->root.value) &&
             gcvalue(&scope->root.value) == probe.root;
  __real_luaA_end(L, scope);
  if (hold) {
    LuaWorld *world = &G(L)->world;
    /* Model the ordinary ownership wait that promotion can encounter. Its
     * pending weak children must remain roots across every intervening epoch.
     * Retain the gate and publish the parked native/root state as that wait
     * does.
     */
    require(pthread_mutex_lock(&world->mutex) == 0);
    L->native_depth++;
    luaW_deactivate(L);
    require(pthread_mutex_unlock(&world->mutex) == 0);
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.entered = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    while (!probe.release)
      require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
    require(pthread_mutex_lock(&world->mutex) == 0);
    while (luaW_stopped(world))
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
    luaW_resume(L);
    luaC_checksnapshot(L);
    L->native_depth--;
    require(pthread_mutex_unlock(&world->mutex) == 0);
  }
}

static void *promote(void *unused) {
  (void)unused;
  lua_lock(probe.source);
  luaA_share(probe.source, probe.root);
  lua_unlock(probe.source);
  return NULL;
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  pthread_t thread;
  struct timespec deadline;
  uint64_t epoch;
  unsigned i;
  int minor;
  require(argc == 2 && L != NULL);
  minor = strcmp(argv[1], "minor") == 0;
  lua_gc(L,
         minor || strcmp(argv[1], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  probe.source = lua_newthread(L);
  lua_newtable(probe.source);
  probe.root = (GCObject *)lua_topointer(probe.source, 1);
  lua_newtable(probe.source);
  lua_pushliteral(probe.source, "v");
  lua_setfield(probe.source, 2, "__mode");
  lua_setmetatable(probe.source, 1);
  for (i = 1; i <= 2; i++) {
    unsigned slot = minor ? 3 - i : i;
    lua_createtable(probe.source, 1, 0);
    lua_pushinteger(probe.source, (lua_Integer)slot + 41);
    lua_rawseti(probe.source, 2, 1);
    /* The final pending head is old, while its temporary successor is young.
     * A root descriptor alone cannot make an old head's edge visible to minors.
     */
    if (minor && i == 1)
      lua_gc(L, LUA_GCCOLLECT);
    lua_rawseti(probe.source, 1, (lua_Integer)slot);
  }
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  probe.armed = 1;
  require(pthread_create(&thread, NULL, promote, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.entered)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  epoch = G(L)->trace_epoch;
  for (i = 0; i < 64 && G(L)->trace_epoch - epoch < 3; i++) {
    if (minor) {
      require(G(L)->gckind == KGC_GENMINOR);
      lua_newtable(L); /* Give each step a young object to collect. */
      lua_pop(L, 1);
      lua_gc(L, LUA_GCSTEP, (size_t)0);
      lua_gc(L, LUA_GCCOUNT);
    } else
      lua_gc(L, LUA_GCCOLLECT);
  }
  require(G(L)->trace_epoch - epoch >= 3);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_join(thread, NULL) == 0);
  require(probe.source->sharing_roots == NULL);
  for (i = 1; i <= 2; i++) {
    require(lua_rawgeti(probe.source, 1, (lua_Integer)i) == LUA_TTABLE);
    require(lua_rawgeti(probe.source, 2, 1) == LUA_TNUMBER);
    require(lua_tointeger(probe.source, 3) == (lua_Integer)i + 41);
    lua_pop(probe.source, 2);
  }
  /* The roots belong only to the active promotion, never to the shared bit. */
  for (i = 0; i < 2; i++)
    lua_gc(L, LUA_GCCOLLECT);
  for (i = 1; i <= 2; i++) {
    require(lua_rawgeti(probe.source, 1, (lua_Integer)i) == LUA_TNIL);
    lua_pop(probe.source, 1);
  }
  lua_close(L);
  require(pthread_cond_destroy(&probe.changed) == 0);
  require(pthread_mutex_destroy(&probe.mutex) == 0);
  return 0;
}
