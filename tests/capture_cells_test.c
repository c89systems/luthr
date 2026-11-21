#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <laccess.h>
#include <lauxlib.h>
#include <ldebug.h>
#include <ldo.h>
#include <lfunc.h>
#include <lgc.h>
#include <lopcodes.h>
#include <lstate.h>
#include <ltable.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *owner;
  int held;
  int release;
  int status;
  int results;
  int retain_gate;
};

static struct fixture *current;
static LuaWorld *observed_world;
static __thread unsigned measured;
static __thread unsigned world_acquisitions;
static __thread unsigned access_calls;
static __thread unsigned read_calls;
static __thread unsigned reading;
static __thread unsigned reader_mutexes;
static __thread unsigned reader_ownership;
static __thread unsigned write_calls;
static const void *measured_function;
static unsigned local_loads;
static unsigned local_stores;
static int collect_pending, collected_pending;
static int collect_lookup, collected_lookup;
static unsigned parallel_started, parallel_finished;

int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
void __real_luaA_begin(lua_State *state, GCObject *object,
                       LuaObjectScope *scope);
void __real_luaA_end(lua_State *state, LuaObjectScope *scope);
void __real_luaF_loadshared(UpVal *cell, TValue *value);
void __real_luaF_writecapture(lua_State *state, UpVal *cell,
                              const TValue *value);
void __real_luaF_storelocals(lua_State *state, StkId first, int count);
StkId __real_luaF_loadlocal(lua_State *state, StkId slot);
void __real_luaF_loadrange(lua_State *state, StkId first, int count);
void __real_luaX_pollrequests(lua_State *state);
int __real_luaH_trygetsharedshortstr(lua_State *, Table *, TString *, TValue *,
                                     lu_byte *);
lu_byte __real_luaH_get(lua_State *, Table *, const TValue *, TValue *);

void __wrap_luaX_pollrequests(lua_State *state) {
  CallInfo *ci = state->ci;
  if (collect_pending && !collected_pending && isLua(ci) &&
      ci->u.l.rootpc != NULL && GET_OPCODE(*ci->u.l.rootpc) == OP_CLOSURE) {
    StkId slot = ci->func.p + 1 + GETARG_A(*ci->u.l.rootpc);
    UpVal *cell;
    for (cell = state->openupval; cell != NULL; cell = cell->open.next) {
      if (cell->level == savestack(state, slot)) {
        /* The recursive closure has been constructed, but the instruction
         * has not published its result to the captured binding yet. */
        collected_pending = 1;
        luaC_fullgc(state, 0);
        break;
      }
    }
  }
  __real_luaX_pollrequests(state);
}
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (reading)
    reader_mutexes++;
  if (measured && observed_world != NULL && mutex == &observed_world->mutex)
    world_acquisitions++;
  return __real_pthread_mutex_lock(mutex);
}

static void collect_at_lookup(lua_State *state) {
  CallInfo *ci = state->ci;
  if (collect_lookup && !collected_lookup && isLua(ci) &&
      ci->u.l.rootpc != NULL && GET_OPCODE(*ci->u.l.rootpc) == OP_GETFIELD) {
    StkId slot = ci->func.p + 1 + GETARG_A(*ci->u.l.rootpc);
    UpVal *cell;
    for (cell = state->openupval; cell != NULL; cell = cell->open.next) {
      if (cell->level == savestack(state, slot)) {
        ptrdiff_t top = savestack(state, state->top.p);
        collected_lookup = 1;
        state->top.p = ci->top.p;
        /* Model a collector observing the pending lookup at VM admission.
         * An emergency collection preserves the borrowed stack addresses. */
        luaC_fullgc(state, 1);
        state->top.p = restorestack(state, top);
        break;
      }
    }
  }
}

int __wrap_luaH_trygetsharedshortstr(lua_State *state, Table *table,
                                     TString *key, TValue *result,
                                     lu_byte *tag) {
  if (collect_lookup && !collected_lookup &&
      strcmp(getstr(key), "payload") == 0)
    return 0; /* Force the VM to publish the frame for a retry. */
  return __real_luaH_trygetsharedshortstr(state, table, key, result, tag);
}

lu_byte __wrap_luaH_get(lua_State *state, Table *table, const TValue *key,
                        TValue *result) {
  collect_at_lookup(state);
  return __real_luaH_get(state, table, key, result);
}

void __wrap_luaA_begin(lua_State *state, GCObject *object,
                       LuaObjectScope *scope) {
  if (reading)
    reader_ownership++;
  measured++;
  access_calls++;
  __real_luaA_begin(state, object, scope);
  measured--;
}

void __wrap_luaA_end(lua_State *state, LuaObjectScope *scope) {
  measured++;
  __real_luaA_end(state, scope);
  measured--;
}

void __wrap_luaF_loadshared(UpVal *cell, TValue *value) {
  measured++;
  read_calls++;
  reading++;
  __real_luaF_loadshared(cell, value);
  reading--;
  measured--;
}

void __wrap_luaF_writecapture(lua_State *state, UpVal *cell,
                              const TValue *value) {
  measured++;
  write_calls++;
  __real_luaF_writecapture(state, cell, value);
  measured--;
}

void __wrap_luaF_storelocals(lua_State *state, StkId first, int count) {
  if (isLua(state->ci) && ci_func(state->ci) == measured_function)
    local_stores++;
  measured++;
  __real_luaF_storelocals(state, first, count);
  measured--;
}

StkId __wrap_luaF_loadlocal(lua_State *state, StkId slot) {
  if (isLua(state->ci) && ci_func(state->ci) == measured_function)
    local_loads++;
  return __real_luaF_loadlocal(state, slot);
}

void __wrap_luaF_loadrange(lua_State *state, StkId first, int count) {
  if (isLua(state->ci) && ci_func(state->ci) == measured_function)
    local_loads++;
  __real_luaF_loadrange(state, first, count);
}

static int select_frame(lua_State *state) {
  measured_function = lua_topointer(state, 1);
  return 0;
}

static void require(int condition) {
  if (!condition) {
    fputs("capture cell invariant failed\n", stderr);
    abort();
  }
}

static void run(lua_State *state, const char *source) {
  if (luaL_dostring(state, source) != LUA_OK) {
    fprintf(stderr, "capture cells: %s\n", lua_tostring(state, -1));
    abort();
  }
}

/* Deliberately retain the owner's execution gate. A shared capture must make
 * progress without a reservation on this unrelated execution resource. */
static int hold_owner(lua_State *state) {
  if (current->retain_gate)
    luaX_lockstate(state);
  require(pthread_mutex_lock(&current->mutex) == 0);
  current->held = 1;
  require(pthread_cond_broadcast(&current->changed) == 0);
  while (!current->release)
    require(pthread_cond_wait(&current->changed, &current->mutex) == 0);
  require(pthread_mutex_unlock(&current->mutex) == 0);
  if (current->retain_gate)
    luaX_unlockstate(state);
  return 0;
}

static void *resume_owner(void *context) {
  struct fixture *fixture = context;
  fixture->status = lua_resume(fixture->owner, NULL, 0, &fixture->results);
  return NULL;
}

static int capture_ready(lua_State *state) {
  (void)state;
  __atomic_store_n(&parallel_started, 1, __ATOMIC_RELEASE);
  return 0;
}

static int capture_done(lua_State *state) {
  lua_pushboolean(state, __atomic_load_n(&parallel_finished, __ATOMIC_ACQUIRE));
  return 1;
}

static void *resume_mixed(void *context) {
  struct fixture *fixture = context;
  fixture->status = lua_resume(fixture->owner, NULL, 1, &fixture->results);
  return NULL;
}

static void mixed_capture(lua_State *state) {
  struct fixture fixture = {0};
  LClosure *reader, *writer;
  pthread_t thread;
  __atomic_store_n(&parallel_started, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&parallel_finished, 0, __ATOMIC_RELAXED);
  lua_pushcfunction(state, capture_ready);
  lua_setglobal(state, "capture_ready");
  lua_pushcfunction(state, capture_done);
  lua_setglobal(state, "capture_done");
  run(state, "local function pair() local value; "
             "return function() return value end,function(v) value=v end end; "
             "local a,b=pair(); local c,d=pair(); return a,b,c,d");
  require(lua_gettop(state) == 4);
  fixture.owner = lua_newthread(state);
  require(
      luaL_loadstring(
          fixture.owner,
          "local write=...; local low,high=math.mininteger,math.maxinteger; "
          "local i=0; capture_ready(); repeat for n=1,128 do "
          "i=i+1; local k=i%8; local v; "
          "if k==0 then v=low elseif k==1 then v=high "
          "elseif k==2 then v=1.5 elseif k==3 then v=false "
          "elseif k==4 then v='capture payload' "
          "elseif k==5 then v={marker=42} "
          "elseif k==6 then v=coroutine.running() end; write(v) "
          "end until capture_done(); return i") == LUA_OK);
  lua_pushvalue(state, 2);
  lua_xmove(state, fixture.owner, 1); /* promotes the source cell */
  lua_upvaluejoin(state, 3, 1, 1, 1);
  lua_upvaluejoin(state, 4, 1, 2, 1);
  require(lua_upvalueid(state, 3, 1) == lua_upvalueid(state, 1, 1));
  luaX_lockstate(state);
  reader = clLvalue(s2v(state->stack.p + 3));
  writer = clLvalue(s2v(state->stack.p + 4));
  require(reader->sharing == (uintptr_t)state &&
          writer->sharing == (uintptr_t)state &&
          (reader->upvals[0]->sharing & LUA_OBJECT_SHARED) != 0 &&
          reader->upvals[0] == writer->upvals[0]);
  luaX_unlockstate(state);
  require(pthread_create(&thread, NULL, resume_mixed, &fixture) == 0);
  while (!__atomic_load_n(&parallel_started, __ATOMIC_ACQUIRE))
    sched_yield();
  require(luaL_loadstring(
              state,
              "local read,write=...; "
              "local low,high=math.mininteger,math.maxinteger; "
              "for n=1,3200 do local v=read(); local t=type(v); "
              "if t=='number' then assert(v==low or v==high or v==1.5) "
              "elseif t=='string' then assert(v=='capture payload') "
              "elseif t=='table' then assert(v.marker==42) "
              "else assert(t=='nil' or t=='boolean' or t=='thread') end; "
              "write(n%2==0 and {marker=42} or low); "
              "if n%64==0 then collectgarbage('collect') end end") == LUA_OK);
  lua_pushvalue(state, 3);
  lua_pushvalue(state, 4);
  require(lua_pcall(state, 2, 0, 0) == LUA_OK);
  __atomic_store_n(&parallel_finished, 1, __ATOMIC_RELEASE);
  require(pthread_join(thread, NULL) == 0);
  require(fixture.status == LUA_OK && fixture.results == 1 &&
          lua_tointeger(fixture.owner, -1) >= 128);
  require(luaL_loadstring(state, "local read,write=...; write({marker=43}); "
                                 "collectgarbage('collect'); "
                                 "assert(read().marker==43)") == LUA_OK);
  lua_pushvalue(state, 3);
  lua_pushvalue(state, 4);
  require(lua_pcall(state, 2, 0, 0) == LUA_OK);
  lua_settop(state, 0);
  lua_gc(state, LUA_GCCOLLECT);
}

static void independent(lua_State *state) {
  struct fixture fixture;
  lua_State *reader;
  pthread_t thread;
  int results;
  memset(&fixture, 0, sizeof(fixture));
  fixture.retain_gate = 1;
  require(pthread_mutex_init(&fixture.mutex, NULL) == 0);
  require(pthread_cond_init(&fixture.changed, NULL) == 0);
  current = &fixture;
  lua_pushcfunction(state, hold_owner);
  lua_setglobal(state, "hold_owner");
  lua_gc(state, LUA_GCSTOP);
  fixture.owner = lua_newthread(state);
  require(luaL_loadstring(
              fixture.owner,
              "local x=7; "
              "local function read() return x end; "
              "local function write(v) x=v end; "
              "coroutine.yield(read,write); hold_owner(); return x") == LUA_OK);
  require(lua_resume(fixture.owner, NULL, 0, &results) == LUA_YIELD &&
          results == 2);
  lua_xmove(fixture.owner, state, 2);
  reader = lua_newthread(state);
  require(luaL_loadstring(
              reader,
              "local read,write=...; assert(read()==7); "
              "write({answer=42}); assert(read().answer==42); return read()") ==
          LUA_OK);
  lua_pushvalue(state, -3);
  lua_pushvalue(state, -3);
  lua_xmove(state, reader, 2);
  require(pthread_create(&thread, NULL, resume_owner, &fixture) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.held)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(lua_resume(reader, NULL, 2, &results) == LUA_OK && results == 1);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  fixture.release = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_join(thread, NULL) == 0);
  require(fixture.status == LUA_OK && fixture.results == 1);
  lua_getfield(fixture.owner, -1, "answer");
  require(lua_tointeger(fixture.owner, -1) == 42);
  require(lua_topointer(fixture.owner, -2) == lua_topointer(reader, -1));
  require(pthread_cond_destroy(&fixture.changed) == 0);
  require(pthread_mutex_destroy(&fixture.mutex) == 0);
  lua_gc(state, LUA_GCRESTART);
}

static const char semantics[] =
    "local x=0; local function read() return x end; "
    "local function write(v) x=v end; "
    "x=41; assert(read()==41); x=x+1; assert(read()==42); "
    "x=false or 9; assert(read()==9); x=nil; assert(read()==nil); "
    "x=function() return 13 end; x=x(); assert(read()==13); "
    "x=(function() return 14 end)(); assert(read()==14); "
    "write(15); assert(x==15); "
    "x=setmetatable({}, {__add=function() return 16 end}); "
    "x=x+1; assert(read()==16); "
    "x='a'..read(); assert(read()=='a16'); "
    "local function edit() "
    "  for n=1,20 do local name=debug.getlocal(2,n); "
    "    if name=='x' then debug.setlocal(2,n,99); return end "
    "  end; error('missing local') "
    "end; edit(); assert(x==99 and read()==99); "
    "local f={}; for i=1,3 do f[i]=function() return i end end; "
    "assert(f[1]()==1 and f[2]()==2 and f[3]()==3); "
    "local floats={}; for i=1.5,3.5 do "
    " floats[#floats+1]=function() return i end end; "
    "assert(floats[1]()==1.5 and floats[3]()==3.5); "
    "local g={}; for k,v in ipairs({7,8,9}) do "
    "  g[k]=function() return k,v end "
    "end; local a,b=g[2](); assert(a==2 and b==8); "
    "local y=77; local function other() return y end; "
    "debug.upvaluejoin(read,1,other,1); x=100; assert(read()==77); "
    "write(101); assert(x==101 and read()==77); "
    "local co=coroutine.create(function() "
    "  local z=1; local function rz() return z end; "
    "  z=setmetatable({}, {__add=function() coroutine.yield(); return 43 "
    "end}); "
    "  z=z+1; assert(rz()==43); return rz "
    "end); assert(coroutine.resume(co)); "
    "local ok,rz=coroutine.resume(co); assert(ok and rz()==43); "
    "local co2=coroutine.create(function() "
    "  local z=0; local function rz() return z end; "
    "  z=(function() coroutine.yield(); return 44 end)(); "
    "  assert(rz()==44); return rz "
    "end); assert(coroutine.resume(co2)); "
    "local ok,rz=coroutine.resume(co2); assert(ok and rz()==44); ";

static const char barriers[] =
    "local x; local function read() return x end; "
    "collectgarbage('collect'); "
    "for n=1,140 do x={n=n}; collectgarbage('step',1); assert(read().n==n) "
    "end; "
    "local co=coroutine.create(function() "
    " local y; local function ry() return y end; "
    " coroutine.yield(ry); y={answer=42}; return ry "
    "end); local ok,ry=coroutine.resume(co); assert(ok); "
    "collectgarbage('collect'); assert(coroutine.resume(co)); "
    "co=nil; collectgarbage('collect'); assert(ry().answer==42); "
    "local co2=coroutine.create(function() local z=0; "
    " coroutine.yield(function() return z end,function(v) z=v end) end); "
    "local ok,read,write=coroutine.resume(co2); assert(ok); "
    "write({answer=43}); co2=nil; collectgarbage('collect'); "
    "assert(read().answer==43); "
    "local function make() local z; "
    " return function(v) z=v end,function() return z end end; "
    "local write,read=make(); collectgarbage('collect'); "
    "for n=1,140 do write({n=n}) end; "
    "collectgarbage('collect'); assert(read().n==140); ";

static const char operands[] =
    "local function work() "
    " local cell=17; local function read() return cell end; "
    " local registry=debug.getregistry(); registry.capture_probe=read; "
    " registry.capture_probe=nil; "
    " local sum=0; for i=1,20000 do sum=sum+i end; "
    " assert(read()==17); return sum+cell-17 "
    "end; select_frame(work); assert(work()==200010000)";

/* Writes through a closure leave the owner's register shadow obsolete. Each
 * subsequent operation must observe the cell, including direct A operands and
 * contiguous call/return ranges, without a blanket frame refresh. */
static const char operand_semantics[] =
    "local x,y,t,k,f; local function write(a,b,c,d,e) x,y,t,k,f=a,b,c,d,e end; "
    "write(7,3,{answer=41,[3]=42},3,function(a,b) return a+b end); "
    "assert(x+1==8 and x+y==10 and x-1==6 and x*y==21); "
    "assert(-x==-7 and ~x==~7 and not (not x)); "
    "assert(x//y==2 and x%y==1 and x<<y==56 and x>>y==0); "
    "assert((x&y)==3 and (x|y)==7 and (x~y)==4); "
    "assert(x>y and y<x and x>=y and y<=x and x==7 and x~=y); "
    "assert(t[k]==42 and t[3]==42 and t.answer==41); "
    "t[k]=43; t[1]=44; t.answer=45; "
    "assert(t[3]==43 and t[1]==44 and t.answer==45); "
    "assert(f(x,y)==10); local z=x or y; assert(z==7); "
    "write('ab','cd',setmetatable({}, {__len=function() return 13 end}),0,f); "
    "assert(x..y=='abcd' and #x==2 and #t==13); "
    "write(false,23,{method=function(self,v) return v+1 end},0,f); "
    "if x then error('stale truth value') end; z=x or y; assert(z==23); "
    "assert(t:method(y)==24); local list={x,y}; "
    "assert(list[1]==false and list[2]==23); "
    "local function returns() local value=0; "
    " local function set(v) value=v end; set(91); return value end; "
    "assert(returns()==91); "
    "local function tail() local fn=function() return 1 end; "
    " local function set(v) fn=v end; set(function() return 92 end); "
    " return fn() end; assert(tail()==92); "
    "local function many() local a,b=0,0; "
    " local function set() a,b=93,94 end; set(); return a,b end; "
    "local a,b=many(); assert(a==93 and b==94); "
    "local function varargs(...) local a,b=...; "
    " local function set() a,b=b,a end; set(); return a,b,... end; "
    "local a,b,c,d=varargs(95,96); assert(a==96 and b==95 and c==95 and "
    "d==96); "
    "local function reuse(n) if n==0 then return 97 end; "
    " do local v=n; local function read() return v end; assert(read()==n) end; "
    " return reuse(n-1) end; assert(reuse(200)==97); ";

static const char shadow_roots[] =
    "local weak=setmetatable({}, {__mode='v'}); "
    "local value={}; weak[1]=value; "
    "local function replace(v) value=v end; "
    "local registry=debug.getregistry(); registry.capture_probe=replace; "
    "registry.capture_probe=nil; replace(nil); "
    "collectgarbage('collect'); collectgarbage('collect'); "
    "assert(weak[1]==nil, 'obsolete capture shadow retained a value'); "
    "assert(value==nil); ";

static const char snapshot_semantics[] =
    "local function work() "
    " local x,y,t; local function set(a,b,c) x,y,t=a,b,c end; "
    " set(7,3,{answer=41}); "
    " debug.sethook(function() collectgarbage('collect') end,'',1); "
    " assert(x+y==10 and x+1==8 and -x==-7 and x<8 and x>y); "
    " assert(t.answer==41); t.answer=x+y; assert(t.answer==10); "
    " set('left','right',{method=function(self,v) return v..'!' end}); "
    " x=x..y; assert(x=='leftright'); assert(t:method(x)=='leftright!'); "
    " x=setmetatable({}, {__add=function() collectgarbage('collect'); "
    " return {answer=42} end}); x=x+1; assert(x.answer==42); "
    " x=(function() collectgarbage('collect'); return {answer=43} end)(); "
    " assert(x.answer==43); "
    " x={answer=44}; assert(x.answer==44); "
    " x=function() return {answer=45} end; assert(x().answer==45); "
    " debug.sethook(); return x "
    "end; assert(work()().answer==45); ";

static const char lookup_roots[] =
    "local value={}; local weak=setmetatable({value},{__mode='v'}); "
    "local function replace(v) value=v end; "
    "local registry=debug.getregistry(); registry.capture_probe=replace; "
    "registry.capture_probe=nil; replace(nil); "
    "collectgarbage('collect'); collectgarbage('collect'); "
    "assert(weak[1]==nil); "
    "local source={payload={answer=46}}; "
    "registry.capture_probe=source; registry.capture_probe=nil; "
    "value=source.payload; "
    "assert(value.answer==46); ";

/* A detached native owner need not resume or read the binding for a foreign
 * writer's obsolete value to become collectible. Reclamation must not depend
 * on the owner's dispatch frequency. */
static void foreign_shadow(lua_State *state) {
  struct fixture fixture;
  lua_State *reader;
  pthread_t thread;
  int results;
  memset(&fixture, 0, sizeof(fixture));
  require(pthread_mutex_init(&fixture.mutex, NULL) == 0);
  require(pthread_cond_init(&fixture.changed, NULL) == 0);
  current = &fixture;
  lua_pushcfunction(state, hold_owner);
  lua_setglobal(state, "hold_owner");
  fixture.owner = lua_newthread(state);
  require(luaL_loadstring(
              fixture.owner,
              "local value={}; "
              "local weak=setmetatable({value},{__mode='v'}); "
              "local function read() return value end; "
              "local function write(v) value=v end; "
              "coroutine.yield(read,write,weak); hold_owner(); return value") ==
          LUA_OK);
  require(lua_resume(fixture.owner, NULL, 0, &results) == LUA_YIELD &&
          results == 3);
  lua_xmove(fixture.owner, state, 3);
  reader = lua_newthread(state);
  require(luaL_loadstring(
              reader, "local read,write,weak=...; write(nil); "
                      "collectgarbage('collect'); collectgarbage('collect'); "
                      "assert(weak[1]==nil); assert(read()==nil)") == LUA_OK);
  lua_pushvalue(state, -4);
  lua_pushvalue(state, -4);
  lua_pushvalue(state, -4);
  lua_xmove(state, reader, 3);
  require(pthread_create(&thread, NULL, resume_owner, &fixture) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.held)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(lua_resume(reader, NULL, 3, &results) == LUA_OK);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  fixture.release = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_join(thread, NULL) == 0);
  require(fixture.status == LUA_OK && fixture.results == 1);
  require(lua_isnil(fixture.owner, -1));
  require(pthread_cond_destroy(&fixture.changed) == 0);
  require(pthread_mutex_destroy(&fixture.mutex) == 0);
}

/* High registers, colliding filter bits and stack movement must retain full
 * semantics. Generate source here to avoid an unreadable fixture.
 */
static void wide_operands(lua_State *state) {
  luaL_Buffer source;
  int i;
  luaL_buffinit(state, &source);
  luaL_addstring(&source, "return function(...) local ");
  for (i = 0; i < 140; i++) {
    char name[24];
    snprintf(name, sizeof(name), "%sv%d", i ? "," : "", i);
    luaL_addstring(&source, name);
  }
  luaL_addstring(
      &source,
      "; local function write() v0=3; v63=7; v64=11; v127=13; "
      "v139=17 end; write(); "
      "assert(v0+v64==14 and v63+v127==20 and v139==17); "
      "local function grow(n) if n==0 then return end; "
      "grow(n-1); end; grow(100); "
      "assert(v0==3 and v63==7 and v64==11 and v127==13 and v139==17); "
      "return v0,v63,v64,v127,v139 end");
  luaL_pushresult(&source);
  require(luaL_loadstring(state, lua_tostring(state, -1)) == LUA_OK);
  require(lua_pcall(state, 0, 1, 0) == LUA_OK);
  require(lua_pcall(state, 0, 5, 0) == LUA_OK);
  require(lua_tointeger(state, -5) == 3);
  require(lua_tointeger(state, -4) == 7);
  require(lua_tointeger(state, -3) == 11);
  require(lua_tointeger(state, -2) == 13);
  require(lua_tointeger(state, -1) == 17);
}

struct compiled {
  char *bytes;
  size_t size, offset, chunk, mapping_size;
  void *mapping;
};

static int write_chunk(lua_State *state, const void *data, size_t size,
                       void *context) {
  struct compiled *code = context;
  (void)state;
  if (size != 0) {
    code->bytes = realloc(code->bytes, code->size + size);
    require(code->bytes != NULL);
    memcpy(code->bytes + code->size, data, size);
    code->size += size;
  }
  return 0;
}

static const char *read_chunk(lua_State *state, void *context, size_t *size) {
  struct compiled *code = context;
  const char *bytes = code->mapping != NULL ? code->mapping : code->bytes;
  size_t remaining = code->size - code->offset;
  (void)state;
  *size = remaining < code->chunk ? remaining : code->chunk;
  bytes += code->offset;
  code->offset += *size;
  return *size == 0 ? NULL : bytes;
}

static unsigned hook_events;
static void inspect_instruction(lua_State *state, lua_Debug *event) {
  require(lua_getinfo(state, "Sl", event));
  if (event->event == LUA_HOOKLINE) {
    require(event->currentline > 0);
    require(strcmp(event->source, "=lowered") == 0);
  }
  hook_events++;
}

/* The same captures, conditional writes, fused iterators, stack moves and
 * yielded metamethods must work from source and fragmented portable chunks.
 * Fixed chunks remain read-only and borrowed until state destruction. Hooks
 * inspect canonical program positions while the execution stream is running. */
static void lowered(lua_State *state, const char *kind, struct compiled *code) {
  int fixed = strstr(kind, "fixed") != NULL;
  if (strstr(kind, "generational") != NULL)
    lua_gc(state, LUA_GCGEN);
  require(luaL_loadbuffer(state, semantics, sizeof(semantics) - 1,
                          "=lowered") == LUA_OK);
  if (strstr(kind, "source") == NULL) {
    require(lua_dump(state, write_chunk, code, 0) == 0);
    lua_pop(state, 1);
    code->chunk = 7;
    if (fixed) {
      size_t page = (size_t)sysconf(_SC_PAGESIZE);
      require(page != 0);
      code->mapping_size = (code->size + page - 1) / page * page;
      code->mapping = mmap(NULL, code->mapping_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      require(code->mapping != MAP_FAILED);
      memcpy(code->mapping, code->bytes, code->size);
      require(mprotect(code->mapping, code->mapping_size, PROT_READ) == 0);
      code->chunk = code->size;
    }
    require(lua_load(state, read_chunk, code, "=restored", fixed ? "B" : "b") ==
            LUA_OK);
  }
  lua_sethook(state, inspect_instruction,
              LUA_MASKLINE | LUA_MASKCOUNT | LUA_MASKCALL | LUA_MASKRET, 3);
  if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(state, -1));
    abort();
  }
  lua_sethook(state, NULL, 0, 0);
  require(hook_events > 100);
  if (fixed)
    require(memcmp(code->mapping, code->bytes, code->size) == 0);
  lua_gc(state, LUA_GCCOLLECT);
}

int main(int argc, char **argv) {
  struct compiled code = {0};
  lua_State *state = luaL_newstate();
  require(state != NULL && argc == 2);
  luaL_openlibs(state);
  if (strcmp(argv[1], "independent") == 0)
    independent(state);
  else if (strncmp(argv[1], "mixed_", 6) == 0) {
    if (strcmp(argv[1], "mixed_generational") == 0)
      lua_gc(state, LUA_GCGEN);
    mixed_capture(state);
  } else if (strncmp(argv[1], "private_", 8) == 0) {
    if (strcmp(argv[1], "private_generational") == 0)
      lua_gc(state, LUA_GCGEN);
    lua_pushcfunction(state, select_frame);
    lua_setglobal(state, "select_frame");
    run(state,
        "local function work() "
        "local value; local function read() return value end; "
        "local function write(v) value=v end; "
        "local values={false,true,math.mininteger,math.maxinteger,1.5,"
        "'capture payload',{},function() return 42 end,coroutine.running()}; "
        "for round=1,48 do for i=1,#values do write(values[i]); "
        "assert(rawequal(read(),values[i])) end; write(nil); "
        "assert(read()==nil); write({marker=round}); "
        "collectgarbage('step',1); assert(read().marker==round) end; "
        "value={marker=49}; collectgarbage('collect'); "
        "assert(value.marker==49 and read().marker==49); return read "
        "end; select_frame(work); local read=work(); "
        "collectgarbage('collect'); assert(read().marker==49)");
    require(local_loads == 0 && local_stores == 0);
    measured_function = NULL;
    run(state, "local co=coroutine.create(function(...) "
               "  local value={marker=select(1,...)}; "
               "  local function read() return value end; "
               "  value=setmetatable(value,{__add=function(old,n) "
               "    coroutine.yield(read); return {marker=old.marker+n} end}); "
               "  value=value+1; assert(value==read()); return read "
               "end); local ok,read=coroutine.resume(co,51,0,0); "
               "assert(ok and read().marker==51); collectgarbage('collect'); "
               "local ok,result=coroutine.resume(co); "
               "assert(ok and result==read and read().marker==52); "
               "co=nil; collectgarbage('collect'); assert(read().marker==52)");
  } else if (strncmp(argv[1], "lowered_", 8) == 0)
    lowered(state, argv[1], &code);
  else if (strcmp(argv[1], "readers") == 0) {
    run(state, "local check=assert; "
               "local x=17; local function read() return x end; "
               "for i=1,20000 do check(read()==17) end");
    fprintf(stderr, "capture readers: mutex=%u ownership=%u\n", reader_mutexes,
            reader_ownership);
    require(read_calls < 100 && reader_mutexes == 0 && reader_ownership == 0);
  } else if (strcmp(argv[1], "access") == 0) {
    observed_world = &G(state)->world;
    lua_gc(state, LUA_GCSTOP);
    require(luaL_loadstring(
                state, "local check=assert; "
                       "local x=0; local function next() x=x+1; return x end; "
                       "local t={0}; for i=1,40 do t[1]=next(); "
                       "check(t[1]==i) end") == LUA_OK);
    access_calls = read_calls = write_calls = world_acquisitions = 0;
    require(lua_pcall(state, 0, 0, 0) == LUA_OK);
    fprintf(stderr, "access=%u read=%u write=%u world=%u\n", access_calls,
            read_calls, write_calls, world_acquisitions);
    require(access_calls > 0 && read_calls <= 1 && write_calls == 0);
    require(world_acquisitions == 0);
    observed_world = NULL;
  } else if (strcmp(argv[1], "uncaptured") == 0) {
    lua_pushcfunction(state, select_frame);
    lua_setglobal(state, "select_frame");
    run(state, "local cell=17; local function read() return cell end; "
               "local function work() local n=0; "
               "for i=1,20000 do n=n+i end; return n end; "
               "select_frame(work); "
               "assert(work()==200010000 and read()==17)");
    require(local_loads == 0);
  } else if (strcmp(argv[1], "operands") == 0) {
    lua_pushcfunction(state, select_frame);
    lua_setglobal(state, "select_frame");
    run(state, operands);
    fprintf(stderr, "capture operand refreshes=%u\n", local_loads);
    require(local_loads > 0 && local_loads < 50);
    measured_function = NULL;
    run(state, operand_semantics);
    wide_operands(state);
    lua_settop(state, 0);
    run(state, shadow_roots);
  } else if (strcmp(argv[1], "shadow_incremental") == 0 ||
             strcmp(argv[1], "shadow_generational") == 0) {
    if (strcmp(argv[1], "shadow_generational") == 0)
      lua_gc(state, LUA_GCGEN);
    collect_pending = 1;
    run(state, "local function recursive(n) if n==0 then return 42 end; "
               "return recursive(n-1) end; assert(recursive(5)==42)");
    require(collected_pending == 1);
    collect_pending = 0;
    collect_lookup = 1;
    run(state, lookup_roots);
    require(collected_lookup == 1);
    collect_lookup = 0;
    run(state, shadow_roots);
    run(state, snapshot_semantics);
    foreign_shadow(state);
  } else if (strcmp(argv[1], "semantics") == 0)
    run(state, semantics);
  else {
    require(strcmp(argv[1], "incremental") == 0 ||
            strcmp(argv[1], "generational") == 0);
    if (strcmp(argv[1], "generational") == 0)
      lua_gc(state, LUA_GCGEN);
    run(state, barriers);
  }
  lua_close(state);
  if (code.mapping != NULL)
    require(munmap(code.mapping, code.mapping_size) == 0);
  free(code.bytes);
  return 0;
}
