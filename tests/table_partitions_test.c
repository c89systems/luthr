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
  lua_State *main, *first, *second, *same, *layout;
  Table *table;
  LuaHashPart *first_part;
  lua_Integer denied_key, sparse_key;
  unsigned deny_count;
  unsigned part, deny_after;
  int entered, release, finished, waiting, part_woken, same_done,
      layout_waiting, layout_done;
} probe = {.mutex = PTHREAD_MUTEX_INITIALIZER,
           .changed = PTHREAD_COND_INITIALIZER};

void *__real_luaM_realloc_(lua_State *, void *, size_t, size_t);
void *__wrap_luaM_realloc_(lua_State *L, void *old, size_t bytes, size_t size) {
  if (L == probe.main && old == NULL && size != 0 && probe.deny_after != 0 &&
      --probe.deny_after == 0)
    return NULL;
  return __real_luaM_realloc_(L, old, bytes, size);
}

void __real_luaH_releasepart(lua_State *, Table *, LuaHashPart *);
void __wrap_luaH_releasepart(lua_State *L, Table *table, LuaHashPart *part) {
  if (L == probe.first && table == probe.table && part == probe.first_part) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    if (!probe.entered) {
      probe.entered = 1;
      require(pthread_cond_broadcast(&probe.changed) == 0);
      while (!probe.release)
        require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
    }
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  __real_luaH_releasepart(L, table, part);
}

void __real_luaQ_wait(lua_State *, pthread_mutex_t *, pthread_cond_t *,
                      LuaWaiter **);
void __wrap_luaQ_wait(lua_State *L, pthread_mutex_t *mutex,
                      pthread_cond_t *cond, LuaWaiter **list) {
  if (L == probe.same) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    if (!probe.waiting) {
      require(mutex != &G(L)->world.mutex);
      require(cond == &probe.first_part->changed);
      require(list == &probe.first_part->waiters);
      probe.waiting = 1;
      require(pthread_cond_broadcast(&probe.changed) == 0);
    }
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  if (L == probe.layout) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    if (!probe.layout_waiting) {
      require(cond == &G(L)->world.changed);
      require(__atomic_load_n(&probe.table->sequence, __ATOMIC_ACQUIRE) & 1);
      probe.layout_waiting = 1;
      require(pthread_cond_broadcast(&probe.changed) == 0);
    }
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  __real_luaQ_wait(L, mutex, cond, list);
}

void __real_luaQ_wake(pthread_cond_t *, LuaWaiter **);
void __wrap_luaQ_wake(pthread_cond_t *cond, LuaWaiter **list) {
  __real_luaQ_wake(cond, list);
  if (probe.first_part != NULL && cond == &probe.first_part->changed) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.part_woken = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
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

static void check_sparse_value(lua_State *L) {
  require(lua_rawgeti(L, 1, probe.sparse_key) == LUA_TTABLE);
  require(lua_rawgeti(L, -1, 1) == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == 8002);
  lua_pop(L, 2);
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

static void *contend(void *unused) {
  (void)unused;
  put(probe.same, -1, 7001);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.same_done = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  return NULL;
}

static void *change_metatable(void *unused) {
  (void)unused;
  lua_createtable(probe.layout, 0, 0);
  require(lua_setmetatable(probe.layout, 1));
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.layout_done = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  return NULL;
}

static void *grow_partition(void *unused) {
  lua_Integer key = (lua_Integer)1 << 40;
  const Value *first_block = NULL;
  const LuaPartArray *array;
  unsigned inserted = 0;
  (void)unused;
  /* Update and grow sparse positive keys while another partition is held. */
  put(probe.second, probe.sparse_key, 8001);
  check(probe.second, probe.sparse_key, 8001);
  lua_newtable(probe.second);
  lua_pushinteger(probe.second, 8002);
  lua_rawseti(probe.second, -2, 1);
  lua_rawseti(probe.second, 1, probe.sparse_key);
  check_sparse_value(probe.second);
  while (inserted < 400) {
    if (partition(key) == probe.part) {
      put(probe.second, key, key);
      check(probe.second, key, key);
      ++inserted;
    }
    ++key;
  }
  for (unsigned i = 1; i <= 512; ++i) {
    lua_Integer index = luaH_indexfrompart(probe.part, i);
    put(probe.second, index, i);
    array = luaH_partarray(probe.table, probe.part);
    if (array != NULL && first_block == NULL)
      first_block = array->blocks[0];
  }
  array = luaH_partarray(probe.table, probe.part);
  require(luaH_partcapacity(array) >= 256);
  require(first_block != NULL && array->blocks[0] == first_block);
  for (unsigned i = 1; i <= 512; ++i) {
    lua_Integer index = luaH_indexfrompart(probe.part, i);
    check(probe.second, index, i);
    lua_pushnil(probe.second);
    lua_rawseti(probe.second, 1, index);
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

static void check_dense_parts(lua_State *L) {
  l_mem capacity;
  run(L, "local big=1<<40; "
         "for i=1,8192 do t[i]=i end; assert(rawlen(t)==8192); "
         "for i=1,128 do t[big+i]=-i end; "
         "assert(t[17]==17 and t[big+17.0]==-17 and t[big+129]==nil); "
         "local dense,sparse=0,0; for k,v in pairs(t) do "
         "if type(k)=='number' then "
         "if k>big then assert(v==big-k); sparse=sparse+1 "
         "else assert(k==v); dense=dense+1 end end end; "
         "assert(dense==8192 and sparse==128); "
         "setmetatable(t,nil); "
         "t[2048.0]={42}; collectgarbage('collect'); assert(t[2048][1]==42); "
         "setmetatable(t,{__mode='k'}); "
         "collectgarbage('collect'); assert(t[2048][1]==42); "
         "setmetatable(t,{__mode='v'}); "
         "local keep={84}; t[2047]=keep; "
         "collectgarbage('collect'); collectgarbage('collect'); "
         "assert(t[2048]==nil and t[2047]==keep); "
         "local k=next(t); while k do t[k]=nil; k=next(t,k) end; "
         "assert(next(t)==nil and #t==0)");
  capacity = luaH_arrayslots(probe.table);
  require(capacity >= 4096);
  put(L, 1 << 30, 17);
  check(L, 1 << 30, 17);
  require(luaH_arrayslots(probe.table) == capacity);
}

static void *append_dense_slot(void *state) {
  lua_State *L = state;
  lua_pushinteger(L, 43);
  lua_rawseti(L, -2, 3);
  return NULL;
}

static void check_dense_allocation_failure(lua_State *L) {
  Table *table;
  Node *nodes;
  lua_Integer hash_key = -1, sparse = LUA_MAXINTEGER;
  lua_settop(L, 0);
  lua_createtable(L, 0, 256);
  table = (Table *)lua_topointer(L, 1);
  lua_pushvalue(L, 1);
  lua_xmove(L, probe.second, 1);
  lua_pop(probe.second, 1);
  /* Give partition one a full hash while leaving its array empty. */
  for (unsigned i = 0; i <= 256; ++i) {
    while (partition(sparse) == 1)
      --sparse;
    put(L, sparse--, i);
  }
  require(luaH_hashparts(table) == LUA_HASH_PARTS);
  while (partition(hash_key) != 1)
    --hash_key;
  put(L, hash_key, 31);
  nodes = luaH_hashnodes(table, 1);
  reject(L, 1, 2);
  require(luaH_arrayslots(table) == 0);
  check(L, hash_key, 31);
  put(L, 1, 42);
  check(L, 1, 42);
  require(luaH_hashnodes(table, 1) == nodes);
  require(luaH_arrayslots(table) == 1);
  require(lua_rawlen(L, 1) == 1);
  check(L, hash_key, 31);
  check(L, sparse + 1, 256);
  {
    const LuaPartArray *array = luaH_partarray(table, 1);
    const LuaPartArray *grown;
    LuaTableWriter *element;
    pthread_t writer;
    /* Growing from one to four slots allocates two blocks. Fail the last
     * allocation after the first block and spare hash are prepared. */
    reject(L, 3, 4);
    require(luaH_partarray(table, 1) == array);
    require(luaH_arrayslots(table) == 1);
    check(L, 1, 42);
    check(L, hash_key, 31);
    lua_pushvalue(L, 1);
    lua_xmove(L, probe.second, 1);
    lua_lock(L);
    array = luaH_partarray(table, 1);
    element = luaH_claimwriter(L, table, arrayval(array->blocks[0], 0),
                               table->sequence);
    require(element != NULL);
    require(pthread_create(&writer, NULL, append_dense_slot, probe.second) ==
            0);
    require(pthread_join(writer, NULL) == 0);
    grown = luaH_partarray(table, 1);
    require(grown != array && grown->size == 4 && array->size == 1);
    require(grown->blocks[0] == array->blocks[0]);
    luaH_releasewriter(L, table, element);
    lua_unlock(L);
    lua_pop(probe.second, 1);
    check(L, 3, 43);
  }
  /* A constructor hook can make SETLIST target this published receiver. */
  lua_pushvalue(L, 1);
  lua_pushinteger(L, 92);
  lua_newtable(L);
  lua_pushinteger(L, 93);
  lua_rawseti(L, -2, 1);
  lua_lock(L);
  luaH_setlist(L, table, L->top.p - 3, 2, 3);
  lua_unlock(L);
  lua_pop(L, 3);
  lua_gc(L, LUA_GCCOLLECT);
  check(L, 1, 42);
  check(L, 2, 92);
  require(lua_rawgeti(L, 1, 3) == LUA_TTABLE);
  require(lua_rawgeti(L, -1, 1) == LUA_TNUMBER && lua_tointeger(L, -1) == 93);
  lua_pop(L, 2);
  require(lua_rawlen(L, 1) == 3);
}

static void check_integer_keys(lua_State *L, int count, lua_Integer stride) {
  int linked = 0;
  Table *table;
  int index;
  lua_newtable(L);
  index = lua_gettop(L);
  lua_pushvalue(L, index);
  lua_xmove(L, probe.second, 1);
  lua_pop(probe.second, 1);
  for (int i = 1; i <= count; ++i) {
    lua_pushinteger(L, i);
    lua_rawseti(L, index, -(lua_Integer)i * stride);
  }
  table = (Table *)lua_topointer(L, index);
  require(luaH_hashparts(table) == LUA_HASH_PARTS);
  /* A dense shard of regularly spaced keys should avoid long chains. */
  for (unsigned part = 1; part < LUA_HASH_PARTS; ++part) {
    const Node *nodes = luaH_hashnodes(table, part);
    unsigned size = luaH_nodesize(nodes);
    for (unsigned i = 0; i < size; ++i) {
      const Node *node = nodes + i;
      linked += gnext(node) != 0;
      for (unsigned length = 1; gnext(node) != 0; ++length) {
        require(length < 8);
        node += gnext(node);
      }
    }
  }
  /* Partitioning a dense range should leave almost all keys at home. */
  require(stride != 1 || linked <= count / 16);
  for (int i = 1; i <= count; ++i) {
    require(lua_rawgeti(L, index, -(lua_Integer)i * stride) == LUA_TNUMBER);
    require(lua_tointeger(L, -1) == i);
    lua_pop(L, 1);
  }
  for (int i = 0; i < 512; ++i) {
    lua_Integer key = LUA_MININTEGER + (lua_Integer)i * 0x100000000LL;
    lua_pushinteger(L, i);
    lua_rawseti(L, index, key);
    lua_pushnumber(L, (lua_Number)key);
    require(lua_rawget(L, index) == LUA_TNUMBER);
    require(lua_tointeger(L, -1) == i);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  LuaHashDirectory *directory;
  pthread_t first, second, same, layout;
  struct timespec deadline;
  unsigned oldsize, total = 657;
  lua_Integer key, empty_slot;
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
  probe.same = lua_newthread(L);
  probe.layout = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, probe.first, 1);
  lua_pushvalue(L, 1);
  lua_xmove(L, probe.second, 1);
  lua_pushvalue(L, 1);
  lua_xmove(L, probe.same, 1);
  lua_pushvalue(L, 1);
  lua_xmove(L, probe.layout, 1);
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
  require(probe.table->asize == 0);
  probe.first_part = luaH_part(directory, partition(-1));
  empty_slot = luaH_indexfrompart(partition(-1), 2);
  for (unsigned i = 1; i <= 4; ++i)
    put(L, luaH_indexfrompart(partition(-1), i), i);
  lua_pushnil(L);
  lua_rawseti(L, 1, empty_slot);
  require(luaH_partcapacity(luaH_partarray(probe.table, partition(-1))) >= 2);
  /* Reopen the layout after setting up an empty slot. */
  lua_newtable(L);
  require(lua_setmetatable(L, 1));
  lua_pushnil(L);
  require(lua_setmetatable(L, 1));
  require(!(probe.table->sequence & LUA_TABLE_SPLIT_VALUES));
  probe.part = partition(-1) == 1 ? 2 : 1;
  probe.sparse_key = LUA_MAXINTEGER;
  while (partition(probe.sparse_key) == partition(-1) ||
         partition(probe.sparse_key) == probe.part)
    --probe.sparse_key;
  put(L, probe.sparse_key, 8000);
  oldsize = luaH_nodesize(luaH_hashnodes(probe.table, probe.part));
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  require(pthread_create(&first, NULL, hold_writer, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.entered)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  {
    TValue index, value;
    setivalue(&index, empty_slot);
    setivalue(&value, 23);
    lua_lock(L);
    require(luaH_tryset(L, probe.table, &index, &value, 0));
    require(!(probe.table->sequence & LUA_TABLE_SPLIT_VALUES));
    lua_unlock(L);
    check(L, empty_slot, 23);
  }
  require(pthread_create(&same, NULL, contend, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.waiting)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_create(&second, NULL, grow_partition, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.finished)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(luaH_nodesize(luaH_hashnodes(probe.table, probe.part)) > oldsize);
  require(!probe.same_done);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_create(&layout, NULL, change_metatable, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.layout_waiting && !probe.layout_done)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(probe.layout_waiting && !probe.layout_done);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  /* A partition waiter is notified even while global admission is held.
   * The layout waiter still needs the global predicate to reopen. */
  while (!probe.part_woken)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(!probe.same_done && !probe.layout_done);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
  require(pthread_join(first, NULL) == 0 && pthread_join(second, NULL) == 0);
  require(pthread_join(same, NULL) == 0 && probe.same_done);
  require(pthread_join(layout, NULL) == 0 && probe.layout_done);
  probe.first_part = NULL;
  for (unsigned i = 1; i <= 4; ++i) {
    lua_pushnil(L);
    lua_rawseti(L, 1, luaH_indexfrompart(partition(-1), i));
  }
  lua_gc(L, LUA_GCCOLLECT);
  check_sparse_value(L);
  lua_pushnil(L);
  lua_rawseti(L, 1, probe.sparse_key);
  require(lua_getmetatable(L, 1));
  lua_pop(L, 1);
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
         "assert((k<0 and (v==-k or k==-1 and v==7001)) or "
         "(k>=(1<<40) and k<(1<<41) and k==v)); count=count+1 end; "
         "assert(count==expected and t[-257.0]==257); "
         "for i=1,200 do t[i]=i end; assert(#t==200 and t[-1]==7001); "
         "local mt={__newindex=function(o,k,v) rawset(o,k,v+1) end}; "
         "setmetatable(t,mt); t['missing']=9; assert(t.missing==10); "
         "t[19]=nil; t[19]=9; assert(t[19]==10); t[19]=19; "
         "t[math.maxinteger]=7; assert(t[math.maxinteger]==8); "
         "t[math.maxinteger]=9; assert(t[math.maxinteger]==9); "
         "mt.__newindex=nil; mt.__mode='v'; "
         "for i=1,200 do t['weak'..i]={i} end; "
         "local keep=t.weak1; collectgarbage('collect'); "
         "collectgarbage('collect'); "
         "assert(keep and t.weak1==keep and t.weak2==nil and t[-1]==7001); "
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
  check_dense_parts(L);
  check_integer_keys(L, 8192 * (LUA_HASH_PARTS - 1), 1);
  check_integer_keys(L, 256 * (LUA_HASH_PARTS - 1), 65537);
  check_integer_keys(L, 256 * (LUA_HASH_PARTS - 1), 0x100000000LL);
  check_dense_allocation_failure(L);
  lua_close(L);
  require(pthread_cond_destroy(&probe.changed) == 0);
  require(pthread_mutex_destroy(&probe.mutex) == 0);
  return 0;
}
