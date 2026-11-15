#include <lauxlib.h>
#include <lstate.h>
#include <lualib.h>
#include <luthrum/lua_runtime.h>
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
struct fixture {
  lua_State *state;
  luthrum_lua_runtime *runtime;
  const char *mode;
  size_t live;
  unsigned failures, finalized;
  int deny, armed;
};
static struct fixture *active;
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *test = context;
  void *memory;
  if (old == NULL)
    old_size = 0;
  if (size == 0) {
    test->live -= old_size;
    free(old);
    return NULL;
  }
  if (test->deny && size > old_size) {
    test->failures++;
    return NULL;
  }
  memory = malloc(size);
  if (memory != NULL) {
    if (old != NULL) {
      memcpy(memory, old, old_size < size ? old_size : size);
      free(old);
    }
    test->live = test->live - old_size + size;
  }
  return memory;
}
static void *host_allocate(void *context, size_t size) {
  return allocate(context, NULL, 0, size);
}
static void host_free(void *context, void *memory, size_t size) {
  (void)allocate(context, memory, size, 0);
}
static int capture(void *context) {
  active->state = context;
  return 0;
}
static int reopen(lua_State *state) { return luaopen_utf8(state); }
static void *release_string(void *context, void *memory, size_t old_size,
                            size_t size) {
  struct fixture *test = context;
  require(old_size == 8195 && size == 0);
  test->finalized++;
  free(memory);
  return NULL;
}
static void snapshot_poll(lua_State *state, void *context) {
  struct fixture *test = context;
  if (test->armed) {
    lua_Debug frame;
    LuaWorldScope scope;
    test->armed = 0;
    scope = luaW_detach(state);
    require(lua_getstack(state, 0, &frame));
    lua_pushnil(state);
    require(lua_setlocal(state, &frame, 1) != NULL);
    (void)lua_gc(state, LUA_GCCOLLECT);
    (void)lua_gc(state, LUA_GCCOLLECT);
    require(test->finalized == 0); /* Only the helper's private root remains. */
    luaW_attach(state, scope);
  }
}
static void snapshot(struct fixture *test) {
  lua_State *state = test->state;
  int kind;
  require(lua_checkstack(state, 8200));
  for (kind = 0; kind < 6; ++kind) {
    char *bytes = malloc(8195);
    int args = 1, results = 1, status;
    require(bytes != NULL);
    memset(bytes, 'a', 8194);
    bytes[8194] = '\0';
    lua_getglobal(state, "utf8");
    lua_getfield(state, -1,
                 kind == 0                ? "len"
                 : kind == 1              ? "offset"
                 : kind == 2 || kind == 5 ? "codepoint"
                                          : "codes");
    lua_remove(state, -2);
    if (kind == 3 || kind == 4) {
      lua_pushliteral(state, "a");
      lua_pushboolean(state, kind == 4);
      require(lua_pcall(state, 2, 3, 0) == LUA_OK);
      lua_pop(state, 2); /* Keep only the strict/lax iterator. */
    }
    if (kind == 1 || kind == 3 || kind == 4) {
      memset(bytes, 128, 8193);
      bytes[0] = (char)194;
      bytes[8193] = 'z';
    } else if (kind == 5)
      bytes[8193] = (char)255;
    (void)lua_pushexternalstring(state, bytes, 8194, release_string, test);
    if (kind == 1) {
      lua_pushinteger(state, 0);
      lua_pushinteger(state, 4096);
      args = 3;
      results = 2;
    } else if (kind == 2 || kind == 5) {
      lua_pushinteger(state, 1);
      lua_pushinteger(state, -1);
      args = 3;
      results = LUA_MULTRET;
    } else if (kind == 3 || kind == 4) {
      lua_pushinteger(state, 1);
      args = results = 2;
    }
    test->armed = 1;
    test->finalized = 0;
    state->state_poll = snapshot_poll;
    state->state_poll_context = test;
    status = lua_pcall(state, args, results, 0);
    state->state_poll = NULL;
    require(status == (kind == 5 ? LUA_ERRRUN : LUA_OK));
    require(!test->armed && test->finalized == 0);
    if (kind == 0)
      require(lua_tointeger(state, -1) == 8194);
    else if (kind == 1)
      require(lua_tointeger(state, -2) == 1 &&
              lua_tointeger(state, -1) == 8193);
    else if (kind == 2)
      require(lua_gettop(state) == 8194 && lua_tointeger(state, 1) == 97 &&
              lua_tointeger(state, -1) == 97);
    else if (kind != 5)
      require(lua_tointeger(state, -2) == 8194 &&
              lua_tointeger(state, -1) == 122);
    require(state->native_roots == NULL);
    lua_settop(state, 0);
    (void)lua_gc(state, LUA_GCCOLLECT);
    require(test->finalized == 1);
  }
}
static void execute(lua_State *state, const char *source) {
  int status = luaL_loadstring(state, source);
  if (status == LUA_OK)
    status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "utf8 library: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static void *run(void *context) {
  struct fixture *test = context;
  int facade = strstr(test->mode, "facade") != NULL;
  lua_State *state;
  if (facade) {
    static const unsigned char source[] = "require('fixture')";
    luthrum_lua_runtime_allocator_config config = {0};
    config.user = test;
    config.alloc_fn = host_allocate;
    config.realloc_fn = allocate;
    config.free_fn = host_free;
    require(luthrum_lua_runtime_new_with_allocator(&test->runtime, &config) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_openlibs(test->runtime) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_register_c_module(
                test->runtime, "fixture", capture) == LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_run_buffer(
                test->runtime, source, sizeof(source) - 1, "utf8-library", 0,
                NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  } else {
    test->state = lua_newstate(allocate, test, 0);
    require(test->state != NULL);
    luaL_openlibs(test->state);
  }
  state = test->state;
  if (strstr(test->mode, "generational") != NULL)
    (void)lua_gc(state, LUA_GCGEN);
  lua_pushcfunction(state, reopen);
  lua_setglobal(state, "reopen");
  if (strstr(test->mode, "semantics") != NULL) {
    execute(
        state,
        "local u=utf8; local c={0,127,128,2047,2048,0xd7ff,0xe000,"
        "0xffff,0x10000,0x10ffff}; "
        "local bytes={ '\\0','\\127','\\194\\128','\\223\\191',"
        "'\\224\\160\\128','\\237\\159\\191','\\238\\128\\128',"
        "'\\239\\191\\191','\\240\\144\\128\\128','\\244\\143\\191\\191'}; "
        "local s=u.char(table.unpack(c)); assert(s==table.concat(bytes)); "
        "assert(u.len(s)==#c and u.char()=='' and u.len('')==0); "
        "local values=table.pack(u.codepoint(s,1,-1)); assert(values.n==#c); "
        "local pos=1; for i,cp in ipairs(c) do "
        "assert(values[i]==cp and u.char(tostring(cp))==bytes[i]); "
        "local a,b=u.offset(s,i); assert(a==pos and b==pos+#bytes[i]-1); "
        "local x,y=u.offset(s,i-#c-1); assert(x==a and y==b); "
        "for j=a,b do local p,q=u.offset(s,0,j); "
        "assert(p==a and q==b) end; "
        "assert(u.len(s,a,b)==1 and u.codepoint(s,a,b)==cp); "
        "pos=b+1 end; local n=0; for p,cp in u.codes(s) do "
        "n=n+1; assert(cp==c[n] and p==u.offset(s,n)) end; assert(n==#c); "
        "assert(u.offset(s,#c+1)==#s+1 and u.offset(s,#c+2)==nil); "
        "assert(u.offset(s,-#c-1)==nil); "
        "local a,b=u.offset(s,0,#s+1); assert(a==#s+1 and b==a); "
        "assert(select('#',u.codepoint(s,2,1))==0); "
        "assert(u.len(s,-1,-2)==0); "
        "local n=0; for ch in s:gmatch(u.charpattern) do "
        "n=n+1; assert(ch==bytes[n]) end; assert(n==#c); "
        "for _,cp in ipairs({0xd800,0xdfff,0x110000,0x1fffff,"
        "0x200000,0x3ffffff,0x4000000,0x7fffffff}) do "
        "local s=u.char(cp); assert(u.len(s)==nil and u.len(s,1,-1,true)==1); "
        "assert(not pcall(u.codepoint,s)); "
        "assert(u.codepoint(s,1,-1,true)==cp); "
        "local f,t,i=u.codes(s,true); local p,v=f(t,i); "
        "assert(p==1 and v==cp and f(t,p)==nil); "
        "f,t,i=u.codes(s); assert(not pcall(f,t,i)) end");
  } else if (strstr(test->mode, "errors") != NULL) {
    execute(state,
            "local u=utf8; local bad={'\\128','\\192\\128','\\193\\191',"
            "'\\224\\128\\128','\\240\\128\\128\\128','\\194','\\226\\130',"
            "'\\254','\\255','\\244\\144\\128\\128','\\237\\160\\128'}; "
            "for _,s in ipairs(bad) do "
            "local n,p=u.len('a'..s); assert(n==nil and p==2); "
            "assert(not pcall(u.codepoint,s)); "
            "assert(not pcall(function() for _ in u.codes(s) do end end)) end; "
            "local f=u.codes('a'); assert(f('abc',-1)==nil); "
            "assert(not pcall(u.codes,'\\128')); "
            "local bad={function() u.len('a',0) end,"
            "function() u.len('a',3) end,function() u.len('a',1,2) end,"
            "function() u.offset('a',1,0) end,function() u.offset('a',1,3) end,"
            "function() u.offset('\\194\\128',1,2) end,"
            "function() u.codepoint('a',0) end,"
            "function() u.codepoint('a',1,2) end,"
            "function() u.char(-1) end,function() u.char(0x80000000) end,"
            "function() u.char(1.5) end,function() u.len({}) end}; "
            "for _,f in ipairs(bad) do local ok,e=pcall(f); "
            "assert(not ok and type(e)=='string') end; "
            "assert(u.len('123')==3 and u.codepoint('a')==97)");
  } else if (strstr(test->mode, "bindings") != NULL) {
    execute(state,
            "local a=reopen(); for k,v in pairs(a) do assert(v==utf8[k]) end; "
            "local f,s,i=utf8.codes('ab'); local g=a.codes('cd'); "
            "assert(f==g and s=='ab' and i==0); "
            "g=a.codes('ab',true); assert(f~=g); "
            "assert(debug.getupvalue(f,1)==nil and "
            "debug.getupvalue(g,1)==nil); "
            "local p,c=f(s,0); assert(p==1 and c==97); "
            "local q,d=g(s,0); assert(q==p and d==c); "
            "p,c=f(s,p); assert(p==2 and c==98); "
            "q,d=g(s,q); assert(q==p and d==c); "
            "assert(f(s,p)==nil and g(s,q)==nil); "
            "collectgarbage('collect'); assert(reopen().len==a.len)");
  } else if (strstr(test->mode, "snapshot") != NULL) {
    snapshot(test);
  } else {
    int index;
    require(strstr(test->mode, "oom") != NULL);
    execute(state, "preload='a'; assert(utf8.char(97)=='a')");
    require(lua_checkstack(state, 4098));
    lua_getglobal(state, "utf8");
    lua_getfield(state, -1, "char");
    lua_remove(state, -2);
    for (index = 0; index < 4096; ++index)
      lua_pushinteger(state, 97);
    test->deny = 1;
    require(lua_pcall(state, 4096, 1, 0) == LUA_ERRMEM);
    test->deny = 0;
    require(test->failures != 0);
    lua_pop(state, 1);
    (void)lua_gc(state, LUA_GCCOLLECT);
    execute(state, "assert(utf8.char(97,98,99)=='abc'); "
                   "assert(utf8.len('abc')==3)");
  }
  require(state->object_scopes == NULL && state->native_roots == NULL);
  if (facade)
    luthrum_lua_runtime_free(test->runtime);
  else
    lua_close(state);
  require(test->live == 0);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture test = {0};
  pthread_t worker;
  require(argc == 2);
  active = &test;
  test.mode = argv[1];
  require(pthread_create(&worker, NULL, run, &test) == 0);
  require(pthread_join(worker, NULL) == 0);
  return 0;
}
