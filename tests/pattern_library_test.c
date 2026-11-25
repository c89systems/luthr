#define _GNU_SOURCE
#include <lauxlib.h>
#include <lstate.h>
#include <lualib.h>
#include <luthr/lua_runtime.h>
#include <pthread.h>
#include <sched.h>
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
enum { TOKENS = 4096 };
struct input {
  char *memory;
  size_t size;
  unsigned released;
};
struct fixture;
struct actor {
  struct fixture *test;
  lua_State *state;
  int cpu, status, results;
  unsigned polls;
};
struct fixture {
  lua_State *state;
  luthr_lua_runtime *runtime;
  const char *mode;
  size_t live;
  unsigned failures, remaining, arrivals, finalized;
  int deny, armed;
  void *old_userdata;
  struct input inputs[3];
  pthread_barrier_t barrier;
  struct actor actors[2];
  lua_Integer tokens[TOKENS];
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
    if (test->remaining != 0)
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
static int capture(void *context) {
  active->state = context;
  return 0;
}
static void execute(lua_State *state, const char *source) {
  int status = luaL_loadstring(state, source);
  if (status == LUA_OK)
    status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "pattern library: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static void function(lua_State *state, const char *name) {
  lua_getglobal(state, "string");
  lua_getfield(state, -1, name);
  lua_remove(state, -2);
}
static int reopen(lua_State *state) { return luaopen_string(state); }
static int bad_userdata(lua_State *state) {
  size_t size = lua_rawlen(state, 1);
  void *memory = lua_newuserdatauv(state, size, 2);
  memset(memory, 0, size);
  return 1;
}
static int strip(lua_State *state) {
  lua_CFunction fn = lua_tocfunction(state, 1);
  require(fn != NULL);
  lua_pushcfunction(state, fn);
  return 1;
}
static int finalize(lua_State *state) {
  require(lua_touserdata(state, 1) == active->old_userdata);
  active->finalized++;
  return 0;
}
static void *release_input(void *context, void *memory, size_t old_size,
                           size_t size) {
  struct input *input = context;
  require(memory == input->memory && old_size == input->size && size == 0);
  input->released++;
  free(memory);
  return NULL;
}
static void input(lua_State *state, struct input *input, size_t len, int fill,
                  const char *suffix) {
  input->memory = malloc(len + 1);
  require(input->memory != NULL);
  input->size = len + 1;
  memset(input->memory, fill, len);
  memcpy(input->memory + len - strlen(suffix), suffix, strlen(suffix));
  input->memory[len] = '\0';
  (void)lua_pushexternalstring(state, input->memory, len, release_input, input);
}
static int replace_snapshot(lua_State *state) {
  lua_Debug frame;
  unsigned index;
  require(lua_getstack(state, 1, &frame));
  for (index = 1; index <= 3; ++index) {
    lua_pushnil(state);
    require(lua_setlocal(state, &frame, (int)index) != NULL);
  }
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  require(active->inputs[0].released == 0);
  input(state, &active->inputs[2], 8194, 'R', "");
  active->armed = 1;
  return 1;
}
static void result_poll(lua_State *state, void *context) {
  struct fixture *test = context;
  if (test->armed) {
    LuaWorldScope scope = luaW_detach(state);
    test->armed = 0;
    (void)lua_gc(state, LUA_GCCOLLECT);
    (void)lua_gc(state, LUA_GCCOLLECT);
    require(test->inputs[0].released == 0 && test->inputs[2].released == 0);
    luaW_attach(state, scope);
  }
}
static void callback_snapshot(struct fixture *test) {
  lua_State *state = test->state;
  size_t len, index;
  const char *result;
  function(state, "gsub");
  input(state, &test->inputs[0], 8194, 'a', "z");
  lua_pushliteral(state, "a+z");
  lua_pushcfunction(state, replace_snapshot);
  state->state_poll = result_poll;
  state->state_poll_context = test;
  require(lua_pcall(state, 3, 2, 0) == LUA_OK);
  state->state_poll = NULL;
  require(!test->armed && lua_tointeger(state, -1) == 1);
  result = lua_tolstring(state, 1, &len);
  require(result != NULL && len == 8194);
  for (index = 0; index < len; ++index)
    require(result[index] == 'R');
  lua_settop(state, 0);
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  require(test->inputs[0].released == 1 && test->inputs[2].released == 1);
}
static void snapshot_poll(lua_State *state, void *context) {
  struct fixture *test = context;
  unsigned index;
  int promote = strstr(test->mode, "promote") != NULL;
  int iterator = strstr(test->mode, "iterator") != NULL;
  int values = strstr(test->mode, "values") != NULL || promote;
  if (test->armed) {
    lua_Debug frame;
    LuaWorldScope scope;
    test->armed = 0;
    scope = luaW_detach(state);
    require(lua_getstack(state, 0, &frame));
    if (iterator || values) {
      require(lua_getinfo(state, "f", &frame));
      if (promote) {
        lua_pushvalue(state, -1);
        lua_setglobal(state, "shared_iterator");
        for (index = 1; index <= 2; ++index) {
          lua_pushnil(state);
          require(lua_setupvalue(state, -2, (int)index) != NULL);
        }
      }
      if (values) {
        require(lua_getupvalue(state, -1, 3) != NULL);
        for (index = 1; index <= 2; ++index) {
          lua_pushnil(state);
          require(lua_setiuservalue(state, -2, (int)index));
        }
        lua_pop(state, 1);
      } else {
        lua_pushnil(state);
        require(lua_setupvalue(state, -2, 3) != NULL);
      }
      lua_pop(state, 1);
    } else {
      unsigned args = strstr(test->mode, "gsub") != NULL ? 3 : 2;
      for (index = 1; index <= args; ++index) {
        lua_pushnil(state);
        require(lua_setlocal(state, &frame, (int)index) != NULL);
      }
    }
    (void)lua_gc(state, LUA_GCCOLLECT);
    (void)lua_gc(state, LUA_GCCOLLECT);
    for (index = 0; index < 3; ++index)
      require(test->inputs[index].released == 0);
    require(test->finalized == 0);
    luaW_attach(state, scope);
  }
}
static void snapshot(struct fixture *test) {
  lua_State *state = test->state;
  int find = strstr(test->mode, "find") != NULL;
  int gsub = strstr(test->mode, "gsub") != NULL;
  int byteclass = strstr(test->mode, "class") != NULL;
  int promote = strstr(test->mode, "promote") != NULL;
  int values = strstr(test->mode, "values") != NULL || promote;
  int iterator = strstr(test->mode, "iterator") != NULL || values;
  int args = gsub ? 3 : find ? 4 : iterator ? 0 : 2;
  int status;
  function(state, find       ? "find"
                  : gsub     ? "gsub"
                  : iterator ? "gmatch"
                             : "match");
  input(state, &test->inputs[0], 8194, 'a', "z");
  if (byteclass)
    input(state, &test->inputs[1], 3, 'a', "[z]");
  else
    input(state, &test->inputs[1], find ? 2049 : 2050, 'a', find ? "z" : "()");
  if (gsub)
    input(state, &test->inputs[2], 2050, 'R', "");
  else if (find) {
    lua_pushinteger(state, 1);
    lua_pushboolean(state, 1);
  }
  if (iterator) {
    require(lua_pcall(state, 2, 1, 0) == LUA_OK);
    if (!promote) {
      require(lua_getupvalue(state, -1, 3) != NULL);
      test->old_userdata = lua_touserdata(state, -1);
      require(test->old_userdata != NULL);
      lua_newtable(state);
      lua_pushcfunction(state, finalize);
      lua_setfield(state, -2, "__gc");
      lua_setmetatable(state, -2);
      lua_pop(state, 1);
    }
  }
  test->armed = 1;
  state->state_poll = snapshot_poll;
  state->state_poll_context = test;
  status = lua_pcall(state, args, LUA_MULTRET, 0);
  state->state_poll = NULL;
  require(!test->armed && status == (values ? LUA_ERRRUN : LUA_OK));
  if (find)
    require(lua_gettop(state) == 2 && lua_tointeger(state, 1) == 6146 &&
            lua_tointeger(state, 2) == 8194);
  else if (gsub) {
    size_t len;
    const char *result = lua_tolstring(state, 1, &len);
    require(result != NULL);
    if (byteclass)
      require(len == 10243 && result[8192] == 'a' && result[8193] == 'R' &&
              result[len - 1] == 'R' && lua_tointeger(state, 2) == 1);
    else
      require(len == 8202 && result[len - 2] == 'a' && result[len - 1] == 'z' &&
              lua_tointeger(state, 2) == 4);
  } else if (!values)
    require(lua_gettop(state) == 1 && lua_tointeger(state, 1) == 2049);
  require(state->native_roots == NULL && state->object_scopes == NULL);
  lua_settop(state, 0);
  if (promote) {
    lua_getglobal(state, "shared_iterator");
    require(lua_type(state, -1) == LUA_TFUNCTION);
    require(lua_pcall(state, 0, 1, 0) == LUA_ERRRUN);
    lua_pop(state, 1);
    lua_pushnil(state);
    lua_setglobal(state, "shared_iterator");
  }
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state,
               LUA_GCCOLLECT); /* Retire finalized iterator and its values. */
  for (unsigned index = 0; index < (gsub ? 3u : 2u); ++index)
    require(test->inputs[index].released == 1);
  require(test->finalized == (iterator && !promote ? 1u : 0u));
}
static void overlap_poll(lua_State *state, void *context) {
  struct actor *actor = context;
  if (actor->polls++ == 0) {
    int status;
    unsigned arrivals =
        __atomic_add_fetch(&actor->test->arrivals, 1, __ATOMIC_SEQ_CST);
    if (arrivals == 2) {
      require(pthread_mutex_lock(&G(state)->world.mutex) == 0);
      require(luaW_activecount(&G(state)->world) == 2);
      require(pthread_mutex_unlock(&G(state)->world.mutex) == 0);
    }
    status = pthread_barrier_wait(&actor->test->barrier);
    require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  }
}
static void *consume(void *context) {
  struct actor *actor = context;
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  CPU_SET(actor->cpu, &cpus);
  require(pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus) == 0);
  actor->status = lua_resume(actor->state, NULL, 0, &actor->results);
  return NULL;
}
static int compare(const void *left, const void *right) {
  lua_Integer a = *(const lua_Integer *)left, b = *(const lua_Integer *)right;
  return (a > b) - (a < b);
}
static void parallel(struct fixture *test, int stress) {
  lua_State *state = test->state;
  pthread_t actors[2];
  cpu_set_t available;
  int cpus[2] = {-1, -1}, cpu, id, results;
  unsigned count = 0;
  require(sched_getaffinity(0, sizeof(available), &available) == 0);
  for (cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &available)) {
      if (cpus[0] < 0)
        cpus[0] = cpu;
      else {
        cpus[1] = cpu;
        break;
      }
    }
  require(cpus[0] >= 0);
  if (cpus[1] < 0)
    cpus[1] = cpus[0];
  execute(state, stress ? "iterator=string.gmatch(string.rep('x',4096).."
                          "string.rep('a',4096),'()a')"
                        : "iterator=string.gmatch(string.rep('x',4096).."
                          "'a'..string.rep('x',4096)..'a','()a')");
  (void)lua_gc(state, LUA_GCSTOP);
  require(pthread_barrier_init(&test->barrier, NULL, 2) == 0);
  for (id = 0; id < 2; ++id) {
    struct actor *actor = &test->actors[id];
    actor->test = test;
    actor->cpu = cpus[id];
    actor->state = lua_newthread(state);
    require(luaL_loadstring(
                actor->state,
                stress ? "coroutine.yield(); local out={}; "
                         "while true do local n=iterator(); "
                         "if not n then break end; out[#out+1]=n; "
                         "if #out%32==0 then collectgarbage('collect') "
                         "end end; return out"
                       : "coroutine.yield(); return iterator()") == LUA_OK);
    require(lua_resume(actor->state, NULL, 0, &results) == LUA_YIELD);
    actor->state->state_poll = overlap_poll;
    actor->state->state_poll_context = actor;
    actor->state->poll_remaining = 1024;
  }
  for (id = 0; id < 2; ++id)
    require(pthread_create(&actors[id], NULL, consume, &test->actors[id]) == 0);
  for (id = 0; id < 2; ++id) {
    struct actor *actor = &test->actors[id];
    require(pthread_join(actors[id], NULL) == 0);
    if (actor->status != LUA_OK)
      fprintf(stderr, "pattern actor: %s\n", lua_tostring(actor->state, -1));
    require(actor->status == LUA_OK && actor->results == 1);
    require(actor->polls != 0 && actor->state->native_roots == NULL);
    if (stress) {
      size_t n = lua_rawlen(actor->state, -1), index;
      for (index = 1; index <= n; ++index) {
        require(count < TOKENS);
        lua_rawgeti(actor->state, -1, (lua_Integer)index);
        test->tokens[count++] = lua_tointeger(actor->state, -1);
        lua_pop(actor->state, 1);
      }
    } else
      test->tokens[count++] = lua_tointeger(actor->state, -1);
  }
  require(test->arrivals == 2 && count == (stress ? TOKENS : 2));
  qsort(test->tokens, count, sizeof(test->tokens[0]), compare);
  for (unsigned index = 0; index < count; ++index)
    require(test->tokens[index] == (stress       ? 4097 + (lua_Integer)index
                                    : index == 0 ? 4097
                                                 : 8194));
  require(pthread_barrier_destroy(&test->barrier) == 0);
  (void)lua_gc(state, LUA_GCRESTART);
  execute(state, "iterator=nil; collectgarbage('collect')");
}
static void oom(struct fixture *test) {
  lua_State *state = test->state;
  unsigned index, kind;
  execute(state, "string.gmatch('a','a')()");
  for (index = 0; index < 2; ++index) {
    unsigned attempts;
    int top = lua_gettop(state), status = LUA_OK;
    require(lua_checkstack(state, 1028));
    test->remaining = index;
    test->deny = 1;
    /* Retain successful iterators until local pages need a backing refill. */
    for (attempts = 0; attempts < 1024 && status == LUA_OK; ++attempts) {
      function(state, "gmatch");
      lua_pushliteral(state, "a");
      lua_pushliteral(state, "a");
      status = lua_pcall(state, 2, 1, 0);
    }
    test->deny = 0;
    require(status == LUA_ERRMEM);
    require(test->failures != 0 && state->native_roots == NULL);
    lua_settop(state, top);
    (void)lua_gc(state, LUA_GCCOLLECT);
  }
  execute(state, "text=string.rep('a',8194)");
  function(state, "gmatch");
  lua_getglobal(state, "text");
  lua_pushliteral(state, "(a+)");
  require(lua_pcall(state, 2, 1, 0) == LUA_OK);
  lua_pushvalue(state, -1);
  test->remaining = 0;
  test->deny = 1;
  require(lua_pcall(state, 0, 1, 0) == LUA_ERRMEM);
  test->deny = 0;
  require(state->native_roots == NULL);
  lua_pop(state, 1);
  lua_pushvalue(state, -1);
  require(lua_pcall(state, 0, LUA_MULTRET, 0) == LUA_OK);
  require(lua_gettop(state) ==
          1); /* Failed result allocation consumed the match. */
  lua_settop(state, 0);
  function(state, "gsub");
  lua_pushliteral(state, "a");
  lua_pushliteral(state, "z");
  lua_pushinteger(state, 123456789);
  test->failures = 0;
  test->deny = 1;
  require(lua_pcall(state, 3, 2, 0) == LUA_OK);
  test->deny = 0;
  require(test->failures == 0 && lua_tointeger(state, -1) == 0);
  lua_settop(state, 0);
  function(state, "gsub");
  lua_getglobal(state, "text");
  lua_pushliteral(state, "[z]");
  lua_pushliteral(state, "!");
  test->deny = 1;
  require(lua_pcall(state, 3, 2, 0) == LUA_OK);
  test->deny = 0;
  require(test->failures == 0 && lua_rawlen(state, 1) == 8194 &&
          lua_tointeger(state, 2) == 0);
  lua_settop(state, 0);
  execute(state, "replacement=text..'%1'");
  for (kind = 0; kind < 2; ++kind) {
    int complete = 0;
    for (index = 0; index < 32 && !complete; ++index) {
      int status;
      function(state, "gsub");
      lua_getglobal(state, "text");
      lua_pushstring(state, kind ? "()a+" : "(a+)");
      if (kind)
        lua_getglobal(state, "replacement");
      else
        lua_pushliteral(state, "%1%1");
      test->remaining = index;
      test->failures = 0;
      test->deny = 1;
      status = lua_pcall(state, 3, 2, 0);
      test->deny = 0;
      require(state->native_roots == NULL && state->object_scopes == NULL);
      if (status == LUA_OK) {
        size_t len;
        const char *result = lua_tolstring(state, 1, &len);
        require(result != NULL && len == (kind ? 8195u : 16388u));
        require(result[0] == 'a' && result[len - 1] == (kind ? '1' : 'a'));
        require(lua_tointeger(state, 2) == 1 && index != 0);
        complete = 1;
      } else
        require(status == LUA_ERRMEM && test->failures != 0);
      lua_settop(state, 0);
      (void)lua_gc(state, LUA_GCCOLLECT);
    }
    require(complete);
  }
  execute(state,
          "local f=string.gmatch('a','(.)'); assert(f()=='a' and f()==nil)");
}
static void *run(void *context) {
  struct fixture *test = context;
  int facade = strstr(test->mode, "facade") != NULL;
  lua_State *state;
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
                                         sizeof(source) - 1, "pattern-library",
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
  lua_pushcfunction(state, bad_userdata);
  lua_setglobal(state, "bad_userdata");
  lua_pushcfunction(state, strip);
  lua_setglobal(state, "strip");
  if (strstr(test->mode, "semantics") != NULL) {
    execute(
        state,
        "local s=string; local z=s.char(0); local a,b=s.find('abcabc','bc'); "
        "assert(a==2 and b==3); "
        "a,b=s.find('a'..z..'b'..z..'c',z..'b'..z,1,true); "
        "assert(a==2 and b==4); assert(s.find('abc','a',4)==nil); "
        "assert(s.find('abc','a',-100)==1 and s.find('abc','a',0)==1); "
        "local a,b=s.match('alpha=12; beta=345','(%a+)=(%d+)'); "
        "assert(a=='alpha' and b=='12'); "
        "assert(s.match('axbyb','a(.-)b')=='x'); "
        "assert(s.match('axbyb','a(.*)b')=='xby'); "
        "assert(s.match(' abc ','%f[%w]%w+%f[%W]')=='abc'); "
        "assert(s.match('ab ab','(%a+)%s+%1')=='ab'); "
        "assert(s.match('(a(b)c)','%b()')=='(a(b)c)'); "
        "assert(s.match('a-b9','[%a%d%-]+')=='a-b9'); "
        "assert(s.match(']]','[%]]+')==']]'); "
        "assert(s.match('ab'..z..'c','^[^c]+')=='ab'..z); "
        "assert(s.match('ac'..z..'b','^[^'..z..']+')=='ac'); "
        "assert(s.match('azzz]','^([^]]+)]')=='azzz'); "
        "assert(s.match('baac','^([^c]+)a')=='ba'); "
        "assert(s.match('baa','^([^b]*)b')==''); "
        "local run=s.rep('a'..z,2049); "
        "assert(s.match(run..'c','^[^c]+')==run); "
        "assert(s.match('ABc','%u+%l?')=='ABc' and s.match('a','[^x]')=='a'); "
        "assert(s.match(z,'%z')==z and s.match('q','%q')=='q'); "
        "local out={}; for n in s.gmatch('ab','()') do out[#out+1]=n end; "
        "assert(#out==3 and out[1]==1 and out[2]==2 and out[3]==3); "
        "out={}; for v in s.gmatch('ba','a*') do out[#out+1]=v end; "
        "assert(#out==2 and out[1]=='' and out[2]=='a'); "
        "out={}; for v in s.gmatch('^a^a','^a') do out[#out+1]=v end; "
        "assert(#out==2 and out[1]=='^a' and out[2]=='^a'); "
        "assert(s.gmatch('abc','.',99)()==nil); "
        "local captures=table.pack(s.gmatch('a',s.rep('()',24)..'a')()); "
        "assert(captures.n==24 and captures[1]==1 and captures[24]==1); "
        "local v,n=s.gsub('abc','(.)','%1%1'); assert(v=='aabbcc' and n==3); "
        "v,n=s.gsub('a12 b34','(%d+)',function(x) return tonumber(x)*2 end); "
        "assert(v=='a24 b68' and n==2); "
        "v,n=s.gsub('abcd','%a',{a=false,c=7,d='D'}); assert(v=='ab7D' and "
        "n==4); "
        "v,n=s.gsub('ab','()','[%1]'); assert(v=='[1]a[2]b[3]' and n==3); "
        "v,n=s.gsub('ab','.',function() return false end); assert(v=='ab' and "
        "n==2); "
        "v,n=s.gsub('a','.',12); assert(v=='12' and n==1); "
        "v,n=s.gsub('abc','.','x',0); assert(v=='abc' and n==0); "
        "v,n=s.gsub('abc','z','%'); assert(v=='abc' and n==0); "
        "v,n=s.gsub('a1 b2','(%a)(%d)',{a='X',b='Y'}); assert(v=='X Y' and "
        "n==2)");
  } else if (strstr(test->mode, "errors") != NULL) {
    execute(state,
            "for _,p in ipairs({'%','[','[a','(','a)','%1','%b','%f.'}) do "
            "assert(not pcall(string.match,'a',p)); "
            "local f=string.gmatch('a',p); assert(not pcall(f)) end; "
            "assert(not pcall(string.match,'a',string.rep('()',33))); "
            "assert(not pcall(string.gsub,'a','.','%')); "
            "assert(not pcall(string.gsub,'a','.','%q')); "
            "assert(not pcall(string.gsub,'a','.',function() return {} end)); "
            "assert(not pcall(string.gsub,'a','.',nil)); "
            "assert(not pcall(string.find,{},'a')); "
            "assert(not pcall(string.gmatch,'a','a','bad')); "
            "assert(string.match('abc','%a+')=='abc')");
  } else if (strstr(test->mode, "bindings") != NULL) {
    execute(
        state,
        "local other=reopen(); for _,k in "
        "ipairs({'find','match','gmatch','gsub'}) do "
        "assert(other[k]==string[k]) end; local f=string.gmatch('ab','(.)'); "
        "local _,source=debug.getupvalue(f,1); local "
        "_,p=debug.getupvalue(f,2); "
        "local _,ud=debug.getupvalue(f,3); assert(source=='ab' and p=='(.)'); "
        "assert(debug.getuservalue(ud,1)==source and "
        "debug.getuservalue(ud,2)==p); "
        "debug.setupvalue(f,1,nil); debug.setupvalue(f,2,nil); "
        "assert(f()=='a' and f()=='b' and f()==nil); "
        "assert(not pcall(strip(f))); "
        "for _,v in ipairs({false,17,'bad',{},bad_userdata(ud)}) do "
        "debug.setupvalue(f,3,v); assert(not pcall(f)) end; "
        "debug.setupvalue(f,3,ud); debug.setuservalue(ud,'different',1); "
        "assert(not pcall(f)); debug.setuservalue(ud,source,1); "
        "debug.setuservalue(ud,nil,2); assert(not pcall(f))");
  } else if (strstr(test->mode, "callback_snapshot") != NULL)
    callback_snapshot(test);
  else if (strstr(test->mode, "snapshot") != NULL)
    snapshot(test);
  else if (strstr(test->mode, "parallel") != NULL ||
           strstr(test->mode, "stress") != NULL)
    parallel(test, strstr(test->mode, "stress") != NULL);
  else if (strstr(test->mode, "stack") != NULL)
    execute(state,
            "local ok,e=pcall(string.match,string.rep('a',240),"
            "string.rep('a?',240)); assert(not ok and type(e)=='string')");
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
