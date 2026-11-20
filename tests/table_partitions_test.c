#include <lapi.h>
#include <lauxlib.h>
#include <lstate.h>
#include <ltable.h>
#include <ltableaccess.h>
#include <lualib.h>
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
  lua_State *main, *first, *second;
  Table *table;
  LuaTableWriter *first_writer;
  lua_Integer denied_key;
  unsigned deny_count;
  unsigned part, deny_after;
  int entered, release, finished;
} probe = {.mutex = PTHREAD_MUTEX_INITIALIZER,
           .changed = PTHREAD_COND_INITIALIZER};

void *__real_luaM_realloc_(lua_State *, void *, size_t, size_t);
void *__wrap_luaM_realloc_(lua_State *L, void *old, size_t bytes, size_t size) {
  if (L == probe.main && old == NULL && size != 0 && probe.deny_after != 0 &&
      --probe.deny_after == 0)
    return NULL;
  return __real_luaM_realloc_(L, old, bytes, size);
}

void __real_luaH_releasewriter(lua_State *, Table *, LuaTableWriter *);
void __wrap_luaH_releasewriter(lua_State *L, Table *table,
                               LuaTableWriter *row) {
  if (L == probe.first && table == probe.table && row == probe.first_writer) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    if (!probe.entered) {
      probe.entered = 1;
      require(pthread_cond_broadcast(&probe.changed) == 0);
      while (!probe.release)
        require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
    }
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  __real_luaH_releasewriter(L, table, row);
}

static unsigned partition(lua_Integer key) {
  lua_Unsigned bits = (lua_Unsigned)key;
  return 1 + (unsigned)((bits ^ (bits >> 16)) % (LUA_HASH_PARTS - 1));
}

static void put(lua_State *L, lua_Integer key, lua_Integer number) {
  lua_pushinteger(L, number);
  lua_rawseti(L, 1, key);
}

static void check(lua_State *L, lua_Integer key, lua_Integer number) {
  require(lua_rawgeti(L, 1, key) == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == number);
  lua_pop(L, 1);
}

static int reject_insert(lua_State *L) {
  probe.deny_after = probe.deny_count;
  put(L, probe.denied_key, -probe.denied_key);
  return 0;
}

static void reject(lua_State *L, lua_Integer key, unsigned allocation) {
  probe.denied_key = key;
  probe.deny_count = allocation;
  lua_pushcfunction(L, reject_insert);
  lua_pushvalue(L, 1);
  require(lua_pcall(L, 1, 0, 0) == LUA_ERRMEM);
  lua_pop(L, 1);
  require(probe.deny_after == 0);
  require(lua_rawgeti(L, 1, key) == LUA_TNIL);
  lua_pop(L, 1);
}

static void *hold_writer(void *unused) {
  (void)unused;
  put(probe.first, -1, 7000);
  return NULL;
}

static void *grow_partition(void *unused) {
  lua_Integer key = -1024;
  unsigned inserted = 0;
  (void)unused;
  while (inserted < 400) {
    if (partition(key) == probe.part) {
      put(probe.second, key, -key);
      check(probe.second, key, -key);
      ++inserted;
    }
    --key;
  }
  check(probe.second, -1, 7000);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.finished = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  return NULL;
}

static void run(lua_State *L, const char *source) {
  int status = luaL_dostring(L, source);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  require(status == LUA_OK);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  LuaHashDirectory *directory;
  pthread_t first, second;
  struct timespec deadline;
  unsigned oldsize, total = 657;
  lua_Integer key;
  require(argc == 2);
  require(L != NULL);
  probe.main = L;
  luaL_openlibs(L);
  lua_gc(L, strcmp(argv[1], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, 0, 256);
  probe.table = (Table *)lua_topointer(L, 1);
  probe.first = lua_newthread(L);
  probe.second = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, probe.first, 1);
  lua_pushvalue(L, 1);
  lua_xmove(L, probe.second, 1);
  for (key = 1; key <= 256; ++key)
    put(L, -key, key);
  require(luaH_hashparts(probe.table) == 1);
  reject(L, -257, 6);
  require(luaH_hashparts(probe.table) == 1);
  for (key = 1; key <= 256; ++key)
    check(L, -key, key);
  put(L, -257, 257);
  directory = luaH_hashdir(probe.table->node);
  require(directory != NULL);
  probe.first_writer =
      luaH_writer(G(L), (Value *)&directory->parts[partition(-1)]);
  for (probe.part = 1; probe.part < LUA_HASH_PARTS; ++probe.part)
    if (luaH_writer(G(L), (Value *)&directory->parts[probe.part]) !=
        probe.first_writer)
      break;
  require(probe.part < LUA_HASH_PARTS);
  oldsize = luaH_nodesize(luaH_hashnodes(probe.table, probe.part));
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  require(pthread_create(&first, NULL, hold_writer, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.entered)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_create(&second, NULL, grow_partition, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.finished)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(luaH_nodesize(luaH_hashnodes(probe.table, probe.part)) > oldsize);
  probe.release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_join(first, NULL) == 0 && pthread_join(second, NULL) == 0);
  {
    Node *nodes = luaH_hashnodes(probe.table, probe.part);
    unsigned size = luaH_nodesize(nodes);
    unsigned filled = 0;
    for (unsigned i = 0; i < size; ++i)
      filled += !isempty(gval(nodes + i));
    key = -16384;
    for (; filled < size; --key) {
      if (partition(key) == probe.part) {
        put(L, key, -key);
        ++filled;
        ++total;
      }
    }
    while (partition(key) != probe.part)
      --key;
    reject(L, key, 1);
    require(luaH_hashnodes(probe.table, probe.part) == nodes);
    put(L, key, -key);
    check(L, key, -key);
    ++total;
  }
  lua_pushinteger(L, total);
  lua_setglobal(L, "expected");
  lua_pushvalue(L, 1);
  lua_setglobal(L, "t");
  run(L, "local count=0; for k,v in pairs(t) do "
         "assert(k<0 and (v==-k or k==-1 and v==7000)); count=count+1 end; "
         "assert(count==expected and t[-257.0]==257); "
         "for i=1,200 do t[i]=i end; assert(#t==200 and t[-1]==7000); "
         "local mt={__newindex=function(o,k,v) rawset(o,k,v+1) end}; "
         "setmetatable(t,mt); t['missing']=9; assert(t.missing==10); "
         "mt.__newindex=nil; mt.__mode='v'; "
         "for i=1,200 do t['weak'..i]={i} end; "
         "local keep=t.weak1; collectgarbage('collect'); "
         "collectgarbage('collect'); "
         "assert(keep and t.weak1==keep and t.weak2==nil and t[-1]==7000); "
         "mt.__mode='k'; local k={}; t[k]={k}; "
         "collectgarbage('collect'); assert(t[k][1]==k); "
         "k=nil; collectgarbage('collect'); collectgarbage('collect'); "
         "for key in pairs(t) do assert(type(key)~='table'); t[key]=nil end; "
         "assert(next(t)==nil)");
  lua_pushexternalstring(L, "external", 8, NULL, NULL);
  lua_pushinteger(L, 1337);
  lua_rawset(L, 1);
  require(lua_getfield(L, 1, "external") == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == 1337);
  lua_pop(L, 1);
  lua_pushexternalstring(L, "external", 8, NULL, NULL);
  require(lua_rawget(L, 1) == LUA_TNUMBER && lua_tointeger(L, -1) == 1337);
  lua_close(L);
  require(pthread_cond_destroy(&probe.changed) == 0);
  require(pthread_mutex_destroy(&probe.mutex) == 0);
  return 0;
}
