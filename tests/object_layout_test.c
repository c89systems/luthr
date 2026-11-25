#include <lapi.h>
#include <lauxlib.h>
#include <lfunc.h>
#include <lmempage.h>
#include <ltable.h>
#include <lualib.h>
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
  require((uintptr_t)(luaH_array(table) - table->asize) ==
          (uintptr_t)(table + 1));
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

static void run_chunk(lua_State *L, const char *source, int results) {
  int status = luaL_loadstring(L, source);
  if (status == LUA_OK)
    status = lua_pcall(L, 0, results, 0);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  require(status == LUA_OK);
}

static void record_constructor_hooks(lua_State *L) {
  run_chunk(L,
            "local function make(n) return {key=n, value=n+1} end\n"
            "local reversed = {value=-1, key=-2}\n"
            "local writes = {}\n"
            "local function during_constructor(action)\n"
            "  local visits = 0\n"
            "  debug.sethook(function()\n"
            "    if debug.getinfo(2, 'f').func == make then\n"
            "      local _, object = debug.getlocal(2, 2)\n"
            "      if type(object) == 'table' and not next(object) then\n"
            "        visits = visits + 1\n"
            "        debug.sethook()\n"
            "        if action == 'replace' then\n"
            "          assert(debug.setlocal(2, 2, reversed))\n"
            "        else\n"
            "          setmetatable(object, {__newindex = writes})\n"
            "        end\n"
            "      end\n"
            "    end\n"
            "  end, '', 1)\n"
            "  local object = make(41)\n"
            "  debug.sethook()\n"
            "  assert(visits == 1)\n"
            "  return object\n"
            "end\n"
            "assert(during_constructor('replace') == reversed)\n"
            "assert(reversed.key == 41 and reversed.value == 42)\n"
            "assert(next(during_constructor('metatable')) == nil)\n"
            "assert(writes.key == 41 and writes.value == 42)\n",
            0);
}

static void record_table(lua_State *L, lua_State *other) {
  const Table *record, *second, *keys, *metatable;
  luaL_openlibs(L);
  run_chunk(L,
            "local function check(t) "
            "  for i=1,64 do "
            "    t.key=i; assert(t.key==i and math.type(t.key)=='integer'); "
            "    t.key=false; assert(t.key==false); "
            "    t.key=1.25; assert(t.key==1.25); "
            "    collectgarbage('step',0) "
            "  end; "
            "  t.key={answer=42}; collectgarbage('collect'); "
            "  assert(t.key.answer==42); t.key=nil; "
            "  local redirects={}; setmetatable(t,{__newindex=redirects}); "
            "  t.key=7; assert(rawget(t,'key')==nil and redirects.key==7) "
            "end; check({key=1,value=2}); check({0,key=1})",
            0);
  run_chunk(L,
            "local function make(n) return {key=n, value=n+1} end; "
            "make = assert(load(string.dump(make), 'record', 'b')); "
            "return make(10), make(20)",
            2);
  record = lua_topointer(L, 2);
  second = lua_topointer(L, 3);
  keys = luaH_recordkeys(record->node);
  require(keys != NULL && keys == luaH_recordkeys(second->node));
  require(record != second && lua_rawlen(L, 2) == 0);
  budget(record, 96);
  lua_pop(L, 1);
  lua_pushexternalstring(L, "key", 3, NULL, NULL);
  require(lua_rawget(L, 2) == LUA_TNUMBER && lua_tointeger(L, -1) == 10);
  lua_pop(L, 1);
  lua_xmove(L, other, 1);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_topointer(other, 1) == record);
  require(luaH_recordkeys(record->node) == keys);
  require(lua_rawgeti(other, 1, 1) == LUA_TNIL);
  lua_pop(other, 1);
  lua_pushexternalstring(other, "key", 3, NULL, NULL);
  require(lua_rawget(other, 1) == LUA_TNUMBER &&
          lua_tointeger(other, -1) == 10);
  lua_pop(other, 1);
  lua_pushexternalstring(other, "key", 3, NULL, NULL);
  lua_pushinteger(other, 42);
  lua_rawset(other, 1);
  lua_getfield(other, 1, "key");
  require(lua_tointeger(other, -1) == 42);
  lua_pop(other, 1);
  lua_pushnil(other);
  lua_rawseti(other, 1, 9999);
  require(luaH_recordkeys(record->node) == keys);
  lua_pushnil(other);
  require(lua_next(other, 1));
  lua_pop(other, 1);
  lua_pushvalue(other, -1);
  lua_pushnil(other);
  lua_rawset(other, 1);
  require(lua_next(other, 1));
  lua_pop(other, 1);
  require(lua_next(other, 1) == 0);
  lua_pushinteger(other, 43);
  lua_rawseti(other, 1, 1);
  require(luaH_recordkeys(record->node) == NULL);
  require(lua_topointer(other, 1) == record);
  require(lua_rawgeti(other, 1, 1) == LUA_TNUMBER);
  require(lua_tointeger(other, -1) == 43);
  lua_pop(other, 1);
  lua_pop(other, 1);

  run_chunk(L,
            "local r = {key=1, value=2}; "
            "local index, newindex = {value=11, extra=12}, {}; "
            "local m = {__index=index, __newindex=newindex}; "
            "return r,m",
            2);
  record = lua_topointer(L, 2);
  metatable = lua_topointer(L, 3);
  keys = luaH_recordkeys(record->node);
  require(keys != NULL && luaH_recordkeys(metatable->node) != NULL);
  require(lua_setmetatable(L, 2));
  require(keys == luaH_recordkeys(record->node));
  lua_setglobal(L, "record_fixture");
  run_chunk(L,
            "local t = record_fixture; record_fixture = nil; "
            "local m = getmetatable(t); "
            "t.key = 3; assert(rawget(t,'key') == 3); "
            "rawset(t,'value',nil); assert(t.value == 11 and t.extra == 12); "
            "t.value = 8; t.extra = 9; "
            "assert(rawget(t,'value') == nil and rawget(t,'extra') == nil); "
            "assert(m.__newindex.value == 8 and m.__newindex.extra == 9); "
            "return t",
            1);
  require(luaH_recordkeys(record->node) == keys);
  lua_pushliteral(L, "extra");
  lua_pushinteger(L, 14);
  lua_rawset(L, 2);
  require(luaH_recordkeys(record->node) == NULL);
  require(lua_getfield(L, 2, "key") == LUA_TNUMBER &&
          lua_tointeger(L, -1) == 3);
  lua_pop(L, 1);
  lua_pop(L, 1);

  run_chunk(L,
            "local edge = {}; local n = math.mininteger; "
            "return {integer=n, float=1.25, edge=edge, name='record'}",
            1);
  record = lua_topointer(L, 2);
  keys = luaH_recordkeys(record->node);
  require(keys != NULL && keys->asize == 4);
  budget(record, 112);
  lua_xmove(L, other, 1);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_getfield(other, 1, "integer") == LUA_TNUMBER &&
          lua_isinteger(other, -1) &&
          lua_tointeger(other, -1) == LUA_MININTEGER);
  lua_pop(other, 1);
  require(lua_getfield(other, 1, "float") == LUA_TNUMBER &&
          lua_tonumber(other, -1) == 1.25);
  lua_pop(other, 1);
  require(lua_getfield(other, 1, "edge") == LUA_TTABLE);
  lua_pop(other, 1);
  require(lua_getfield(other, 1, "name") == LUA_TSTRING &&
          strcmp(lua_tostring(other, -1), "record") == 0);
  lua_pop(other, 2);

  run_chunk(L,
            "local function make(mode) "
            "  local key, value = {}, 'weak-'..mode; "
            "  local record = {key=key, value=value}; "
            "  return setmetatable(record, {__mode=mode, sentinel=true}) "
            "end; return make('k'), make('v'), make('kv')",
            3);
  for (int i = 2; i <= 4; i++) {
    const Table *weak = lua_topointer(L, i);
    require(luaH_recordkeys(weak->node) != NULL);
  }
  lua_xmove(L, other, 3);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  for (int i = 1; i <= 3; i++) {
    const Table *weak = lua_topointer(other, i);
    require(luaH_recordkeys(weak->node) != NULL);
    require(lua_getfield(other, i, "key") == (i == 1 ? LUA_TTABLE : LUA_TNIL));
    lua_pop(other, 1);
    require(lua_getfield(other, i, "value") == LUA_TSTRING);
    require(strcmp(lua_tostring(other, -1), i == 1   ? "weak-k"
                                            : i == 2 ? "weak-v"
                                                     : "weak-kv") == 0);
    lua_pop(other, 1);
  }
  lua_pop(other, 3);
  record_constructor_hooks(L);

  run_chunk(
      L,
      "local function field(a,b) return ({first=a, second=b}).first end; "
      "local function item(a,b) return ({a,b})[2] end; "
      "local function absent(a) return ({first=a}).missing end; "
      "local function constant(a) return ({first=a, second=23}).second end; "
      "local function repeated(a) return ({first=1, first=a}).first end; "
      "local function choose(key,a,b) return ({first=a, second=b})[key] end; "
      "local function selectat(key,a,b) return ({a,b})[key] end; "
      "local value={}; "
      "assert(field(value,false)==value and item(false,value)==value); "
      "assert(field(nil,false)==nil and item(false,nil)==nil); "
      "assert(absent(value)==nil and constant(value)==23); "
      "assert(repeated(value)==value); "
      "assert(choose('first',value,17)==value and choose('second',0,17)==17); "
      "assert(choose(nil,value,17)==nil and choose({},value,17)==nil); "
      "assert(selectat(2.0,17,value)==value and selectat(1,17,value)==17); "
      "assert(selectat(2.5,17,value)==nil and selectat('2',17,value)==nil); "
      "local function keeps(a,b) "
      "  local t={first=a,second=b}; return t.first,t "
      "end; "
      "local picked, surviving=keeps(value,23); "
      "assert(picked==value and surviving.second==23); "
      "local calls=0; local function effect() calls=calls+1; return value end; "
      "assert(({first=effect(),second=0}).first==value and calls==1); "
      "assert(assert(load(string.dump(field,true),nil,'b'))(value,0)==value); "
      "assert(assert(load(string.dump(item,true),nil,'b'))(0,value)==value); "
      "collectgarbage('collect'); collectgarbage('stop'); "
      "local before=collectgarbage('count'); "
      "for i=1,6000 do "
      "  assert(field(i,0)==i and item(0,i)==i and "
      "         choose('second',0,i)==i and selectat(2,0,i)==i); "
      "end; "
      "assert(collectgarbage('count')-before<32); collectgarbage('restart'); "
      "local observed=false; "
      "debug.sethook(function() "
      "  if debug.getinfo(2,'f').func ~= field then return end; "
      "  for i=1,8 do "
      "    local name, slot=debug.getlocal(2,i); "
      "    if not name then break end; "
      "    if type(slot)=='table' and rawget(slot,'first')==101 then "
      "      rawset(slot,'first',137); observed=true; "
      "    end; "
      "  end; "
      "end,'',1); "
      "local hooked=field(101,105); debug.sethook(); "
      "assert(observed and hooked==137)",
      0);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate(), *other;
  int userdata, capture;
  require(argc == 3 && L != NULL);
  lua_gc(L, strcmp(argv[2], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  other = lua_newthread(L);
  if (strncmp(argv[1], "table_", 6) == 0) {
    if (strcmp(argv[1], "table_record") == 0)
      record_table(L, other);
    else {
      require(strcmp(argv[1], "table_lua") == 0 ||
              strcmp(argv[1], "table_api") == 0);
      small_table(L, other, strcmp(argv[1], "table_lua") == 0);
    }
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
