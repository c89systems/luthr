#include <laccess.h>
#include <lauxlib.h>
#include <lfunc.h>
#include <lstate.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *owner;
  int held;
  int release;
  int status;
  int results;
};

static struct fixture *current;
static LuaWorld *observed_world;
static __thread unsigned measured;
static __thread unsigned world_acquisitions;
static __thread unsigned access_calls;
static __thread unsigned read_calls;
static __thread unsigned write_calls;
static __thread unsigned barrier_flushes;

int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
void __real_luaA_begin(lua_State *state, GCObject *object,
                       LuaObjectScope *scope);
void __real_luaA_end(lua_State *state, LuaObjectScope *scope);
void __real_luaF_readcapture(lua_State *state, LClosure *closure, int n,
                             LuaNativeRoot *value);
void __real_luaF_writecapture(lua_State *state, LClosure *closure, int n,
                              const TValue *value);
void __real_luaF_storelocals(lua_State *state, StkId first, int count);
void __real_luaW_stop(lua_State *state);

void __wrap_luaW_stop(lua_State *state) {
  if (state->barrier_count == LUA_BARRIER_CAPACITY)
    barrier_flushes++;
  __real_luaW_stop(state);
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (measured && observed_world != NULL && mutex == &observed_world->mutex)
    world_acquisitions++;
  return __real_pthread_mutex_lock(mutex);
}

void __wrap_luaA_begin(lua_State *state, GCObject *object,
                       LuaObjectScope *scope) {
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

void __wrap_luaF_readcapture(lua_State *state, LClosure *closure, int n,
                             LuaNativeRoot *value) {
  measured++;
  read_calls++;
  __real_luaF_readcapture(state, closure, n, value);
  measured--;
}

void __wrap_luaF_writecapture(lua_State *state, LClosure *closure, int n,
                              const TValue *value) {
  measured++;
  write_calls++;
  __real_luaF_writecapture(state, closure, n, value);
  measured--;
}

void __wrap_luaF_storelocals(lua_State *state, StkId first, int count) {
  measured++;
  __real_luaF_storelocals(state, first, count);
  measured--;
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
  luaX_lockstate(state);
  require(pthread_mutex_lock(&current->mutex) == 0);
  current->held = 1;
  require(pthread_cond_broadcast(&current->changed) == 0);
  while (!current->release)
    require(pthread_cond_wait(&current->changed, &current->mutex) == 0);
  require(pthread_mutex_unlock(&current->mutex) == 0);
  luaX_unlockstate(state);
  return 0;
}

static void *resume_owner(void *context) {
  struct fixture *fixture = context;
  fixture->status = lua_resume(fixture->owner, NULL, 0, &fixture->results);
  return NULL;
}

static void independent(lua_State *state) {
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

int main(int argc, char **argv) {
  lua_State *state = luaL_newstate();
  require(state != NULL && argc == 2);
  luaL_openlibs(state);
  if (strcmp(argv[1], "independent") == 0)
    independent(state);
  else if (strcmp(argv[1], "access") == 0) {
    observed_world = &G(state)->world;
    access_calls = read_calls = write_calls = world_acquisitions = 0;
    lua_gc(state, LUA_GCSTOP);
    run(state, "local x=0; local function next() x=x+1; return x end; "
               "local t={0}; for i=1,40 do t[1]=next(); assert(t[1]==i) end");
    fprintf(stderr, "access=%u read=%u write=%u world=%u\n", access_calls,
            read_calls, write_calls, world_acquisitions);
    require(access_calls > 0 && read_calls > 0 && write_calls > 0);
    require(world_acquisitions == 0);
    observed_world = NULL;
  } else if (strcmp(argv[1], "semantics") == 0)
    run(state, semantics);
  else {
    require(strcmp(argv[1], "incremental") == 0 ||
            strcmp(argv[1], "generational") == 0);
    if (strcmp(argv[1], "generational") == 0)
      lua_gc(state, LUA_GCGEN);
    run(state, barriers);
    if (strcmp(argv[1], "generational") == 0)
      require(barrier_flushes > 0);
  }
  lua_close(state);
  return 0;
}
