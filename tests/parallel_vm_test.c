#define _GNU_SOURCE
#include <laccess.h>
#include <lauxlib.h>
#include <lstate.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture;

struct actor {
  struct fixture *fixture;
  lua_State *state;
  int cpu;
  int affinity_error;
  int polled;
  int nargs;
  int status;
  int results;
  int done;
  unsigned yields;
};

struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_barrier_t managed;
  int ready;
  int start;
  int abort_start;
  int forbid_allocations;
  unsigned allocations;
  pthread_t failure_carrier;
  void *failure_owner;
  unsigned failures_left;
  size_t failure_minimum;
  unsigned allocation_failures;
  unsigned emergency_frees;
  global_State *global;
  int inject_failure;
  int allocator_blocked;
  int release_allocator;
  int waiter_observed;
  unsigned resources_created;
  unsigned resources_finalized;
  int native_entered;
  int release_native;
  int resume_yields;
  struct actor actors[2];
};

struct resource {
  struct fixture *fixture;
  unsigned magic;
};

static const char closed_source[] =
    "local x=7; local object={n=7}; local function answer() return 13 end; "
    "return "
    "function() local t={0}; coroutine.yield(); "
    "for i=1,5000 do local k=i%7; "
    "if k==0 then x=7 elseif k==1 then x='word' elseif k==2 then x=false "
    "elseif k==3 then x=nil elseif k==4 then x=1.25 "
    "elseif k==5 then x=object else x=answer end; t[1]=t[1]+1 end; "
    "return t[1] end, "
    "function() local t={0}; coroutine.yield(); local ok=true; "
    "for i=1,5000 do local v=x; if v~=7 and v~='word' and v~=false and "
    "v~=nil and v~=1.25 and v~=object and v~=answer then ok=false end; "
    "t[1]=t[1]+1 end; return ok,t[1] end";

static const char open_source[] =
    "return function() local x=0; local t={0}; "
    "local function change() coroutine.yield(); x=42; return true end; "
    "coroutine.yield(change); while x~=42 do t[1]=t[1]+1 end; "
    "return t[1] end";

static const char api_source[] =
    "return function() local x=0; local t={0}; "
    "local function read() return x end; coroutine.yield(read); "
    "while x~=42 do t[1]=t[1]+1 end; return t[1] end";

static const char cycle_source[] =
    "return function() local x=7; local function read() return x end; "
    "read(); local other=coroutine.yield(read); local sum=0; "
    "for i=1,5000 do sum=sum+other() end; return sum end";

static const char blocked_source[] =
    "return function() coroutine.yield(); fixture_wait(); return true end, "
    "function() local t={0}; coroutine.yield(); fixture_entered(); "
    "for i=1,5000 do t[1]=t[1]+1 end; return t[1] end";

static const char allocation_source[] =
    "local long=string.rep('long',32); local shared={0}; return "
    "function() coroutine.yield(); fixture_allocate_ready(1); "
    "if fixture_inject_failure then "
    "fixture_fail() end; "
    "local sum=0; for i=1,500 do local resource=fixture_resource(); "
    "if type(resource)~='userdata' then error('resource') end; "
    "local t={i}; local k=fixture_intern('key'..i); t[k]=i; "
    "local lk=fixture_intern(long..i); t[long]=i; t[lk]=i; "
    "local f=function() return t[1]+t[k]+t[long]+t[lk] end; sum=sum+f(); "
    "shared=t "
    "end; return sum end, "
    "function() coroutine.yield(); fixture_allocate_ready(2); "
    "if fixture_require_native then fixture_entered() end; "
    "local ok=true; local sum=0; "
    "for i=1,500 do local resource=fixture_resource(); "
    "if type(resource)~='userdata' then error('resource') end; "
    "local v=shared; if type(v)~='table' or "
    "type(v[1])~='number' then ok=false end; local t={i}; "
    "local k=fixture_intern('key'..i); t[k]=i; "
    "local lk=fixture_intern(long..i); t[long]=i; t[lk]=i; "
    "local f=function() return t[1]+t[k]+t[long]+t[lk] end; sum=sum+f() "
    "end; return ok,sum end";

static const char allocation_open_source[] =
    "return function() local long=string.rep('long',32); local shared={0}; "
    "local function reader() coroutine.yield(); local ok=true; local sum=0; "
    "for i=1,500 do local resource=fixture_resource(); "
    "if type(resource)~='userdata' then error('resource') end; "
    "local v=shared; if type(v)~='table' or "
    "type(v[1])~='number' then ok=false end; local t={i}; "
    "local k=fixture_intern('key'..i); t[k]=i; "
    "local lk=fixture_intern(long..i); t[long]=i; t[lk]=i; "
    "local f=function() return t[1]+t[k]+t[long]+t[lk] end; sum=sum+f() "
    "end; return ok,sum end; coroutine.yield(reader); "
    "local sum=0; for i=1,500 do local resource=fixture_resource(); "
    "if type(resource)~='userdata' then error('resource') end; local t={i}; "
    "local k=fixture_intern('key'..i); t[k]=i; "
    "local lk=fixture_intern(long..i); t[long]=i; t[lk]=i; "
    "local f=function() return t[1]+t[k]+t[long]+t[lk] end; "
    "sum=sum+f(); shared=t end; return sum end";

static const char table_source[] =
    "local t={hot=7}; fixture_table=t; local long=string.rep('long',32); "
    "local function actor(id) return function() coroutine.yield(); "
    "local sum=0; for i=1,300 do local k='key'..id..':'..i; "
    "local lk=long..id..':'..i; t[(id-1)*300+i]=i; t[k]={i}; t[lk]=i; "
    "t.hot=(i%3==0) and {7} or ((i%2==0) and 7 or 'word'); "
    "local v=t.hot; assert(v==7 or v=='word' or "
    "(type(v)=='table' and v[1]==7)); "
    "t['gone'..id]=i; t['gone'..id]=nil; "
    "rawset(t,'raw'..id,i); assert(rawget(t,'raw'..id)==i); "
    "assert(t[k][1]==i and t[lk]==i); "
    "assert(type(#t)=='number' and next(t)~=nil); "
    "local bulk={1,2,3,(function() return 4,5,6 end)()}; assert(#bulk==6); "
    "fixture_table_api(t,id,i); sum=sum+i end; "
    "for i=1,300 do assert(t[(id-1)*300+i]==i and "
    "t['key'..id..':'..i][1]==i and t[long..id..':'..i]==i) end; "
    "return true,sum end end; "
    "return actor(1),actor(2)";

static const char table_meta_source[] =
    "local a,b={},{}; setmetatable(a,b); setmetatable(b,a); "
    "local u=fixture_userdata; local function index(t,k) "
    "assert(rawget(t,'self')==t); rawset(t,'reentered',true); "
    "if k=='missing' then return 17 end; return nil end; "
    "local function newindex(t,k,v) rawset(t,k,v) end; "
    "rawset(a,'self',a); rawset(b,'self',b); "
    "local function actor(id,t,mt) return function() coroutine.yield(); "
    "for i=1,300 do rawset(t,'__index',index); "
    "rawset(t,'__newindex',newindex); rawset(t,'__len',function() return 29 "
    "end); "
    "rawset(t,'__eq',function() return true end); "
    "setmetatable(t,(i%2==0) and mt or t); "
    "t.x=nil; t.x=i; assert(t.x==i); local v=t.missing; "
    "assert(v==nil or v==17); local len=#t; assert(len==0 or len==29); "
    "local equal=(t==mt); assert(type(equal)=='boolean'); "
    "fixture_userdata_api(u,id,i,t); "
    "end; return true,45150 end end; return actor(1,a,b),actor(2,b,a)";

static const char table_blocked_source[] =
    "local t={}; local mt={__index=function() fixture_wait(); return 17 end}; "
    "setmetatable(t,mt); return function() coroutine.yield(); "
    "assert(t.missing==17); return true end, "
    "function() coroutine.yield(); fixture_entered(); "
    "mt.__index=nil; setmetatable(t,nil); "
    "for i=1,5000 do rawset(t,i,i); assert(t[i]==i); "
    "mt['key'..i]=i end; mt=nil; collectgarbage('collect'); return 5000 end";

static const char table_error_source[] =
    "local t={}; local mt={__index=function(self,k) "
    "if k=='wait' then fixture_wait(); return 17 end; error('meta failure') "
    "end}; "
    "setmetatable(t,mt); return function() coroutine.yield(); "
    "for i=1,300 do local ok,msg=xpcall(function() return t.missing end, "
    "function(e) rawset(t,'handled',true); return e end); "
    "assert(not ok and msg:match('meta failure')); "
    "local ok=pcall(function() t[nil]=1 end); assert(not ok); "
    "rawset(t,i,i) end; return true,45150 end, "
    "function() coroutine.yield(); local sum=0; for i=1,300 do "
    "t['key'..i]={i}; assert(t['key'..i][1]==i); sum=sum+i "
    "end; return true,sum end";

static const char table_failure_source[] =
    "local t={hot=7}; for i=1,4096 do t[i]=0 end; "
    "return function() coroutine.yield(); "
    "fixture_allocate_ready(1); fixture_fail(t); "
    "assert(rawget(t,4097)==nil); t.recovered=true; return true,45150 end, "
    "function() coroutine.yield(); fixture_allocate_ready(2); "
    "assert(t.hot==7); for i=1,300 do t[i]={i}; assert(t[i][1]==i) end; "
    "return true,45150 end";

static const char table_handler_source[] =
    "local t={}; return function() coroutine.yield(); "
    "local ok,msg=xpcall(function() t[nil]=1 end,function(e) "
    "fixture_wait(); rawset(t,'handled',true); return e end); "
    "assert(not ok and msg:match('table index is nil') and t.handled); "
    "return true end, function() coroutine.yield(); fixture_entered(); "
    "for i=1,5000 do t[i]=i; assert(t[i]==i) end; return 5000 end";

static const char table_yield_source[] =
    "local t,other={},{}; local mt={}; setmetatable(t,mt); "
    "setmetatable(other,mt); local function method(v) return function() "
    "coroutine.yield('metamethod'); return v end end; "
    "mt.__index=method(17); mt.__newindex=function(self,k,v) "
    "coroutine.yield('newindex'); rawset(self,k,v) end; "
    "mt.__len=method(29); mt.__eq=method(true); mt.__call=method(31); "
    "mt.__add=method(37); return function() coroutine.yield(); "
    "assert(t.missing==17); t.x=23; assert(t.x==23); assert(#t==29); "
    "assert(t==other and t()==31 and t+t==37); return true,45150 end, "
    "function() coroutine.yield(); for i=1,300 do rawset(t,i,i); "
    "mt['key'..i]={i}; assert(t[i]==i) end; return true,45150 end";

static const char join_source[] =
    "local left,right=7,'word'; local function self() return left,right end; "
    "debug.upvaluejoin(self,1,self,2); local v,w=self(); "
    "assert(v=='word' and w=='word'); "
    "local function cell(x) return function(v) if v~=nil then x=v end; "
    "return x end,function() return function() return x end end, "
    "function() return x+1 end end; "
    "local a,makea,bad=cell(7); local b,makeb=cell('word'); "
    "local function valid(v) return v==7 or v=='word' or "
    "(type(v)=='table' and v.n==7) end; "
    "local function actor(to,other,factory,otherfactory) return function() "
    "coroutine.yield(); local sum=0; for i=1,300 do "
    "local v=(i%3==0) and {n=7} or ((i%2==0) and 7 or 'word'); "
    "local donor=cell(v); debug.upvaluejoin(to,1,donor,1); "
    "assert(valid(to(v))); debug.upvaluejoin(factory,1,to,1); "
    "assert(valid(factory()()) and valid(otherfactory()())); "
    "debug.upvaluejoin(to,1,other,1); fixture_join_api(to,other,v); "
    "debug.upvaluejoin(bad,1,to,1); local ok,result=pcall(bad); "
    "assert((ok and result==8) or (not ok and type(result)=='string' "
    "and (result:match('arithmetic') or result:match('attempt to "
    "add'))),result); "
    "assert(debug.upvalueid(to,1)~=nil); sum=sum+i; "
    "if i%50==0 then collectgarbage('collect') end "
    "end; return true,sum end end; "
    "return actor(a,b,makea,makeb),actor(b,a,makeb,makea)";

static const char join_open_source[] =
    "return function() local x=7; local function anchor(v) "
    "if v~=nil then x=v end; return x end; "
    "local function active(v) if v~=nil then x=v end; return x end; "
    "local function make() return function() return x end end; "
    "coroutine.yield(anchor,active,make); local sum=0; "
    "for i=1,300 do local v=x; assert(v==7 or v=='word' or "
    "(type(v)=='table' and v.n==7)); local f=make(); v=f(); "
    "assert(v==7 or v=='word' or (type(v)=='table' and v.n==7)); "
    "sum=sum+i end; fixture_wait(); return true,sum end";

static const char join_reader_source[] =
    "local anchor,active,make=...; local function cell(x) "
    "return function() return x end end; "
    "return function() coroutine.yield(); local sum=0; for i=1,300 do "
    "if i==150 then fixture_entered() end; "
    "local v=(i%3==0) and {n=7} or ((i%2==0) and 7 or 'word'); "
    "local donor=cell(v); debug.upvaluejoin(active,1,donor,1); "
    "debug.upvaluejoin(make,1,active,1); local f=make(); local value=f(); "
    "assert(value==7 or value=='word' or "
    "(type(value)=='table' and value.n==7)); "
    "fixture_join_api(active,anchor,v); sum=sum+i; "
    "if i%50==0 then collectgarbage('collect') end "
    "end; return true,sum end";

static const char capi_source[] =
    "local function cell(x) return function() return x end end; "
    "local t1,t2={7,token=17},{7,token=17}; "
    "local u1,u2=fixture_udnew(),fixture_udnew(); local f1,f2=cell(7),cell(7); "
    "local values={7,'word','7',true,false,t1,u1,f1,print,fixture_lighttoken, "
    "coroutine.create(function() end)}; "
    "local shared=fixture_cclosure(7,t1,u1,f1); return "
    "function() coroutine.yield(); local sum=0; for i=1,300 do "
    "local v=values[(i%#values)+1]; if i%13==0 then v=nil end; "
    "shared(1,v,(i%2==0) and t1 or t2,(i%2==0) and u1 or u2, "
    "(i%2==0) and f1 or f2); debug.setupvalue(shared,1,v); shared(0); "
    "sum=sum+i end; return true,sum end, "
    "function() coroutine.yield(); local sum=0; for i=1,300 do "
    "shared(0); shared(2); shared(3); shared(4,f2); sum=sum+i; "
    "if i%50==0 then collectgarbage('collect') end "
    "end; return true,sum end";

static const char capi_failure_source[] =
    "local shared=fixture_cclosure(867214391); return "
    "function() coroutine.yield(); fixture_allocate_ready(1); "
    "local ok,msg=pcall(shared,5); assert(not ok and msg:match('memory')); "
    "shared(1,7); collectgarbage('restart'); return true,45150 end, "
    "function() coroutine.yield(); fixture_allocate_ready(2); "
    "local v=tonumber(shared(8)); assert(v and (v==7 or "
    "(v>=867214391 and v<=867218487))); local sum=0; "
    "for i=1,300 do local t={i}; sum=sum+t[1] end; return true,sum end";

static const char capi_blocked_source[] =
    "local shared=fixture_cclosure(setmetatable({token=17, "
    "resource=fixture_resource()}, {__index=function(t) "
    "fixture_wait(); return t.token end})); return "
    "function() coroutine.yield(); assert(shared(6)==17); "
    "return true,45150 end, function() coroutine.yield(); fixture_entered(); "
    "shared(1,{token=29}); for i=1,300 do "
    "local value={i}; debug.setupvalue(shared,1,value) end; "
    "collectgarbage('collect'); return true,45150 end";

static const char registry_source[] =
    "local r=debug.getregistry(); local g=fixture_registry_globals; "
    "r[g].registry_token=7; local a,b={},{}; "
    "for k,v in pairs(r) do a[k]=v; b[k]=v end; "
    "a[g]={registry_token=7}; b[g]={registry_token='word'}; "
    "local shared=fixture_cclosure(7); return "
    "function() coroutine.yield(); local sum=0; for i=1,300 do "
    "shared(9,(i%2==0) and a or b); sum=sum+i; "
    "if i%30==0 then collectgarbage('collect') end "
    "end; return true,sum end, function() coroutine.yield(); "
    "local sum=0; for i=1,300 do shared(10); sum=sum+i end; "
    "return true,sum end";

static const char capi_conversion_source[] =
    "local function actor(id) return function() "
    "local convert=fixture_cclosure(0); coroutine.yield(); local sum=0; "
    "for i=1,300 do local value=id*1000000+i; convert(1,value); "
    "convert(11,value); for j=1,8 do collectgarbage('step') end; "
    "assert(convert(8)==tostring(value)); sum=sum+i "
    "end; return true,sum end end; return actor(1),actor(2)";

static const char capi_borrowed_source[] =
    "local shared=fixture_cclosure(string.rep('borrowed',32)); return "
    "function() coroutine.yield(); shared(12); return true,45150 end, "
    "function() coroutine.yield(); fixture_entered(); "
    "debug.setupvalue(shared,1,'replacement'); collectgarbage('collect'); "
    "for i=1,300 do local t={string.rep('other',32)..i} end; "
    "collectgarbage('collect'); return true,45150 end";

static void check(int status) {
  if (status != 0) {
    fprintf(stderr, "parallel fixture synchronization failed: %d\n", status);
    abort();
  }
}

static void *allocate(void *context, void *pointer, size_t old_size,
                      size_t size) {
  struct fixture *fixture = context;
  int reject = 0;
  if (fixture->forbid_allocations) {
    fputs(
        "parallel prototype attempted allocation/free before GC coordination\n",
        stderr);
    abort();
  }
  fixture->allocations++; /* a legacy allocator receives serialized callbacks */
  check(pthread_mutex_lock(&fixture->mutex));
  if (size == 0 && fixture->failures_left != 0 && fixture->global != NULL &&
      fixture->global->gcemergency)
    fixture->emergency_frees++;
  if (fixture->failures_left != 0 &&
      pthread_equal(fixture->failure_carrier, pthread_self()) && size != 0 &&
      size >= fixture->failure_minimum &&
      (pointer == NULL || size > old_size)) {
    if (!fixture->allocator_blocked) {
      fixture->allocator_blocked = 1;
      check(pthread_cond_broadcast(&fixture->changed));
      while (!fixture->release_allocator)
        check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
    }
    fixture->failures_left--;
    fixture->allocation_failures++;
    reject = 1;
  }
  check(pthread_mutex_unlock(&fixture->mutex));
  if (reject)
    return NULL;
  if (size == 0) {
    free(pointer);
    return NULL;
  }
  return realloc(pointer, size);
}

static void managed_poll(lua_State *state, void *context) {
  struct actor *actor = context;
  (void)state;
  if (!actor->polled) {
    int status;
    actor->polled = 1;
    /* This runs inside VM dispatch with stack ownership retained. It is not a
     * Lua/C callback, which would release managed ownership before entry. */
    status = pthread_barrier_wait(&actor->fixture->managed);
    if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD)
      check(status);
  }
}

static struct fixture *native_context(lua_State *state) {
  return lua_touserdata(state, lua_upvalueindex(1));
}

static void wait_fixture(lua_State *state, struct fixture *fixture) {
  LuaObjectScope *scope;
  for (scope = state->object_scopes; scope != NULL; scope = scope->previous)
    if (scope->held)
      abort();
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->native_entered = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  while (!fixture->release_native)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
}

static int native_wait(lua_State *state) {
  wait_fixture(state, native_context(state));
  return 0;
}

static int native_entered(lua_State *state) {
  struct fixture *fixture = native_context(state);
  check(pthread_mutex_lock(&fixture->mutex));
  while (!fixture->native_entered)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int native_setcapture(lua_State *state) {
  if (lua_getupvalue(state, 1, 1) == NULL || lua_tointeger(state, -1) != 0)
    abort();
  lua_pop(state, 1);
  lua_pushinteger(state, 42);
  if (lua_setupvalue(state, 1, 1) == NULL ||
      lua_getupvalue(state, 1, 1) == NULL || lua_tointeger(state, -1) != 42)
    abort();
  return 0;
}

static int native_intern(lua_State *state) {
  const char *text = lua_tostring(state, 1);
  if (text == NULL)
    abort();
  lua_pushstring(state, text);
  if (!lua_rawequal(state, 1, 2))
    abort();
  return 1;
}

static int native_table_api(lua_State *state) {
  lua_Integer id = lua_tointeger(state, 2);
  lua_Integer value = lua_tointeger(state, 3);
  lua_Integer key = 10000 + id;
  lua_pushinteger(state, value);
  lua_rawseti(state, 1, key);
  lua_rawgeti(state, 1, key);
  if (lua_tointeger(state, -1) != value || lua_rawlen(state, 1) > 10002)
    abort();
  lua_pop(state, 1);
  lua_pushinteger(state, value);
  lua_setfield(state, 1, id == 1 ? "api1" : "api2");
  lua_getfield(state, 1, id == 1 ? "api1" : "api2");
  if (lua_tointeger(state, -1) != value)
    abort();
  return 0;
}

static int native_userdata_api(lua_State *state) {
  int id = (int)lua_tointeger(state, 2);
  lua_Integer value = lua_tointeger(state, 3);
  lua_pushinteger(state, value);
  if (!lua_setiuservalue(state, 1, id) ||
      lua_getiuservalue(state, 1, id) != LUA_TNUMBER ||
      lua_tointeger(state, -1) != value)
    abort();
  lua_pop(state, 1);
  lua_pushvalue(state, 4);
  lua_setmetatable(state, 1);
  if (!lua_getmetatable(state, 1) || !lua_istable(state, -1))
    abort();
  return 0;
}

static int native_join_api(lua_State *state) {
  int top;
  lua_upvaluejoin(state, 1, 1, 1, 1);
  lua_upvaluejoin(state, 1, 1, 2, 1);
  lua_pushvalue(state, 3);
  if (lua_setupvalue(state, 1, 1) == NULL ||
      lua_getupvalue(state, 1, 1) == NULL || lua_upvalueid(state, 1, 1) == NULL)
    abort();
  if (lua_istable(state, -1)) {
    lua_getfield(state, -1, "n");
    if (lua_tointeger(state, -1) != 7)
      abort();
  } else if (lua_isinteger(state, -1)) {
    if (lua_tointeger(state, -1) != 7)
      abort();
  } else if (!lua_isstring(state, -1) ||
             strcmp(lua_tostring(state, -1), "word") != 0)
    abort();
  top = lua_gettop(state);
  if (lua_getupvalue(state, 1, 0) != NULL ||
      lua_upvalueid(state, 1, 0) != NULL ||
      lua_upvalueid(state, 1, 256) != NULL || lua_gettop(state) != top)
    abort();
  lua_pushvalue(state, 3);
  if (lua_setupvalue(state, 1, 0) != NULL || lua_gettop(state) != top + 1)
    abort();
  lua_pop(state, 1);
  return 0;
}

static char pointer_key;

static int native_shared(lua_State *state) {
  int mode = (int)lua_tointeger(state, 1);
  int slot = lua_upvalueindex(1);
  if (mode == 1) {
    int top = lua_gettop(state);
    int i;
    for (i = 2; i <= top; ++i) {
      if (i == 3) {
        lua_pushinteger(state, 17);
        lua_rawsetp(state, i, &pointer_key);
      }
      lua_copy(state, i, lua_upvalueindex(i - 1));
    }
  } else if (mode == 0) {
    char buffer[64];
    size_t length;
    unsigned digits;
    int numeric;
    lua_Number number;
    lua_Integer integer;
    const char *text;
    int type = lua_type(state, slot);
    if (type < LUA_TNIL || type > LUA_TTHREAD ||
        lua_type(state, lua_upvalueindex(256)) != LUA_TNONE)
      abort();
    (void)lua_iscfunction(state, slot);
    (void)lua_isinteger(state, slot);
    (void)lua_isnumber(state, slot);
    (void)lua_isstring(state, slot);
    (void)lua_isuserdata(state, slot);
    (void)lua_toboolean(state, slot);
    (void)lua_tocfunction(state, slot);
    (void)lua_touserdata(state, slot);
    (void)lua_tothread(state, slot);
    (void)lua_topointer(state, slot);
    (void)lua_rawlen(state, slot);
    number = lua_tonumberx(state, slot, &numeric);
    if ((numeric && number != 7) || (!numeric && number != 0))
      abort();
    integer = lua_tointegerx(state, slot, &numeric);
    if ((numeric && integer != 7) || (!numeric && integer != 0))
      abort();
    digits = lua_numbertocstring(state, slot, buffer);
    if (digits != 0 && (digits != 2 || strcmp(buffer, "7") != 0))
      abort();
    /* These two strings also have independent roots in the Lua fixture. */
    text = lua_tolstring(state, slot, &length);
    if (text != NULL && (length != strlen(text) ||
                         (strcmp(text, "7") != 0 && strcmp(text, "word") != 0)))
      abort();
    if (!lua_rawequal(state, slot, slot) ||
        !lua_compare(state, slot, slot, LUA_OPEQ))
      abort();
    lua_pushvalue(state, slot);
    if (lua_type(state, -1) == LUA_TNONE)
      abort();
    lua_pop(state, 1);
  } else if (mode == 2) {
    int table = lua_upvalueindex(2);
    lua_pushliteral(state, "token");
    lua_gettable(state, table);
    lua_getfield(state, table, "token");
    lua_geti(state, table, 1);
    lua_pushliteral(state, "token");
    lua_rawget(state, table);
    lua_rawgeti(state, table, 1);
    lua_rawgetp(state, table, &pointer_key);
    if (lua_tointeger(state, -6) != 17 || lua_tointeger(state, -5) != 17 ||
        lua_tointeger(state, -4) != 7 || lua_tointeger(state, -3) != 17 ||
        lua_tointeger(state, -2) != 7 || lua_tointeger(state, -1) != 17)
      abort();
    lua_pop(state, 6);
    lua_pushliteral(state, "hot");
    lua_pushinteger(state, 17);
    lua_settable(state, table);
    lua_pushinteger(state, 17);
    lua_setfield(state, table, "hot");
    lua_pushinteger(state, 7);
    lua_seti(state, table, 1);
    lua_pushliteral(state, "hot");
    lua_pushinteger(state, 17);
    lua_rawset(state, table);
    lua_pushinteger(state, 7);
    lua_rawseti(state, table, 1);
    lua_pushinteger(state, 17);
    lua_rawsetp(state, table, &pointer_key);
    lua_pushnil(state);
    if (!lua_next(state, table))
      abort();
    lua_pop(state, 2);
    if (lua_getmetatable(state, table))
      lua_pop(state, 1);
    lua_pushnil(state);
    lua_setmetatable(state, table);
    lua_len(state, table);
    if (lua_tointeger(state, -1) != 1)
      abort();
    lua_pop(state, 1);
  } else if (mode == 3) {
    int userdata = lua_upvalueindex(3);
    lua_pushinteger(state, 7);
    if (!lua_setiuservalue(state, userdata, 1) ||
        lua_getiuservalue(state, userdata, 1) != LUA_TNUMBER ||
        lua_tointeger(state, -1) != 7 || lua_rawlen(state, userdata) != 1 ||
        *(unsigned char *)lua_touserdata(state, userdata) != 42)
      abort();
    lua_pop(state, 1);
    lua_pushnil(state);
    lua_setmetatable(state, userdata);
  } else if (mode == 4) {
    int function = lua_upvalueindex(4);
    lua_upvaluejoin(state, function, 1, 2, 1);
    lua_pushinteger(state, 7);
    if (lua_setupvalue(state, function, 1) == NULL ||
        lua_getupvalue(state, function, 1) == NULL ||
        lua_tointeger(state, -1) != 7 ||
        lua_upvalueid(state, function, 1) == NULL)
      abort();
    lua_pop(state, 1);
  } else if (mode == 5) {
    struct fixture *fixture;
    void *user;
    (void)lua_getallocf(state, &user);
    fixture = user;
    if (!lua_checkstack(state, 4100))
      return luaL_error(state, "cannot prepare conversion failure test");
    check(pthread_mutex_lock(&fixture->mutex));
    fixture->failure_carrier = pthread_self();
    fixture->failure_owner = luaQ_ownertoken();
    fixture->failures_left = ~0U; /* refuse refills until conversion fails */
    check(pthread_mutex_unlock(&fixture->mutex));
    /* Retain distinct successful conversions until the string pages need
     * backing storage. The failed conversion must leave the shared value
     * intact while the peer waits for its claim. */
    for (int i = 1; i <= 4096; ++i) {
      lua_pushinteger(state, 867214391 + i);
      lua_replace(state, slot);
      (void)lua_tolstring(state, slot, NULL);
      lua_pushvalue(state, slot);
    }
    abort();
  } else if (mode == 6) {
    lua_getfield(state, slot, "missing");
    return 1;
  } else if (mode == 8) {
    lua_pushvalue(state, slot);
    return 1;
  } else if (mode == 9) {
    lua_copy(state, 2, LUA_REGISTRYINDEX);
  } else if (mode == 10) {
    lua_getglobal(state, "registry_token");
    if (lua_isinteger(state, -1)) {
      if (lua_tointeger(state, -1) != 7)
        abort();
    } else if (strcmp(lua_tostring(state, -1), "word") != 0)
      abort();
    lua_pop(state, 1);
    lua_pushinteger(state, 7);
    lua_setglobal(state, "registry_aux");
    lua_getglobal(state, "registry_aux");
    if (!lua_isnil(state, -1) && lua_tointeger(state, -1) != 7)
      abort();
    lua_pop(state, 1);
  } else if (mode == 11) {
    char expected[64];
    const char *text;
    size_t length;
    lua_Integer number = lua_tointeger(state, 2);
    int count = snprintf(expected, sizeof(expected), "%lld", (long long)number);
    if (count < 0 || (size_t)count >= sizeof(expected))
      abort();
    text = lua_tolstring(state, slot, &length);
    if (text == NULL || length != (size_t)count || strcmp(text, expected) != 0)
      abort();
  } else if (mode == 12) {
    const char *text;
    size_t length, i;
    void *user;
    lua_pushvalue(state, slot); /* ordinary API root, independent of capture */
    text = lua_tolstring(state, -1, &length);
    if (text == NULL || length != 256)
      abort();
    (void)lua_getallocf(state, &user);
    wait_fixture(state, user);
    for (i = 0; i < length; i += 8)
      if (memcmp(text + i, "borrowed", 8) != 0)
        abort();
    lua_pop(state, 1);
  } else
    abort();
  return 0;
}

static int native_cclosure(lua_State *state) {
  int n = lua_gettop(state);
  int i;
  if (n > 1) {
    lua_pushinteger(state, 17);
    lua_rawsetp(state, 2, &pointer_key);
  }
  for (i = 1; i <= n; ++i)
    lua_pushvalue(state, i);
  lua_pushcclosure(state, native_shared, n);
  return 1;
}

static int native_udnew(lua_State *state) {
  unsigned char *payload = lua_newuserdatauv(state, 1, 2);
  *payload = 42;
  lua_pushinteger(state, 7);
  (void)lua_setiuservalue(state, -2, 1);
  return 1;
}

static int fail_allocation(lua_State *state) {
  struct fixture *fixture;
  void *user;
  int table = lua_istable(state, 1);
  (void)lua_getallocf(state, &user);
  fixture = user;
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->failure_carrier = pthread_self();
  fixture->failure_owner = luaQ_ownertoken();
  /* Refilling a small page can recover from a failed allocator call. Target
   * the actual array growth so both failures belong to that allocation. */
  fixture->failure_minimum = table ? 4096 * sizeof(Value) : 0;
  fixture->failures_left =
      2; /* original attempt, then the emergency-GC retry */
  check(pthread_mutex_unlock(&fixture->mutex));
  if (table) {
    lua_pushinteger(state, 1);
    lua_rawseti(state, 1, 4097);
  } else
    (void)lua_newuserdatauv(state, 4096, 0);
  abort();
}

static int native_fail(lua_State *state) {
  int args = lua_istable(state, 1) ? 1 : 0;
  lua_pushcfunction(state, fail_allocation);
  if (args)
    lua_pushvalue(state, 1);
  if (lua_pcall(state, args, 0, 0) != LUA_ERRMEM)
    abort();
  lua_pop(state, 1);
  (void)lua_gc(state, LUA_GCRESTART);
  return 0;
}

static int native_allocate_ready(lua_State *state) {
  struct fixture *fixture = lua_touserdata(state, lua_upvalueindex(1));
  int role = (int)lua_tointeger(state, 1);
  check(pthread_mutex_lock(&fixture->mutex));
  if (fixture->inject_failure && role == 2) {
    while (!fixture->allocator_blocked)
      check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  }
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int finalize_resource(lua_State *state) {
  struct resource *resource = lua_touserdata(state, 1);
  struct fixture *fixture = resource->fixture;
  if (resource->magic != 0x1234abcdU || luaW_scope(state).heap != 0)
    abort();
  resource->magic = 0;
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->resources_finalized++;
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int native_resource(lua_State *state) {
  struct resource *resource;
  struct fixture *fixture;
  void *user;
  (void)lua_getallocf(state, &user);
  fixture = user;
  resource = lua_newuserdatauv(state, sizeof(*resource), 0);
  resource->fixture = fixture;
  resource->magic = 0x1234abcdU;
  luaL_setmetatable(state, "parallel.resource");
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->resources_created++;
  check(pthread_mutex_unlock(&fixture->mutex));
  return 1;
}

static void register_native(lua_State *state, const char *name,
                            lua_CFunction operation, struct fixture *fixture) {
  lua_pushlightuserdata(state, fixture);
  lua_pushcclosure(state, operation, 1);
  lua_setglobal(state, name);
}

static lua_State *prepare_actor(lua_State *main, int function_index,
                                struct actor *actor, int expected_results) {
  lua_State *state = lua_newthread(main);
  int results;
  lua_pushvalue(main, function_index);
  lua_xmove(main, state, 1);
  if (!lua_checkstack(state, 128) ||
      lua_resume(state, main, 0, &results) != LUA_YIELD ||
      results != expected_results)
    return NULL;
  actor->state = state;
  return state;
}

static int prepare(lua_State *main, struct fixture *fixture, const char *name) {
  const char *source;
  int results;
  if (strcmp(name, "closed") == 0)
    source = closed_source;
  else if (strcmp(name, "open") == 0)
    source = open_source;
  else if (strcmp(name, "api") == 0)
    source = api_source;
  else if (strcmp(name, "cycle") == 0)
    source = cycle_source;
  else if (strcmp(name, "blocked") == 0)
    source = blocked_source;
  else if (strncmp(name, "allocate_open", 13) == 0)
    source = allocation_open_source;
  else if (strncmp(name, "allocate", 8) == 0)
    source = allocation_source;
  else if (strncmp(name, "table_meta", 10) == 0)
    source = table_meta_source;
  else if (strcmp(name, "table_blocked") == 0)
    source = table_blocked_source;
  else if (strcmp(name, "table_error") == 0)
    source = table_error_source;
  else if (strcmp(name, "table_failure") == 0)
    source = table_failure_source;
  else if (strcmp(name, "table_handler_blocked") == 0)
    source = table_handler_source;
  else if (strcmp(name, "table_yield") == 0)
    source = table_yield_source;
  else if (strncmp(name, "join_open", 9) == 0)
    source = join_open_source;
  else if (strncmp(name, "join_", 5) == 0)
    source = join_source;
  else if (strcmp(name, "capi_failure") == 0)
    source = capi_failure_source;
  else if (strcmp(name, "capi_blocked") == 0)
    source = capi_blocked_source;
  else if (strcmp(name, "capi_borrowed") == 0)
    source = capi_borrowed_source;
  else if (strncmp(name, "capi_conversion", 15) == 0)
    source = capi_conversion_source;
  else if (strncmp(name, "capi_registry", 13) == 0)
    source = registry_source;
  else if (strncmp(name, "capi_", 5) == 0)
    source = capi_source;
  else if (strncmp(name, "table_", 6) == 0)
    source = table_source;
  else
    return 1;
  register_native(main, "fixture_wait", native_wait, fixture);
  register_native(main, "fixture_entered", native_entered, fixture);
  lua_pushcfunction(main, native_table_api);
  lua_setglobal(main, "fixture_table_api");
  lua_pushcfunction(main, native_userdata_api);
  lua_setglobal(main, "fixture_userdata_api");
  lua_pushcfunction(main, native_join_api);
  lua_setglobal(main, "fixture_join_api");
  lua_pushcfunction(main, native_cclosure);
  lua_setglobal(main, "fixture_cclosure");
  lua_pushcfunction(main, native_udnew);
  lua_setglobal(main, "fixture_udnew");
  lua_pushinteger(main, LUA_RIDX_GLOBALS);
  lua_setglobal(main, "fixture_registry_globals");
  lua_pushlightuserdata(main, &pointer_key);
  lua_setglobal(main, "fixture_lighttoken");
  (void)lua_newuserdatauv(main, 1, 2);
  lua_setglobal(main, "fixture_userdata");
  lua_pushcfunction(main, native_setcapture);
  lua_setglobal(main, "fixture_setcapture");
  lua_pushcfunction(main, native_intern);
  lua_setglobal(main, "fixture_intern");
  lua_pushcfunction(main, native_fail);
  lua_setglobal(main, "fixture_fail");
  /* This rendezvous runs while the allocator is deliberately blocked. */
  lua_pushlightuserdata(main, fixture);
  lua_pushcclosure(main, native_allocate_ready, 1);
  lua_setglobal(main, "fixture_allocate_ready");
  luaL_newmetatable(main, "parallel.resource");
  lua_pushcfunction(main, finalize_resource);
  lua_setfield(main, -2, "__gc");
  lua_pop(main, 1);
  lua_pushcfunction(main, native_resource);
  lua_setglobal(main, "fixture_resource");
  lua_pushboolean(main, strcmp(name, "allocate_failure") == 0);
  lua_setglobal(main, "fixture_inject_failure");
  lua_pushboolean(main, strcmp(name, "allocate_blocked") == 0);
  lua_setglobal(main, "fixture_require_native");
  if (luaL_loadstring(main, source) != LUA_OK ||
      lua_pcall(main, 0, LUA_MULTRET, 0) != LUA_OK)
    return 1;
  results = lua_gettop(main);
  if (strcmp(name, "allocate_blocked") == 0) {
    if (luaL_loadstring(main, "return function() coroutine.yield(); "
                              "fixture_wait(); return true end") != LUA_OK ||
        lua_pcall(main, 0, 1, 0) != LUA_OK)
      return 1;
    lua_replace(main, 1);
  }
  if (strncmp(name, "join_open", 9) == 0) {
    lua_State *owner = prepare_actor(main, 1, &fixture->actors[0], 3);
    int first_argument;
    if (owner == NULL)
      return 1;
    lua_xmove(owner, main, 3);
    first_argument = lua_gettop(main) - 2;
    if (luaL_loadstring(main, join_reader_source) != LUA_OK)
      return 1;
    lua_pushvalue(main, first_argument);
    lua_pushvalue(main, first_argument + 1);
    lua_pushvalue(main, first_argument + 2);
    if (lua_pcall(main, 3, 1, 0) != LUA_OK ||
        prepare_actor(main, lua_gettop(main), &fixture->actors[1], 0) == NULL)
      return 1;
  } else if (strcmp(name, "open") == 0 || strcmp(name, "api") == 0 ||
             strncmp(name, "allocate_open", 13) == 0) {
    lua_State *owner = prepare_actor(main, 1, &fixture->actors[0], 1);
    int reader_index;
    if (owner == NULL)
      return 1;
    lua_xmove(owner, main, 1);
    reader_index = lua_gettop(main);
    if (strcmp(name, "api") == 0) {
      if (luaL_loadstring(main,
                          "local capture=...; return function() "
                          "coroutine.yield(); fixture_setcapture(capture); "
                          "return true end") != LUA_OK)
        return 1;
      lua_pushvalue(main, reader_index);
      if (lua_pcall(main, 1, 1, 0) != LUA_OK)
        return 1;
      reader_index = lua_gettop(main);
    }
    if (prepare_actor(main, reader_index, &fixture->actors[1], 0) == NULL)
      return 1;
  } else if (strcmp(name, "cycle") == 0) {
    int reader_refs[2];
    int i;
    for (i = 0; i < 2; ++i) {
      lua_State *owner = prepare_actor(main, 1, &fixture->actors[i], 1);
      if (owner == NULL)
        return 1;
      lua_xmove(owner, main, 1);
      reader_refs[i] = luaL_ref(main, LUA_REGISTRYINDEX);
    }
    for (i = 0; i < 2; ++i) {
      lua_rawgeti(main, LUA_REGISTRYINDEX, reader_refs[1 - i]);
      lua_xmove(main, fixture->actors[i].state, 1);
      fixture->actors[i].nargs = 1;
    }
  } else {
    if (results != 2)
      return 1;
    if (prepare_actor(main, 1, &fixture->actors[0], 0) == NULL ||
        prepare_actor(main, 2, &fixture->actors[1], 0) == NULL)
      return 1;
  }
  return 0;
}

static void *run_actor(void *context) {
  struct actor *actor = context;
  struct fixture *fixture = actor->fixture;
  cpu_set_t selected;
  CPU_ZERO(&selected);
  CPU_SET(actor->cpu, &selected);
  actor->affinity_error =
      pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected);
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->ready++;
  check(pthread_cond_broadcast(&fixture->changed));
  while (!fixture->start && !fixture->abort_start)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  if (fixture->abort_start) {
    check(pthread_mutex_unlock(&fixture->mutex));
    return NULL;
  }
  check(pthread_mutex_unlock(&fixture->mutex));
  actor->status = lua_resume(actor->state, NULL, actor->nargs, &actor->results);
  while (actor->status == LUA_YIELD && fixture->resume_yields) {
    actor->yields++;
    lua_settop(actor->state, 0);
    (void)lua_gc(actor->state, LUA_GCCOLLECT);
    actor->status = lua_resume(actor->state, NULL, 0, &actor->results);
  }
  check(pthread_mutex_lock(&fixture->mutex));
  actor->done = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  check(pthread_mutex_unlock(&fixture->mutex));
  return NULL;
}

static int verify_results(struct fixture *fixture, const char *name) {
  struct actor *first = &fixture->actors[0];
  struct actor *second = &fixture->actors[1];
  int i;
  for (i = 0; i < 2; ++i) {
    struct actor *actor = &fixture->actors[i];
    if (actor->status != LUA_OK || !actor->polled) {
      fprintf(stderr, "%s actor %d: status=%d poll=%d\n", name, i,
              actor->status, actor->polled);
      if (actor->status != LUA_OK)
        fprintf(stderr, "%s\n", lua_tostring(actor->state, -1));
      return 1;
    }
  }
  if ((strncmp(name, "table_", 6) == 0 && strstr(name, "blocked") == NULL) ||
      strncmp(name, "join_", 5) == 0 || strncmp(name, "capi_", 5) == 0)
    return first->results != 2 || !lua_toboolean(first->state, -2) ||
           lua_tointeger(first->state, -1) != 45150 || second->results != 2 ||
           !lua_toboolean(second->state, -2) ||
           lua_tointeger(second->state, -1) != 45150;
  if (strcmp(name, "closed") == 0)
    return first->results != 1 || lua_tointeger(first->state, -1) != 5000 ||
           second->results != 2 || !lua_toboolean(second->state, -2) ||
           lua_tointeger(second->state, -1) != 5000;
  if (strcmp(name, "allocate_blocked") == 0)
    return first->results != 1 || !lua_toboolean(first->state, -1) ||
           second->results != 2 || !lua_toboolean(second->state, -2) ||
           lua_tointeger(second->state, -1) != 501000;
  if (strncmp(name, "allocate", 8) == 0)
    return first->results != 1 || lua_tointeger(first->state, -1) != 501000 ||
           second->results != 2 || !lua_toboolean(second->state, -2) ||
           lua_tointeger(second->state, -1) != 501000;
  if (strcmp(name, "open") == 0 || strcmp(name, "api") == 0)
    return first->results != 1 || lua_tointeger(first->state, -1) < 0 ||
           second->results != 1 || !lua_toboolean(second->state, -1);
  if (strcmp(name, "cycle") == 0)
    return first->results != 1 || second->results != 1 ||
           lua_tointeger(first->state, -1) != 35000 ||
           lua_tointeger(second->state, -1) != 35000;
  return first->results != 1 || !lua_toboolean(first->state, -1) ||
         second->results != 1 || lua_tointeger(second->state, -1) != 5000;
}

int main(int argc, char **argv) {
  struct fixture fixture;
  pthread_t threads[2];
  cpu_set_t available;
  lua_State *main_state;
  int cpus[2] = {-1, -1};
  int cpu, i, created = 0, failed = 0;
  int allocation_case, table_case, join_case, capi_case;
  unsigned before_allocations;
  if (argc != 2)
    return 2;
  memset(&fixture, 0, sizeof(fixture));
  table_case = strncmp(argv[1], "table_", 6) == 0;
  join_case = strncmp(argv[1], "join_", 5) == 0;
  capi_case = strncmp(argv[1], "capi_", 5) == 0;
  allocation_case = strncmp(argv[1], "allocate", 8) == 0 || table_case ||
                    join_case || capi_case;
  fixture.inject_failure = strcmp(argv[1], "allocate_failure") == 0 ||
                           strcmp(argv[1], "table_failure") == 0 ||
                           strcmp(argv[1], "capi_failure") == 0;
  fixture.resume_yields = strcmp(argv[1], "table_yield") == 0;
  if (sched_getaffinity(0, sizeof(available), &available) != 0)
    return 1;
  for (cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &available)) {
      if (cpus[0] < 0)
        cpus[0] = cpu;
      else {
        cpus[1] = cpu;
        break;
      }
    }
  }
  if (cpus[0] < 0)
    return 1;
  if (cpus[1] < 0)
    cpus[1] = cpus[0];
  check(pthread_mutex_init(&fixture.mutex, NULL));
  check(pthread_cond_init(&fixture.changed, NULL));
  check(pthread_barrier_init(&fixture.managed, NULL, 2));
  main_state = lua_newstate(allocate, &fixture, 0);
  if (main_state == NULL)
    return 1;
  fixture.global = G(main_state);
  luaL_openlibs(main_state);
  (void)lua_gc(main_state, LUA_GCSTOP);
  G(main_state)->gcstopem = 1;
  if (prepare(main_state, &fixture, argv[1]) != 0) {
    fputs("parallel fixture preparation failed\n", stderr);
    failed = 1;
    goto cleanup;
  }
  if (allocation_case) {
    G(main_state)->gcstopem = 0;
    if (fixture.inject_failure) {
      /* Backing storage, rather than a small object in a retained page. */
      lua_newuserdatauv(main_state, 4096, 0);
      lua_pop(main_state, 1);
    }
    if (strstr(argv[1], "generational") != NULL)
      (void)lua_gc(main_state, LUA_GCGEN);
    if (strcmp(argv[1], "allocate_stopped") != 0 &&
        strcmp(argv[1], "table_stopped") != 0 &&
        strcmp(argv[1], "join_stopped") != 0 &&
        strcmp(argv[1], "capi_stopped") != 0 && !fixture.inject_failure)
      (void)lua_gc(main_state, LUA_GCRESTART);
  }
  before_allocations = fixture.allocations;
  for (i = 0; i < 2; ++i) {
    struct actor *actor = &fixture.actors[i];
    if (G(actor->state) != G(main_state))
      abort();
    actor->fixture = &fixture;
    actor->cpu = cpus[i];
    actor->state->state_poll = managed_poll;
    actor->state->state_poll_context = actor;
    actor->state->poll_remaining = 1;
    if (pthread_create(&threads[i], NULL, run_actor, actor) != 0) {
      failed = 1;
      break;
    }
    created++;
  }
  check(pthread_mutex_lock(&fixture.mutex));
  while (!failed && fixture.ready != 2)
    check(pthread_cond_wait(&fixture.changed, &fixture.mutex));
  for (i = 0; i < created; ++i)
    failed |= fixture.actors[i].affinity_error != 0;
  fixture.forbid_allocations = !failed && !allocation_case;
  fixture.abort_start = failed;
  fixture.start = !failed;
  check(pthread_cond_broadcast(&fixture.changed));
  check(pthread_mutex_unlock(&fixture.mutex));
  if (!failed && (strcmp(argv[1], "blocked") == 0 ||
                  strcmp(argv[1], "allocate_blocked") == 0 ||
                  strcmp(argv[1], "table_blocked") == 0 ||
                  strcmp(argv[1], "table_handler_blocked") == 0 ||
                  strcmp(argv[1], "capi_blocked") == 0 ||
                  strcmp(argv[1], "capi_borrowed") == 0 ||
                  strncmp(argv[1], "join_open", 9) == 0)) {
    check(pthread_mutex_lock(&fixture.mutex));
    while (!fixture.native_entered)
      check(pthread_cond_wait(&fixture.changed, &fixture.mutex));
    check(pthread_mutex_unlock(&fixture.mutex));
    check(pthread_join(threads[1], NULL));
    check(pthread_mutex_lock(&fixture.mutex));
    failed |= fixture.actors[0].done;
    fixture.release_native = 1;
    check(pthread_cond_broadcast(&fixture.changed));
    check(pthread_mutex_unlock(&fixture.mutex));
    check(pthread_join(threads[0], NULL));
  } else {
    if (!failed && fixture.inject_failure) {
      LuaWorld *world = &G(main_state)->world;
      check(pthread_mutex_lock(&fixture.mutex));
      while (!fixture.allocator_blocked)
        check(pthread_cond_wait(&fixture.changed, &fixture.mutex));
      check(pthread_mutex_unlock(&fixture.mutex));
      check(pthread_mutex_lock(&world->mutex));
      while (((table_case || capi_case)
                  ? fixture.actors[1].state->object_waiting
                  : fixture.actors[1].state->heap_waiting) == 0)
        luaQ_wait(main_state, &world->mutex, &world->changed, &world->waiters);
      failed |= luaW_activecount(world) != 1 || world->heap_depth == 0 ||
                fixture.actors[1].state->native_depth == 0 ||
                world->heap_owner.token != fixture.failure_owner;
      fixture.waiter_observed = 1;
      check(pthread_mutex_unlock(&world->mutex));
      check(pthread_mutex_lock(&fixture.mutex));
      fixture.release_allocator = 1;
      check(pthread_cond_broadcast(&fixture.changed));
      check(pthread_mutex_unlock(&fixture.mutex));
    }
    for (i = 0; i < created; ++i)
      check(pthread_join(threads[i], NULL));
  }
  fixture.forbid_allocations = 0;
  if (!failed)
    failed = verify_results(&fixture, argv[1]);
  if (fixture.resume_yields)
    failed |= fixture.actors[0].yields != 6;
  if (allocation_case && !table_case && !join_case && !capi_case) {
    failed |= fixture.resources_created !=
              (strcmp(argv[1], "allocate_blocked") == 0 ? 500U : 1000U);
    if (strcmp(argv[1], "allocate_stopped") == 0)
      failed |= fixture.resources_finalized != 0;
    else
      failed |= fixture.resources_finalized == 0;
  }
  if (fixture.inject_failure)
    failed |= !fixture.waiter_observed || fixture.emergency_frees == 0;
  if (strcmp(argv[1], "capi_failure") == 0) {
    failed |= fixture.allocation_failures < 2;
    fixture.failures_left = 0;
  } else
    failed |= fixture.failures_left != 0 ||
              fixture.allocation_failures != (fixture.inject_failure ? 2U : 0U);
  if (!failed)
    printf("parallel VM %s: shared VM, CPU %d/%d, managed overlap, %u "
           "allocator calls\n",
           argv[1], cpus[0], cpus[1], fixture.allocations - before_allocations);
cleanup:
  G(main_state)->gcstopem = 0;
  lua_close(main_state);
  failed |= fixture.resources_created != fixture.resources_finalized;
  check(pthread_barrier_destroy(&fixture.managed));
  check(pthread_cond_destroy(&fixture.changed));
  check(pthread_mutex_destroy(&fixture.mutex));
  return failed;
}
