#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <ltable.h>
#include <ltableaccess.h>
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
  lua_State *layout, *writer, *other;
  Table *table;
  lua_Integer key;
  int layout_entered, layout_release, waiting, entered, release, completed;
  int observed, register_wait;
} probe = {.mutex = PTHREAD_MUTEX_INITIALIZER,
           .changed = PTHREAD_COND_INITIALIZER};

static __thread pthread_mutex_t *delay_registration, *unexpected_mutex;

int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  require(mutex != unexpected_mutex);
  if (mutex == delay_registration) {
    delay_registration = NULL;
    require(__real_pthread_mutex_lock(&probe.mutex) == 0);
    probe.observed = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    while (!probe.register_wait)
      require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  return __real_pthread_mutex_lock(mutex);
}

static void hold_writer(void) {
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.entered = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  while (!probe.release)
    require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
}

void __real_luaA_begin(lua_State *, GCObject *, LuaObjectScope *);
void __wrap_luaA_begin(lua_State *L, GCObject *object, LuaObjectScope *scope) {
  __real_luaA_begin(L, object, scope);
  if (object != (GCObject *)probe.table)
    return;
  if (L == probe.layout && !probe.layout_entered) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.layout_entered = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    while (!probe.layout_release)
      require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  } else if (L == probe.writer)
    hold_writer(); /* A queued whole-table fallback must fail the same test. */
}

void __real_luaH_elementstore(lua_State *, Table *, Value *, lu_byte *,
                              const TValue *);
void __wrap_luaH_elementstore(lua_State *L, Table *table, Value *payload,
                              lu_byte *tag, const TValue *value) {
  if (L == probe.writer && table == probe.table)
    hold_writer();
  __real_luaH_elementstore(L, table, payload, tag, value);
}

void __real_luaQ_wait(lua_State *, pthread_mutex_t *, pthread_cond_t *,
                      LuaWaiter **);
void __wrap_luaQ_wait(lua_State *L, pthread_mutex_t *mutex,
                      pthread_cond_t *cond, LuaWaiter **waiters) {
  if (L == probe.writer) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.waiting = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  __real_luaQ_wait(L, mutex, cond, waiters);
}

static void *resize(void *unused) {
  LuaObjectScope scope;
  TValue key, value;
  (void)unused;
  lua_lock(probe.layout);
  luaA_begin(probe.layout, (GCObject *)probe.table, &scope);
  setivalue(&key, 1);
  setivalue(&value, 11);
  require(luaH_assignowned(probe.layout, probe.table, &key, &value, 1));
  luaH_resizearray(probe.layout, probe.table, 8192);
  luaA_end(probe.layout, &scope);
  lua_unlock(probe.layout);
  return NULL;
}

static void *write_value(void *unused) {
  (void)unused;
  lua_pushinteger(probe.writer, 22);
  lua_rawseti(probe.writer, 1, 2);
  return NULL;
}

static void *independent(void *unused) {
  (void)unused;
  lua_pushinteger(probe.other, 33);
  lua_rawseti(probe.other, 1, probe.key);
  require(lua_rawgeti(probe.other, 1, 1) == LUA_TNUMBER);
  require(lua_tointeger(probe.other, -1) == 11);
  require(lua_rawgeti(probe.other, 1, probe.key) == LUA_TNUMBER);
  require(lua_tointeger(probe.other, -1) == 33);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.completed = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  return NULL;
}

static void *late_observer(void *state) {
  lua_State *L = state;
  lua_lock(L);
  delay_registration = &G(L)->world.mutex;
  require(luaH_waitlayout(L, probe.table));
  require(delay_registration == NULL);
  lua_unlock(L);
  return NULL;
}

static void check_late_observer(lua_State *L) {
  LuaObjectScope scope;
  struct timespec deadline;
  pthread_t observer;
  TValue key, value;
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  lua_lock(L);
  luaA_begin(L, (GCObject *)probe.table, &scope);
  require(pthread_create(&observer, NULL, late_observer, probe.other) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.observed)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  /* Release the observed owner before the waiter can register. */
  luaA_end(L, &scope);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.register_wait = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_join(observer, NULL) == 0);
  /* Subsequent slot writers can finish without acquiring VM admission. */
  setivalue(&key, 2);
  setivalue(&value, 22);
  unexpected_mutex = &G(L)->world.mutex;
  require(luaH_tryset(L, probe.table, &key, &value, 1));
  unexpected_mutex = NULL;
  lua_unlock(L);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  struct timespec deadline;
  pthread_t a, b, c;
  unsigned i;
  require(argc == 2 && L != NULL);
  lua_gc(L, strcmp(argv[1], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, 1024, 0);
  probe.table = (Table *)lua_topointer(L, 1);
  for (i = 1; i <= 1024; i++) {
    lua_pushinteger(L, 0);
    lua_rawseti(L, 1, i);
  }
  probe.layout = lua_newthread(L);
  probe.writer = lua_newthread(L);
  probe.other = lua_newthread(L);
  for (i = 0; i < 3; i++) {
    lua_pushvalue(L, 1);
    lua_xmove(L,
              i == 0   ? probe.layout
              : i == 1 ? probe.writer
                       : probe.other,
              1);
  }
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  require(pthread_create(&a, NULL, resize, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.layout_entered)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_create(&b, NULL, write_value, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.waiting)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  probe.layout_release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  while (!probe.entered)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(probe.table->asize == 8192);
  for (i = 3; i <= 8192; i++)
    if (luaH_writer(G(L), getArrVal(probe.table, i - 1)) !=
        luaH_writer(G(L), getArrVal(probe.table, 1)))
      break;
  require(i <= 8192);
  probe.key = i;
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_create(&c, NULL, independent, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.completed)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  probe.release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0 &&
          pthread_join(c, NULL) == 0);
  check_late_observer(L);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(L, 1, 2) == LUA_TNUMBER && lua_tointeger(L, -1) == 22);
  lua_close(L);
  require(pthread_cond_destroy(&probe.changed) == 0);
  require(pthread_mutex_destroy(&probe.mutex) == 0);
  return 0;
}
