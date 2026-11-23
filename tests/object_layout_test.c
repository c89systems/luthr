#include <lapi.h>
#include <lauxlib.h>
#include <lfunc.h>
#include <lmempage.h>
#include <ltable.h>
#include <stdint.h>
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

static void budget(const void *object, size_t maximum) {
  const LuaAllocBlock *block = (const LuaAllocBlock *)object - 1;
  const LuaAllocPage *page = luaMP_page(block);
  require(page != NULL);
  if (page->blockbytes > maximum) {
    fprintf(stderr, "object allocation: %zu bytes, budget %zu\n",
            (size_t)page->blockbytes, maximum);
    abort();
  }
}

static int answer(lua_State *L) {
  lua_pushinteger(L, 42);
  return 1;
}

static void small_table(lua_State *L, lua_State *other, int from_lua) {
  const Table *table;
  unsigned i;
  lua_createtable(L, 2, 0);
  table = lua_topointer(L, -1);
  require(table != NULL && table->asize == 2);
  budget(table, 112);
  lua_pop(L, 1);
  lua_createtable(L, 0, 2);
  table = lua_topointer(L, -1);
  require(table != NULL && table->inline_bytes >= 2 * sizeof(Node));
  budget(table, 128);
  lua_pop(L, 1);
  lua_createtable(L, 0, 4);
  table = lua_topointer(L, -1);
  require(table != NULL && table->inline_bytes >= 4 * sizeof(Node));
  budget(table, 192);
  lua_pop(L, 1);
  if (from_lua) {
    require(luaL_loadstring(L, "return {17, 42, 31, answer=43}") == LUA_OK);
    require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  } else {
    lua_createtable(L, 3, 1);
    lua_pushinteger(L, 42);
    lua_rawseti(L, -2, 2);
    lua_pushinteger(L, 43);
    lua_setfield(L, -2, "answer");
  }
  table = lua_topointer(L, -1);
  require(table != NULL && table->inline_bytes != 0);
  budget(table, 160);
  require((uintptr_t)(table->array - table->asize) == (uintptr_t)(table + 1));
  require((uintptr_t)table->node - (uintptr_t)(table + 1) <
          table->inline_bytes);
  lua_xmove(L, other, 1);
  for (i = 4; i <= 256; i++) {
    lua_pushinteger(other, i);
    lua_rawseti(other, 1, i);
    lua_pushinteger(other, i * 2);
    lua_rawseti(other, 1, -(lua_Integer)i);
  }
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_topointer(other, 1) == table);
  lua_rawgeti(other, 1, 2);
  require(lua_tointeger(other, -1) == 42);
  lua_pop(other, 1);
  lua_getfield(other, 1, "answer");
  require(lua_tointeger(other, -1) == 43);
  lua_pop(other, 1);
  for (i = 4; i <= 256; i++) {
    lua_rawgeti(other, 1, i);
    require(lua_tointeger(other, -1) == i);
    lua_pop(other, 1);
    lua_rawgeti(other, 1, -(lua_Integer)i);
    require(lua_tointeger(other, -1) == i * 2);
    lua_pop(other, 1);
  }
  lua_pop(other, 1);
  lua_gc(L, LUA_GCCOLLECT);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate(), *other;
  int userdata, capture;
  require(argc == 3 && L != NULL);
  lua_gc(L, strcmp(argv[2], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  other = lua_newthread(L);
  if (strncmp(argv[1], "table_", 6) == 0) {
    require(strcmp(argv[1], "table_lua") == 0 ||
            strcmp(argv[1], "table_api") == 0);
    small_table(L, other, strcmp(argv[1], "table_lua") == 0);
    lua_close(L);
    return 0;
  }
  userdata = strncmp(argv[1], "userdata", 8) == 0;
  capture = strcmp(argv[1], "capture") == 0 || strcmp(argv[1], "cell") == 0;
  if (userdata) {
    int values = strcmp(argv[1], "userdata_values") == 0;
    unsigned char *payload = lua_newuserdatauv(L, 48, values);
    require((uintptr_t)payload % sizeof(UValue) == 0);
    memset(payload, 0xa5, 48);
    budget(gcvalue(s2v(L->stack.p + 2)), values ? 256 : 128);
    if (values) {
      lua_newtable(L);
      lua_pushinteger(L, 43);
      lua_setfield(L, -2, "answer");
      require(lua_setiuservalue(L, 2, 1));
    }
    require(luaL_loadstring(L, "return {__index={answer=42}}") == LUA_OK);
    require(lua_pcall(L, 0, 1, 0) == LUA_OK);
    require(lua_setmetatable(L, 2));
  } else if (strcmp(argv[1], "managed") == 0) {
    luaQ_pushmanaged(L, answer, 0);
    lua_pushcfunction(L, answer);
    require(lua_iscfunction(L, 2) && lua_tocfunction(L, 2) == answer);
    require(lua_upvalueid(L, 2, 1) == NULL);
    require(lua_rawequal(L, 2, 3) && lua_compare(L, 2, 3, LUA_OPEQ));
    require(lua_topointer(L, 2) == lua_topointer(L, 3));
    lua_createtable(L, 0, 1);
    lua_pushvalue(L, 2);
    lua_pushinteger(L, 43);
    lua_rawset(L, 4);
    lua_pushvalue(L, 3);
    require(lua_rawget(L, 4) == LUA_TNUMBER && lua_tointeger(L, -1) == 43);
    lua_pop(L, 1);
    lua_xmove(L, other, 1);
    lua_pushcfunction(other, answer);
    require(lua_rawget(other, 1) == LUA_TNUMBER &&
            lua_tointeger(other, -1) == 43);
    lua_pop(other, 1);
    lua_pushcfunction(other, answer);
    require(lua_next(other, 1) == 0);
    lua_pop(other, 1);
    lua_pop(L, 1);
  } else {
    require(capture || strcmp(argv[1], "closure") == 0);
    require(luaL_loadstring(
                L, capture ? "local v=42; return function() return v end"
                           : "return function() return 42 end") == LUA_OK);
    require(lua_pcall(L, 0, 1, 0) == LUA_OK);
    if (strcmp(argv[1], "cell") != 0)
      budget(lua_topointer(L, 2), 64);
    if (capture) {
      LClosure *closure = (LClosure *)lua_topointer(L, 2);
      budget(closure->upvals[0], 128);
    }
  }
  lua_xmove(L, other, 1);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  if (userdata) {
    const unsigned char *payload = lua_touserdata(other, 1);
    unsigned i;
    require(lua_rawlen(other, 1) == 48);
    for (i = 0; i < 48; i++)
      require(payload[i] == 0xa5);
    require(lua_getfield(other, 1, "answer") == LUA_TNUMBER);
    require(lua_tointeger(other, -1) == 42);
    lua_pop(other, 1);
    if (strcmp(argv[1], "userdata_values") == 0) {
      require(lua_getiuservalue(other, 1, 1) == LUA_TTABLE);
      require(lua_getfield(other, -1, "answer") == LUA_TNUMBER);
      require(lua_tointeger(other, -1) == 43);
    }
  } else {
    require(lua_pcall(other, 0, 1, 0) == LUA_OK);
    require(lua_tointeger(other, 1) == 42);
  }
  lua_close(L);
  return 0;
}
