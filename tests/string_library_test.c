#include <lauxlib.h>
#include <lstate.h>
#include <lualib.h>
#include <luthr/lua_runtime.h>
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
struct input {
  char *bytes;
  size_t size;
  unsigned released;
};
struct fixture {
  lua_State *state;
  luthr_lua_runtime *runtime;
  const char *mode;
  size_t live;
  size_t large_size;
  unsigned large_requests;
  unsigned remaining, failures, finalized;
  int deny, armed, args;
  struct input inputs[2];
};
static struct fixture *active;
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *test = context;
  void *memory;
  if (old == NULL)
    old_size = 0;
  if (test->large_size && size >= test->large_size && size > old_size)
    test->large_requests++;
  if (size == 0) {
    test->live -= old_size;
    free(old);
    return NULL;
  }
  if (test->deny && size > old_size) {
    if (test->remaining)
      test->remaining--;
    else {
      test->failures++;
      return NULL;
    }
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
static int capture(void *state) {
  active->state = state;
  return 0;
}
static int reopen(lua_State *state) { return luaopen_string(state); }
static int rebind(lua_State *state) {
  lua_CFunction binding = lua_tocfunction(state, 1);
  require(binding != NULL);
  lua_pushcfunction(state, binding);
  return 1;
}
static void execute(lua_State *state, const char *source) {
  int status = luaL_loadstring(state, source);
  if (status == LUA_OK)
    status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "string library: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static void function(lua_State *state, const char *name) {
  lua_getglobal(state, "string");
  lua_getfield(state, -1, name);
  lua_remove(state, -2);
}
static void *release_input(void *context, void *memory, size_t old_size,
                           size_t size) {
  struct input *input = context;
  require(memory == input->bytes && old_size == input->size && size == 0);
  input->released++;
  free(memory);
  return NULL;
}
static void input(lua_State *state, unsigned id, size_t len, int fill,
                  const char *prefix) {
  struct input *input = &active->inputs[id];
  input->size = len + 1;
  input->bytes = malloc(len + 1);
  require(input->bytes != NULL);
  memset(input->bytes, fill, len);
  memcpy(input->bytes, prefix, strlen(prefix));
  input->bytes[len] = 0;
  (void)lua_pushexternalstring(state, input->bytes, len, release_input, input);
}
static void snapshot_poll(lua_State *state, void *context) {
  struct fixture *test = context;
  if (test->armed) {
    lua_Debug frame;
    LuaWorldScope scope = luaW_detach(state);
    int arg;
    test->armed = 0;
    require(lua_getstack(state, 0, &frame));
    for (arg = 1; arg <= test->args; ++arg) {
      lua_pushnil(state);
      require(lua_setlocal(state, &frame, arg) != NULL);
    }
    (void)lua_gc(state, LUA_GCCOLLECT);
    (void)lua_gc(state, LUA_GCCOLLECT);
    require(test->inputs[0].released == 0 && test->inputs[1].released == 0);
    require(test->finalized == 0);
    luaW_attach(state, scope);
  }
}
static int finalize(lua_State *state) {
  (void)state;
  active->finalized++;
  return 0;
}
static int tostring_snapshot(lua_State *state) {
  lua_Debug frame;
  int arg;
  require(lua_getstack(state, 1, &frame));
  for (arg = 1; arg <= 2; ++arg) {
    lua_pushnil(state);
    require(lua_setlocal(state, &frame, arg) != NULL);
  }
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  require(active->inputs[0].released == 0 && active->finalized == 0);
  input(state, 1, 8194, 'R', "");
  active->armed = 1;
  return 1;
}
static void snapshot(struct fixture *test) {
  static const char *const names[] = {
      "sub",      "reverse", "lower",  "upper",  "rep",    "byte",  "pack",
      "packsize", "unpack",  "format", "format", "format", "format"};
  lua_State *state = test->state;
  unsigned kind;
  require(lua_checkstack(state, 8300));
  for (kind = 0; kind < sizeof(names) / sizeof(names[0]); ++kind) {
    int status, results = 1, inputs = 1;
    size_t expected = 8194, len;
    const char *result;
    memset(test->inputs, 0, sizeof(test->inputs));
    test->finalized = 0;
    function(state, names[kind]);
    test->args = 1;
    if (kind == 6 || kind == 8) {
      input(state, 0, 2050, ' ', "c8194");
      input(state, 1, 8194, 'a', "");
      inputs = test->args = 2;
      if (kind == 8)
        results = 2;
    } else if (kind == 7)
      input(state, 0, 8194, 'x', "");
    else if (kind >= 9) {
      input(state, 0, kind == 9 ? 8194 : 2050, 'R',
            kind == 9    ? ""
            : kind == 11 ? "%q"
                         : "%s");
      if (kind >= 10) {
        inputs = test->args = 2;
        if (kind == 12) {
          (void)lua_newuserdatauv(state, 1, 0);
          lua_newtable(state);
          lua_pushcfunction(state, tostring_snapshot);
          lua_setfield(state, -2, "__tostring");
          lua_pushcfunction(state, finalize);
          lua_setfield(state, -2, "__gc");
          lua_setmetatable(state, -2);
        } else
          input(state, 1, 8194, 'a', "");
        expected = kind == 11 ? 10244 : 10242;
      }
    } else {
      input(state, 0, 8194, kind == 2 ? 'A' : 'a', "");
      if (kind == 0) {
        lua_pushinteger(state, 1);
        lua_pushinteger(state, -1);
        test->args = 3;
      } else if (kind == 4) {
        lua_pushinteger(state, 2);
        input(state, 1, 2050, '-', "");
        test->args = 3;
        inputs = 2;
        expected = 18438;
      } else if (kind == 5) {
        lua_pushinteger(state, 1);
        lua_pushinteger(state, -1);
        test->args = 3;
        results = LUA_MULTRET;
      }
    }
    test->armed = kind != 12;
    state->state_poll = snapshot_poll;
    state->state_poll_context = test;
    status = lua_pcall(state, test->args, results, 0);
    state->state_poll = NULL;
    if (status != LUA_OK)
      fprintf(stderr, "snapshot %u: %s\n", kind, lua_tostring(state, -1));
    require(status == LUA_OK && !test->armed);
    if (kind == 5) {
      int index;
      require(lua_gettop(state) == 8194);
      for (index = 1; index <= 8194; ++index)
        require(lua_tointeger(state, index) == 'a');
    } else if (kind == 7)
      require(lua_tointeger(state, 1) == 8194);
    else {
      result = lua_tolstring(state, 1, &len);
      require(result != NULL && len == expected);
      require(result[0] == (kind == 3                 ? 'A'
                            : kind == 9 || kind == 12 ? 'R'
                            : kind == 11              ? '"'
                                                      : 'a'));
      if (kind == 8)
        require(lua_tointeger(state, 2) == 8195);
    }
    require(state->native_roots == NULL && state->object_scopes == NULL);
    lua_settop(state, 0);
    (void)lua_gc(state, LUA_GCCOLLECT);
    (void)lua_gc(state, LUA_GCCOLLECT);
    for (int id = 0; id < inputs; ++id)
      require(test->inputs[id].released == 1);
    require(test->finalized == (kind == 12 ? 1u : 0u));
  }
}
static void oom(struct fixture *test) {
  static const char *const operations[] = {
      "return string.sub(text,1)",
      "return string.reverse(text)",
      "return string.lower(text)",
      "return string.upper(text)",
      "return string.rep(text,2,text)",
      "return string.byte(text,1,-1)",
      "return string.char(table.unpack(numbers))",
      "return string.format('%q:%s',text,text)",
      "return string.pack('c16388s4z',text,text,text)",
      "return string.unpack('c8194c8194',text..text)"};
  lua_State *state = test->state;
  unsigned kind;
  execute(state, "text=string.rep('a',8194); numbers={}; for i=1,8194 do "
                 "numbers[i]=97 end");
  require(lua_checkstack(state, 8300));
  for (kind = 0; kind < sizeof(operations) / sizeof(operations[0]); ++kind) {
    unsigned budget;
    int complete = 0;
    require(luaL_loadstring(state, operations[kind]) == LUA_OK);
    for (budget = 0; budget < 64 && !complete; ++budget) {
      int status;
      lua_pushvalue(state, 1);
      test->remaining = budget;
      test->failures = 0;
      test->deny = 1;
      status = lua_pcall(state, 0, LUA_MULTRET, 0);
      test->deny = 0;
      require(state->native_roots == NULL && state->object_scopes == NULL);
      if (status == LUA_OK) {
        require(lua_gettop(state) > 1);
        complete = 1;
      } else
        require(status == LUA_ERRMEM && test->failures != 0);
      lua_settop(state, 1);
      (void)lua_gc(state, LUA_GCCOLLECT);
    }
    require(complete);
    lua_settop(state, 0);
  }
}

static void interrupt_slice(lua_State *state, void *context) {
  struct fixture *test = context;
  if (test->large_requests != 0) {
    state->state_poll = NULL;
    (void)luaL_error(state, "slice interrupted");
  }
}

static void slice(struct fixture *test) {
  lua_State *state = test->state;
  size_t size;
  const char *result;
  execute(state, "text=string.rep('a\\0b\\255',32768)");
  require(luaL_loadstring(state, "return string.sub(text,2,-2)") == LUA_OK);
  test->large_size = 65536;
  require(lua_pcall(state, 0, 1, 0) == LUA_OK);
  test->large_size = 0;
  require(test->large_requests == 1);
  result = lua_tolstring(state, 1, &size);
  require(result != NULL && size == 131070 && result[0] == '\0' &&
          result[size - 1] == 'b');
  lua_settop(state, 0);
  require(luaL_loadstring(state, "return string.sub(text,2,-2)") == LUA_OK);
  test->large_requests = 0;
  test->large_size = 65536;
  state->state_poll = interrupt_slice;
  state->state_poll_context = test;
  require(lua_pcall(state, 0, 1, 0) == LUA_ERRRUN);
  state->state_poll = NULL;
  state->state_poll_context = NULL;
  test->large_size = 0;
  result = lua_tostring(state, -1);
  require(result != NULL && strstr(result, "slice interrupted") != NULL);
  require(state->native_roots == NULL);
  lua_settop(state, 0);
  (void)lua_gc(state, LUA_GCCOLLECT);
}

static void *run(void *context) {
  struct fixture *test = context;
  lua_State *state;
  int facade = strstr(test->mode, "facade") != NULL;
  if (facade) {
    static const unsigned char source[] = "require('fixture')";
    luthr_lua_runtime_allocator_config config = {0};
    config.user = test;
    config.alloc_fn = host_allocate;
    config.realloc_fn = allocate;
    config.free_fn = host_free;
    require(luthr_lua_runtime_new_with_allocator(&test->runtime, &config) ==
            LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_openlibs(test->runtime) == LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_register_c_module(
                test->runtime, "fixture", capture) == LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_run_buffer(test->runtime, source,
                                         sizeof(source) - 1, "string-library",
                                         0, NULL, 0) == LUTHR_LUA_RUNTIME_OK);
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
  if (strstr(test->mode, "semantics") != NULL)
    execute(
        state,
        "local s=string; local z=s.char(0); assert(s.len('a'..z..'b')==3); "
        "assert(s.sub('abcdef',-4,-2)=='cde' and s.sub('a',2)==''); "
        "assert(s.sub(123,0,99)=='123' and s.sub('abc',-99,-99)==''); "
        "assert(s.sub('abc','2',nil)=='bc' and s.sub('abc',2.0)=='bc'); "
        "assert(s.byte(123,nil,nil)==49 and s.byte('abc','-1')==99); "
        "assert(s.sub('abc',math.mininteger,math.maxinteger)=='abc'); "
        "for _,n in ipairs({0,1,39,40,41,1023,1024,1025,2051}) do "
        "local text=s.rep('a'..z..'b'..s.char(255),n//4)..s.rep('Z',n%4); "
        "local copy=s.sub('['..text..']',2,-2); local keys={[copy]=true}; "
        "assert(copy==text and keys[text]); "
        "local data=s.pack('s4',text); local "
        "value,nextpos=s.unpack('s4',data); "
        "assert(value==text and keys[value] and nextpos==#data+1) end; "
        "assert(s.reverse('a'..z..'b')=='b'..z..'a'); "
        "assert(s.lower('AZ-'..z..'09')=='az-'..z..'09' and "
        "s.upper('az')=='AZ'); "
        "assert(s.rep('x',3,'-')=='x-x-x' and s.rep('',3,'ab')=='abab'); "
        "assert(s.rep('x',0)==''); local a,b,c=s.byte('a'..z..'b',1,-1); "
        "assert(a==97 and b==0 and c==98 and select('#',s.byte('',1))==0); "
        "assert(s.char(0,255,97)==z..'\\255a' and s.char()==''); "
        "assert(s.format('%d %04x %.2f %% %s',-12,15,1.25,'hi')=='-12 000f "
        "1.25 % hi'); "
        "assert(s.format('%c:%u:%o:%X:%e:%g',65,12,8,255,1,2)=='A:12:10:FF:1."
        "000000e+00:2'); "
        "assert(s.format('%s', 'a'..z..'b')=='a'..z..'b'); "
        "assert(s.format('%5s:%.2s','a','abcd')=='    a:ab'); "
        "assert(s.format('%99s',s.rep('a',100))==s.rep('a',100)); "
        "for _,v in "
        "ipairs({'a'..z..'1\\n\\\"\\\\',true,false,123,math.mininteger,1.25,"
        "math.huge,-math.huge}) do "
        "local f=assert(load('return '..s.format('%q',v))); assert(f()==v) "
        "end; "
        "assert(s.format('%q',nil)=='nil'); local n=assert(load('return "
        "'..s.format('%q',0/0)))(); assert(n~=n); "
        "local mt={__tostring=function() collectgarbage('collect'); return "
        "'shown' end}; "
        "assert(s.format('%s',setmetatable({},mt))=='shown'); "
        "debug.setmetatable(false,mt); assert(s.format('%q',false)=='shown'); "
        "debug.setmetatable(false,nil); "
        "assert(type(s.format('%p',{}))=='string' and "
        "type(s.format('%p',nil))=='string'); "
        "local fmt='>!4bBi2I2i4I4jJfdnc4s2zx'; "
        "local "
        "p=s.pack(fmt,-1,255,-123,65000,-12345,123456,math.mininteger,-1,1.5,2."
        "5,3.5,'xy','abc','z'); "
        "local t=table.pack(s.unpack(fmt,p)); assert(t.n==15 and t[1]==-1 and "
        "t[2]==255); "
        "assert(t[3]==-123 and t[4]==65000 and t[5]==-12345 and t[6]==123456); "
        "assert(t[7]==math.mininteger and t[8]==-1 and t[9]==1.5 and "
        "t[10]==2.5 and t[11]==3.5); "
        "assert(t[12]=='xy'..z..z and t[13]=='abc' and t[14]=='z' and "
        "t[15]==#p+1); "
        "assert(s.pack('<I2',258)=='\\2\\1' and s.pack('>I2',258)=='\\1\\2'); "
        "assert(s.packsize('>!4bXi4i4c3')==11 and s.packsize('=lLT')==24); "
        "assert(s.packsize('x'..z..'?')==1 and s.pack('c0','')==''); "
        "a,b=s.unpack('>I2','xx'..s.pack('>I2',258),-2); assert(a==258 and "
        "b==5); "
        "a,b=s.unpack('c0',''); assert(a=='' and b==1); "
        "assert(s.unpack('i16',s.pack('i16',-1))==-1)");
  else if (strstr(test->mode, "errors") != NULL)
    execute(
        state,
        "for _,f in ipairs({function() string.sub({},1) end,"
        "function() string.sub() end,function() "
        "string.char(256) end, "
        "function() string.char(-1) end,function() "
        "string.rep('aa',math.maxinteger) end, "
        "function() string.rep('x',0,{}) end,function() string.format('%s') "
        "end, "
        "function() string.format('%q',{}) end,function() "
        "string.format('%5q',1) end, "
        "function() string.format('%n',1) end,function() string.format('%',1) "
        "end, "
        "function() "
        "string.format('%999999999999999999999999999999999999999s','a') end, "
        "function() string.format('%.2s','a'..string.char(0)) end, "
        "function() string.format('%s',setmetatable({}, {__tostring=function() "
        "return {} end})) end, "
        "function() string.pack('b',128) end,function() string.pack('B',-1) "
        "end, "
        "function() string.pack('c1','ab') end,function() "
        "string.pack('z','a'..string.char(0)) end, "
        "function() string.pack('i17',1) end,function() string.pack('i0',1) "
        "end, "
        "function() string.pack('!3i3',1) end,function() "
        "string.packsize('Xc2') end, "
        "function() string.packsize('s') end,function() string.packsize('c') "
        "end, "
        "function() string.unpack('z','abc') end,function() "
        "string.unpack('i4','a') end, "
        "function() string.unpack('s1','\\9abc') end,function() "
        "string.unpack('i1','a',3) end, "
        "function() string.unpack('I16',string.rep('\\255',16)) end}) do "
        "assert(not pcall(f)) end; "
        "assert(string.format('%s:%d','ok',12)=='ok:12')");
  else if (strstr(test->mode, "bindings") != NULL) {
    lua_pushcfunction(state, rebind);
    lua_setglobal(state, "rebind");
    execute(state, "local other=reopen(); for _,k in "
                   "ipairs({'byte','char','format','len','lower','rep','"
                   "reverse','sub','upper','pack','packsize','unpack'}) do "
                   "assert(other[k]==string[k]) end; "
                   "local sub,byte=rebind(other.sub),rebind(other.byte); "
                   "assert(sub(123,'2')=='23' and byte('ab',nil)==97); "
                   "local s=string.rep('ab',1025); "
                   "assert(sub(s,2,-2)==string.sub(s,2,-2)); "
                   "local values=table.pack(byte(s,2,1026)); "
                   "assert(values.n==1025 and values[1]==98 and "
                   "values[1024]==97 and values[1025]==98); "
                   "local ok,err=pcall(sub,'abc',1.5); "
                   "assert(not ok and err:find('no integer representation')); "
                   "ok,err=pcall(byte,'abc',1,{}); "
                   "assert(not ok and err:find('number expected')); "
                   "assert(byte('b')==98)");
  } else if (strstr(test->mode, "snapshot") != NULL)
    snapshot(test);
  else if (strstr(test->mode, "slice") != NULL)
    slice(test);
  else
    oom(test);
  require(state->native_roots == NULL && state->object_scopes == NULL);
  if (facade)
    luthr_lua_runtime_free(test->runtime);
  else
    lua_close(state);
  require(test->live == 0);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *test = calloc(1, sizeof(*test));
  pthread_t worker;
  require(test != NULL && argc == 2);
  active = test;
  test->mode = argv[1];
  require(pthread_create(&worker, NULL, run, test) == 0);
  require(pthread_join(worker, NULL) == 0);
  free(test);
  return 0;
}
