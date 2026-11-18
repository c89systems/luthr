#include <laccess.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lgc.h>
#include <lmem.h>
#include <lroots.h>
#include <lstate.h>
#include <lstring.h>
#include <ltable.h>
#include <ltablestorage.h>
#include <ltm.h>
#include <lua.h>
#include <pthread.h>
#include <sched.h>
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

static __thread unsigned measuring, reading;
static unsigned access_calls, mutex_calls;
static void *watched;
static unsigned returned;
static __thread lua_State *relocating;
static __thread unsigned relocations;

void __real_luaA_metafield(lua_State *, const TValue *, TString *,
                           LuaNativeRoot *, int);
void __wrap_luaA_metafield(lua_State *state, const TValue *object, TString *key,
                           LuaNativeRoot *result, int event) {
  if (state == relocating && event == TM_NEWINDEX && relocations == 0) {
    uintptr_t before = (uintptr_t)state->stack.p;
    relocations++;
    require(luaD_reallocstack(state, stacksize(state) + 128, 1));
    require((uintptr_t)state->stack.p != before);
  }
  __real_luaA_metafield(state, object, key, result, event);
}
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  TString *first;
  unsigned entered, release, writer_done;
  lua_State *writer;
};
static struct fixture *fixture;

void __real_luaA_begin(lua_State *, GCObject *, LuaObjectScope *);
void __wrap_luaA_begin(lua_State *state, GCObject *object,
                       LuaObjectScope *scope) {
  if (measuring)
    access_calls++;
  __real_luaA_begin(state, object, scope);
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (measuring)
    mutex_calls++;
  return __real_pthread_mutex_lock(mutex);
}
void __real_luaM_free_(lua_State *, void *, size_t);
void __wrap_luaM_free_(lua_State *state, void *memory, size_t bytes) {
  if (memory == watched)
    __atomic_fetch_add(&returned, 1, __ATOMIC_RELAXED);
  __real_luaM_free_(state, memory, bytes);
}
int __real_luaS_eqstr(TString *, TString *);
int __wrap_luaS_eqstr(TString *a, TString *b) {
  if (reading && b == fixture->first) {
    reading = 0;
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->entered++;
    require(pthread_cond_broadcast(&fixture->changed) == 0);
    while (!fixture->release)
      require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
  }
  return __real_luaS_eqstr(a, b);
}

static void fast(lua_State *state) {
  Table *table;
  TValue key, value, result;
  TString *name;
  uint64_t sequence;
  unsigned i;
  lua_newtable(state);
  lua_pushinteger(state, 42);
  lua_rawseti(state, 1, 1);
  lua_pushinteger(state, 43);
  lua_setfield(state, 1, "answer");
  lua_pushliteral(state, "answer");
  luaX_lockstate(state);
  table = hvalue(s2v(state->stack.p + 1));
  name = tsvalue(s2v(state->top.p - 1));
  sequence = table->sequence;
  access_calls = mutex_calls = 0;
  measuring = 1;
  for (i = 0; i < 10000; i++) {
    require(luaH_getint(state, table, 1, &result) == LUA_VNUMINT);
    require(ivalue(&result) == 42);
    require(luaH_getstr(state, table, name, &result) == LUA_VNUMINT);
    require(ivalue(&result) == 43);
    setfltvalue(&key, 1.0);
    require(luaH_get(state, table, &key, &result) == LUA_VNUMINT);
    require(ivalue(&result) == 42);
    setnilvalue(&key);
    require(tagisempty(luaH_get(state, table, &key, &result)));
  }
  measuring = 0;
  require(access_calls == 0 && mutex_calls == 0);
  require(table->sequence == sequence && table->access.owner == NULL);
  setivalue(&key, 1);
  setivalue(&value, 84);
  luaH_set(state, table, &key, &value);
  require(luaH_getint(state, table, 1, &result) == LUA_VNUMINT);
  require(ivalue(&result) == 84);
  luaX_unlockstate(state);
}

static void resume_epoch(lua_State *state) {
  LuaWorld *world = &G(state)->world;
  luaX_lockstate(state);
  require(pthread_mutex_lock(&world->mutex) == 0);
  luaW_deactivate(state);
  __atomic_fetch_add(&world->storage_epoch, 1, __ATOMIC_SEQ_CST);
  __atomic_store_n(&world->storage_waiting, 1, __ATOMIC_SEQ_CST);
  /* Resumption runs under world.mutex even when a new epoch needs reporting.
   * It must not recursively acquire that mutex to notify storage waiters. */
  luaW_resume(state);
  require(__atomic_load_n(&state->admission->active, __ATOMIC_SEQ_CST) == 1);
  __atomic_store_n(&world->storage_waiting, 0, __ATOMIC_SEQ_CST);
  require(pthread_mutex_unlock(&world->mutex) == 0);
  luaHS_quiescent(state);
  luaX_unlockstate(state);
}

static void batching(lua_State *state) {
  Table *table;
  LuaWorld *world = &G(state)->world;
  uint64_t epoch;
  unsigned i;
  lua_gc(state, LUA_GCSTOP);
  lua_newtable(state);
  luaX_lockstate(state);
  luaHS_reclaim(state, LUAHS_DRAIN);
  table = hvalue(s2v(state->stack.p + 1));
  epoch = world->storage_epoch;
  for (i = 0; i < 16; i++)
    luaH_resize(state, table, 0, i % 2 ? 16 : 32);
  require(state->storage_bytes != 0 && state->storage_bytes < 65536);
  require(world->storage_bytes == 0 && world->storage_epoch == epoch);
  mutex_calls = 0;
  measuring = 1;
  for (i = 0; i < 1000; i++) {
    luaHS_quiescent(state);
    luaHS_reclaim(state, LUAHS_ASSIST);
  }
  measuring = 0;
  require(mutex_calls == 0);
  luaX_unlockstate(state);
  require(state->storage_retired == NULL && state->storage_bytes == 0);
  require(world->storage_epoch == epoch + 1);
  lua_gc(state, LUA_GCCOLLECT);
  require(world->storage_bytes == 0);
}

static void *reader(void *argument) {
  lua_State *state = argument;
  reading = 1;
  lua_gettable(state, 1);
  require(lua_tointeger(state, -1) == 84);
  return NULL;
}

static void *writer(void *argument) {
  struct fixture *test = argument;
  lua_State *state = test->writer;
  Table *table;
  luaX_lockstate(state);
  table = hvalue(s2v(state->stack.p + 1));
  luaH_resize(state, table, 0, 16384);
  luaX_unlockstate(state); /* pressure must wait for the old readers */
  __atomic_store_n(&test->writer_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void retirement(lua_State *state, int pressure, unsigned readers) {
  struct fixture test;
  lua_State *states[2];
  pthread_t threads[2], worker;
  char key[80];
  unsigned i, candidate, hash;
  Table *table;
  TValue name, value;
  memset(&test, 0, sizeof(test));
  fixture = &test;
  require(pthread_mutex_init(&test.mutex, NULL) == 0);
  require(pthread_cond_init(&test.changed, NULL) == 0);
  lua_gc(state, LUA_GCSTOP);
  lua_createtable(state, 0, 2);
  memset(key, 'a', sizeof(key));
  lua_pushlstring(state, key, sizeof(key));
  luaX_lockstate(state);
  test.first = tsvalue(s2v(state->top.p - 1));
  hash = luaS_hashlongstr(test.first) & 1;
  luaX_unlockstate(state);
  for (candidate = 0;; candidate++) {
    snprintf(key + 65, sizeof(key) - 65, "-%u", candidate);
    lua_pushlstring(state, key, sizeof(key));
    luaX_lockstate(state);
    i = luaS_hashlongstr(tsvalue(s2v(state->top.p - 1))) & 1;
    luaX_unlockstate(state);
    if (i == hash)
      break;
    lua_pop(state, 1);
  }
  lua_pushvalue(state, 2);
  lua_pushinteger(state, 1);
  lua_settable(state, 1);
  lua_pushvalue(state, 3);
  lua_pushinteger(state, 42);
  lua_settable(state, 1);
  for (i = 0; i < readers; i++) {
    states[i] = lua_newthread(state);
    lua_pushvalue(state, 1);
    lua_pushvalue(state, 3);
    lua_xmove(state, states[i], 2);
  }
  test.writer = lua_newthread(state);
  lua_pushvalue(state, 1);
  lua_xmove(state, test.writer, 1);
  luaX_lockstate(state);
  table = hvalue(s2v(state->stack.p + 1));
  watched = table->node; /* two-node hash has no Limbox */
  returned = 0;
  luaX_unlockstate(state);
  for (i = 0; i < readers; i++)
    require(pthread_create(&threads[i], NULL, reader, states[i]) == 0);
  require(pthread_mutex_lock(&test.mutex) == 0);
  while (test.entered != readers)
    require(pthread_cond_wait(&test.changed, &test.mutex) == 0);
  require(pthread_mutex_unlock(&test.mutex) == 0);
  luaX_lockstate(state);
  luaH_resize(state, table, 0, pressure ? 8192 : 256);
  setobj(state, &name, s2v(state->stack.p + 3));
  setivalue(&value, 84);
  luaH_set(state, table, &name, &value);
  luaHS_reclaim(state, LUAHS_ASSIST);
  require(G(state)->world.storage_bytes != 0 || state->storage_bytes != 0);
  require(__atomic_load_n(&returned, __ATOMIC_RELAXED) == 0);
  luaX_unlockstate(state);
  if (pressure) {
    require(pthread_create(&worker, NULL, writer, &test) == 0);
    while (!__atomic_load_n(&G(state)->world.storage_waiting, __ATOMIC_ACQUIRE))
      sched_yield();
    require(__atomic_load_n(&test.writer_done, __ATOMIC_ACQUIRE) == 0);
    /* A nonproducer can reach a VM poll while a producer waits for old
     * readers. It must not inherit the producer's pressure wait. */
    luaX_lockstate(state);
    luaHS_reclaim(state, LUAHS_ASSIST);
    luaX_unlockstate(state);
  }
  require(pthread_mutex_lock(&test.mutex) == 0);
  test.release = 1;
  require(pthread_cond_broadcast(&test.changed) == 0);
  require(pthread_mutex_unlock(&test.mutex) == 0);
  for (i = 0; i < readers; i++)
    require(pthread_join(threads[i], NULL) == 0);
  if (pressure)
    require(pthread_join(worker, NULL) == 0);
  luaX_lockstate(state);
  luaHS_reclaim(state, LUAHS_DRAIN);
  require(__atomic_load_n(&returned, __ATOMIC_RELAXED) == 1);
  require(G(state)->world.storage_bytes == 0);
  luaX_unlockstate(state);
  watched = NULL;
  require(pthread_cond_destroy(&test.changed) == 0);
  require(pthread_mutex_destroy(&test.mutex) == 0);
  fixture = NULL;
}

struct allocator {
  lua_Alloc original;
  void *context;
  int deny;
};
static void *allocate(void *context, void *memory, size_t old, size_t size) {
  struct allocator *allocator = context;
  if (allocator->deny && size > old && size >= 131072)
    return NULL;
  return allocator->original(allocator->context, memory, old, size);
}
static int fail_resize(lua_State *state) {
  Table *table;
  int hash = lua_toboolean(state, 2);
  luaX_lockstate(state);
  table = hvalue(s2v(state->ci->func.p + 1));
  luaH_resize(state, table, hash ? 16 : 16384, hash ? 16384 : 2);
  luaX_unlockstate(state);
  return 0;
}
static void failure(lua_State *state, int hash) {
  struct allocator allocator;
  allocator.original = lua_getallocf(state, &allocator.context);
  allocator.deny = 0;
  lua_setallocf(state, allocate, &allocator);
  lua_newtable(state);
  lua_pushinteger(state, 42);
  lua_rawseti(state, 1, 1);
  lua_pushinteger(state, 84);
  lua_setfield(state, 1, "answer");
  lua_pushcfunction(state, fail_resize);
  lua_pushvalue(state, 1);
  lua_pushboolean(state, hash);
  allocator.deny = 1;
  require(lua_pcall(state, 2, 0, 0) == LUA_ERRMEM);
  allocator.deny = 0;
  lua_pop(state, 1);
  lua_rawgeti(state, 1, 1);
  require(lua_tointeger(state, -1) == 42);
  lua_getfield(state, 1, "answer");
  require(lua_tointeger(state, -1) == 84);
  lua_gc(state, LUA_GCCOLLECT);
  lua_setallocf(state, allocator.original, allocator.context);
}

struct mixed {
  lua_State *writer, *readers[2];
  const void *marker;
  unsigned done;
};
struct mixed_reader {
  struct mixed *test;
  unsigned index;
};

static void check_value(lua_State *state, const void *marker) {
  int type = lua_type(state, -1);
  if (type == LUA_TNUMBER) {
    if (lua_isinteger(state, -1)) {
      lua_Integer value = lua_tointeger(state, -1);
      require(value == LUA_MAXINTEGER || value == LUA_MININTEGER);
    } else
      require(lua_tonumber(state, -1) == 3.25);
  } else if (type == LUA_TSTRING) {
    size_t size, i;
    const char *value = lua_tolstring(state, -1, &size);
    require(size == 80);
    for (i = 0; i < size; i++)
      require(value[i] == 'z');
  } else if (type == LUA_TTABLE)
    require(lua_topointer(state, -1) == marker);
  else {
    require(type == LUA_TLIGHTUSERDATA);
    require(lua_touserdata(state, -1) == (void *)(uintptr_t)0x12345678);
  }
  lua_pop(state, 1);
}

static void *mixed_writer(void *argument) {
  struct mixed *test = argument;
  lua_State *state = test->writer;
  unsigned i;
  for (i = 0; i < 5000; i++) {
    switch (i % 6) {
    case 0:
      lua_pushinteger(state, LUA_MAXINTEGER);
      break;
    case 1:
      lua_pushinteger(state, LUA_MININTEGER);
      break;
    case 2:
      lua_pushnumber(state, 3.25);
      break;
    case 3:
      lua_pushvalue(state, 2);
      break;
    case 4:
      lua_pushvalue(state, 3);
      break;
    default:
      lua_pushlightuserdata(state, (void *)(uintptr_t)0x12345678);
      break;
    }
    lua_pushvalue(state, -1);
    lua_setfield(state, 1, "value");
    lua_rawseti(state, 1, 1);
    if (i % 31 == 0) {
      Table *table;
      luaX_lockstate(state);
      table = hvalue(s2v(state->stack.p + 1));
      luaH_resize(state, table, i % 62 ? 8 : 32, i % 62 ? 2 : 64);
      luaX_unlockstate(state);
    }
    if (i % 113 == 0)
      lua_gc(state, LUA_GCCOLLECT);
  }
  __atomic_store_n(&test->done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void *mixed_reader(void *argument) {
  struct mixed_reader *reader = argument;
  struct mixed *test = reader->test;
  lua_State *state = test->readers[reader->index];
  unsigned i;
  for (i = 0; i < 5000 || !__atomic_load_n(&test->done, __ATOMIC_ACQUIRE);
       i++) {
    lua_getfield(state, 1, "value");
    check_value(state, test->marker);
    lua_rawgeti(state, 1, 1);
    check_value(state, test->marker);
  }
  return NULL;
}

static void mixed(lua_State *state, int generational) {
  struct mixed test;
  struct mixed_reader readers[2];
  pthread_t threads[3];
  char value[80];
  unsigned i;
  memset(&test, 0, sizeof(test));
  if (generational)
    lua_gc(state, LUA_GCGEN);
  lua_newtable(state);
  lua_pushinteger(state, LUA_MAXINTEGER);
  lua_setfield(state, 1, "value");
  lua_pushinteger(state, LUA_MININTEGER);
  lua_rawseti(state, 1, 1);
  memset(value, 'z', sizeof(value));
  lua_pushlstring(state, value, sizeof(value));
  lua_newtable(state);
  test.marker = lua_topointer(state, 3);
  test.writer = lua_newthread(state);
  for (i = 1; i <= 3; i++)
    lua_pushvalue(state, (int)i);
  lua_xmove(state, test.writer, 3);
  for (i = 0; i < 2; i++) {
    test.readers[i] = lua_newthread(state);
    lua_pushvalue(state, 1);
    lua_xmove(state, test.readers[i], 1);
    readers[i].test = &test;
    readers[i].index = i;
  }
  require(pthread_create(&threads[0], NULL, mixed_writer, &test) == 0);
  for (i = 0; i < 2; i++)
    require(pthread_create(&threads[i + 1], NULL, mixed_reader, &readers[i]) ==
            0);
  for (i = 0; i < 3; i++)
    require(pthread_join(threads[i], NULL) == 0);
  lua_gc(state, LUA_GCCOLLECT);
  require(G(state)->world.storage_bytes == 0 && state->storage_bytes == 0);
}

static int unexpected_write(lua_State *state) {
  return luaL_error(state, "existing-key assignment invoked __newindex");
}

static void lua_writes(lua_State *state, Table *metatable) {
  Table *table = (Table *)lua_topointer(state, 1);
  uint64_t before, meta_before = metatable == NULL
                                     ? 0
                                     : __atomic_load_n(&metatable->sequence,
                                                       __ATOMIC_ACQUIRE);
  require(luaL_loadstring(state,
                          "local t,k=...; local function up() t.field=31 end; "
                          "t[1]=11; t.field=13; t[k]=17; up(); "
                          "return t[1]+t.field+t[k]") == LUA_OK);
  lua_pushvalue(state, 1);
  lua_pushliteral(state, "lua-key");
  before = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
  require(lua_pcall(state, 2, 1, 0) == LUA_OK);
  require(lua_tointeger(state, -1) == 59);
  require(__atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) - before == 8);
  require(metatable == NULL ||
          __atomic_load_n(&metatable->sequence, __ATOMIC_ACQUIRE) ==
              meta_before);
  lua_pop(state, 1);
}

/* Insertion, including resize, has the same single ownership boundary as an
 * update. Existing values must not acquire the table's unrelated metatable. */
static void writes(lua_State *state, int generational) {
  Table *table, *metatable;
  uint64_t before;
  unsigned i;
  if (generational)
    lua_gc(state, LUA_GCGEN);
  lua_gc(state, LUA_GCSTOP);
  lua_newtable(state);
  table = (Table *)lua_topointer(state, 1);
  before = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
  for (i = 1; i <= 128; i++) {
    char field[32], long_key[96];
    snprintf(field, sizeof(field), "field-%u", i);
    snprintf(long_key, sizeof(long_key),
             "a-long-distinct-table-key-that-is-not-a-short-interned-string-%u",
             i);
    lua_pushinteger(state, i);
    lua_setfield(state, 1, field);
    lua_pushinteger(state, i * 2);
    lua_seti(state, 1, i);
    lua_pushstring(state, long_key);
    lua_pushinteger(state, i * 3);
    lua_settable(state, 1);
  }
  require(__atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) - before ==
          2 * 3 * 128);
  lua_getfield(state, 1, "field-128");
  require(lua_tointeger(state, -1) == 128);
  lua_pop(state, 1);
  lua_rawgeti(state, 1, 128);
  require(lua_tointeger(state, -1) == 256);
  lua_pop(state, 1);
  lua_writes(state, NULL);
  lua_newtable(state);
  metatable = (Table *)lua_topointer(state, -1);
  lua_pushcfunction(state, unexpected_write);
  lua_setfield(state, -2, "__newindex");
  lua_setmetatable(state, 1);
  lua_writes(state, metatable);
  lua_gc(state, LUA_GCCOLLECT);
}

/* Releasing access must retain exactly the receiver root until the caller
 * drops it: collection can run in the gap before a metamethod is called. */
static void retained_root(lua_State *state, int generational) {
  LuaObjectScope scope;
  Table *table;
  TValue value;
  if (generational)
    lua_gc(state, LUA_GCGEN);
  lua_gc(state, LUA_GCSTOP);
  lua_newtable(state);
  lua_pushinteger(state, 42);
  lua_rawseti(state, -2, 1);
  luaX_lockstate(state);
  table = hvalue(s2v(state->top.p - 1));
  watched = table;
  returned = 0;
  luaA_begin(state, obj2gco(table), &scope);
  state->top.p--; /* the retained root becomes the sole reference */
  luaA_endrooted(state, &scope);
  require(state->object_scopes == NULL);
  luaC_fullgc(state, 0);
  require(returned == 0);
  require(luaH_getint(state, table, 1, &value) == LUA_VNUMINT);
  require(ivalue(&value) == 42);
  luaR_poproot(state, &scope.root);
  luaC_fullgc(state, 0);
  require(returned == 1);
  watched = NULL;
  luaX_unlockstate(state);
}

struct moving_allocator {
  lua_Alloc original;
  void *context;
};

static void *move_realloc(void *context, void *memory, size_t old_size,
                          size_t size) {
  struct moving_allocator *allocator = context;
  if (memory != NULL && size != 0) {
    void *replacement = allocator->original(allocator->context, NULL, 0, size);
    if (replacement == NULL)
      return NULL;
    memcpy(replacement, memory, old_size < size ? old_size : size);
    allocator->original(allocator->context, memory, old_size, 0);
    return replacement;
  }
  return allocator->original(allocator->context, memory, old_size, size);
}

static void write_error(lua_State *state, int generational) {
  static const char *const sources[] = {
      "local receiver=17; receiver.value=42",
      "local receiver=17; return function() receiver.value=42 end",
  };
  struct moving_allocator allocator;
  unsigned i;
  if (generational)
    lua_gc(state, LUA_GCGEN);
  lua_gc(state, LUA_GCSTOP);
  allocator.original = lua_getallocf(state, &allocator.context);
  for (i = 0; i < 2; i++) {
    const char *error;
    require(luaL_loadstring(state, sources[i]) == LUA_OK);
    if (i == 1)
      require(lua_pcall(state, 0, 1, 0) == LUA_OK);
    lua_setallocf(state, move_realloc, &allocator);
    relocating = state;
    relocations = 0;
    require(lua_pcall(state, 0, 0, 0) == LUA_ERRRUN);
    relocating = NULL;
    lua_setallocf(state, allocator.original, allocator.context);
    error = lua_tostring(state, -1);
    require(relocations == 1 && error != NULL);
    require(strstr(error, "index a number value") != NULL);
    require(strstr(error, i == 0 ? "(local 'receiver')"
                                 : "(upvalue 'receiver')") != NULL);
    lua_pop(state, 1);
  }
}

int main(int argc, char **argv) {
  lua_State *state = luaL_newstate();
  require(state != NULL && argc == 2);
  if (strncmp(argv[1], "retained_", 9) == 0)
    retained_root(state, strcmp(argv[1], "retained_generational") == 0);
  else if (strncmp(argv[1], "write_error_", 12) == 0)
    write_error(state, strcmp(argv[1], "write_error_generational") == 0);
  else if (strncmp(argv[1], "writes_", 7) == 0)
    writes(state, strcmp(argv[1], "writes_generational") == 0);
  else if (strcmp(argv[1], "mixed_incremental") == 0 ||
           strcmp(argv[1], "mixed_generational") == 0)
    mixed(state, strcmp(argv[1], "mixed_generational") == 0);
  else if (strcmp(argv[1], "resume") == 0)
    resume_epoch(state);
  else if (strcmp(argv[1], "fast") == 0)
    fast(state);
  else if (strcmp(argv[1], "batching") == 0)
    batching(state);
  else if (strcmp(argv[1], "overlap") == 0)
    retirement(state, 0, 2);
  else if (strcmp(argv[1], "retirement") == 0)
    retirement(state, 0, 1);
  else if (strcmp(argv[1], "pressure") == 0)
    retirement(state, 1, 1);
  else {
    require(strcmp(argv[1], "failure") == 0 ||
            strcmp(argv[1], "hash_failure") == 0);
    failure(state, strcmp(argv[1], "hash_failure") == 0);
  }
  lua_close(state);
  return 0;
}
