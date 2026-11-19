#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <lcallback.h>
#include <lualib.h>
#include <pthread.h>
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

static const char graph[] =
    "local r={child={42}}; r.self=r; r[r.child]={43}; "
    "local v=r.child; r.f=function(x) if x then v=x end return v end; "
    "setmetatable(r,{__index={method=r.child}}); "
    "local p=r; for i=1,10000 do p.next={}; p=p.next end; p.back=r; return r";

static int capture(lua_State *L) {
  lua_pushvalue(L, lua_upvalueindex(1));
  return 1;
}

static void create_graph(lua_State *L) {
  int i;
  require(luaL_loadstring(L, graph) == LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  require(lua_gettop(L) == 1);
  lua_newuserdatauv(L, 1, 4);
  for (i = 1; i <= 4; i++) {
    lua_getfield(L, 1, "child");
    require(lua_setiuservalue(L, 2, i));
  }
  lua_setfield(L, 1, "userdata");
  for (i = 0; i < 4; i++)
    lua_getfield(L, 1, "child");
  lua_pushcclosure(L, capture, 4);
  lua_setfield(L, 1, "cclosure");
}

static void check_value(lua_State *L, int index, int shared) {
  const TValue *value = s2v(L->stack.p + index);
  uintptr_t *sharing = luaA_sharing(gcvalue(value));
  uintptr_t state = __atomic_load_n(sharing, __ATOMIC_ACQUIRE);
  require(shared ? (state & LUA_OBJECT_SHARED) != 0 : state == (uintptr_t)L);
}

static void check_graph(lua_State *L, int shared) {
  static const char *fields[] = {"child", "f", "cclosure", "userdata"};
  unsigned i;
  check_value(L, 1, shared);
  for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    lua_getfield(L, 1, fields[i]);
    check_value(L, 2, shared);
    if (i == 1) {
      LClosure *closure = (LClosure *)lua_topointer(L, 2);
      uintptr_t state =
          __atomic_load_n(&closure->upvals[0]->sharing, __ATOMIC_ACQUIRE);
      require(shared ? (state & LUA_OBJECT_SHARED) != 0
                     : state == (uintptr_t)L);
    }
    lua_pop(L, 1);
  }
  lua_getfield(L, 1, "child");
  lua_rawget(L, 1); /* a mutable value reached through a mutable key */
  check_value(L, 2, shared);
  lua_pop(L, 1);
  require(lua_getmetatable(L, 1));
  check_value(L, 2, shared);
  lua_getfield(L, 2, "__index");
  check_value(L, 3, shared);
  lua_pop(L, 2);
  lua_getfield(L, 1, "next");
  for (i = 0; i < 10000; i++) {
    check_value(L, 2, shared);
    lua_getfield(L, 2, "next");
    lua_replace(L, 2);
  }
  require(lua_isnil(L, 2));
  lua_pop(L, 1);
}

struct worker {
  lua_State *L;
  unsigned id;
};
static void *mutate(void *argument) {
  struct worker *worker = argument;
  lua_State *L = worker->L;
  unsigned i;
  for (i = 1; i <= 64; i++) {
    int kind;
    lua_createtable(L, 1, 1);
    lua_pushinteger(L, (lua_Integer)(worker->id * 1000 + i));
    lua_rawseti(L, 2, 1);
    lua_pushvalue(L, 2);
    lua_setfield(L, 2, "self");
    for (kind = 0; kind < 2; kind++) {
      lua_getfield(L, 1, kind == 0 ? "cclosure" : "userdata");
      lua_pushvalue(L, 2);
      if (kind == 0)
        require(lua_setupvalue(L, 3, (int)worker->id + 1) != NULL);
      else
        require(lua_setiuservalue(L, 3, (int)worker->id + 1));
      if (kind == 0)
        require(lua_getupvalue(L, 3, (int)worker->id + 1) != NULL);
      else
        require(lua_getiuservalue(L, 3, (int)worker->id + 1) == LUA_TTABLE);
      require(lua_rawgeti(L, 4, 1) == LUA_TNUMBER);
      require(lua_tointeger(L, 5) == (lua_Integer)(worker->id * 1000 + i));
      lua_pop(L, 3);
    }
    lua_pushvalue(L, 2);
    lua_rawseti(L, 1, (lua_Integer)worker->id + 1);
    lua_pop(L, 1);
    if ((i & 7) == 0)
      lua_gc(L, LUA_GCSTEP, (size_t)0);
  }
  return NULL;
}

static int resurrect(lua_State *L) {
  lua_pushvalue(L, 1);
  lua_setglobal(L, "resurrected");
  return 0;
}

static void finalizer(lua_State *L) {
  lua_State *source = lua_newthread(L);
  create_graph(source);
  check_graph(source, 0);
  require(lua_getmetatable(source, 1));
  lua_pushcfunction(source, resurrect);
  lua_setfield(source, 2, "__gc");
  lua_setmetatable(source, 1);
  check_graph(source, 1);
  lua_settop(source, 0);
  lua_pop(L, 1); /* the original private owner may die before resurrection */
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_getglobal(L, "resurrected") == LUA_TTABLE);
  lua_getfield(L, 1, "child");
  require(lua_rawgeti(L, 2, 1) == LUA_TNUMBER);
  require(lua_tointeger(L, 3) == 42);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  LuaCallback callback;
  struct worker workers[4];
  pthread_t threads[4];
  unsigned i;
  int registered = 0;
  require(argc == 2 && L != NULL);
  luaL_openlibs(L);
  lua_gc(L, strstr(argv[1], "generational") ? LUA_GCGEN : LUA_GCINC);
  if (strncmp(argv[1], "finalizer", 9) == 0) {
    finalizer(L);
    lua_close(L);
    return 0;
  }
  create_graph(L);
  check_graph(L, 0);
  if (strncmp(argv[1], "global", 6) == 0) {
    lua_pushvalue(L, 1);
    lua_setglobal(L, "shared");
  } else if (strncmp(argv[1], "cclosure", 8) == 0) {
    lua_pushnil(L);
    lua_pushcclosure(L, capture, 1);
    lua_pushvalue(L, 2);
    lua_setglobal(L, "holder");
    lua_pushvalue(L, 1);
    require(lua_setupvalue(L, 2, 1) != NULL);
    lua_pop(L, 1);
  } else if (strncmp(argv[1], "uservalue", 9) == 0) {
    lua_newuserdatauv(L, 1, 1);
    lua_pushvalue(L, 2);
    lua_setglobal(L, "holder");
    lua_pushvalue(L, 1);
    require(lua_setiuservalue(L, 2, 1));
    lua_pop(L, 1);
  } else if (strncmp(argv[1], "metatable", 9) == 0) {
    lua_newtable(L);
    lua_pushvalue(L, 2);
    lua_setglobal(L, "holder");
    lua_pushvalue(L, 1);
    lua_setmetatable(L, 2);
    lua_pop(L, 1);
  } else if (strncmp(argv[1], "callback", 8) == 0) {
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, capture, 1);
    require(luaCB_init(L, &callback, 1) == LUA_OK);
    registered = 1;
    lua_pop(L, 1);
  } else
    require(strncmp(argv[1], "xmove", 5) == 0);
  if (strncmp(argv[1], "xmove", 5) != 0)
    check_graph(L, 1);
  for (i = 0; i < 4; i++) {
    workers[i].L = lua_newthread(L);
    workers[i].id = i;
    lua_pushvalue(L, 1);
    lua_xmove(L, workers[i].L, 1);
    if (i == 0)
      check_graph(workers[i].L, 1);
  }
  /* The first transfer must promote the entire cyclic graph before exposure. */
  lua_settop(L, 5);
  for (i = 0; i < 4; i++)
    require(pthread_create(&threads[i], NULL, mutate, &workers[i]) == 0);
  for (i = 0; i < 3; i++)
    lua_gc(L, LUA_GCCOLLECT);
  for (i = 0; i < 4; i++)
    require(pthread_join(threads[i], NULL) == 0);
  lua_gc(L, LUA_GCCOLLECT);
  for (i = 0; i < 4; i++) {
    lua_rawgeti(L, 1, (lua_Integer)i + 1);
    lua_rawgeti(L, -1, 1);
    require(lua_tointeger(L, -1) == (lua_Integer)(i * 1000 + 64));
    lua_pop(L, 2);
  }
  if (registered) {
    luaCB_revoke(&callback);
    require(luaCB_destroy(L, &callback) == LUA_OK);
  }
  lua_close(L);
  return 0;
}
