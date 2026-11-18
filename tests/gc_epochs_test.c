#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
#include <lualib.h>
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

static unsigned records[3], unchanged;
static int inspecting;

void __real_luaC_scansweep(lua_State *, LuaSweepWork *);
void __wrap_luaC_scansweep(lua_State *state, LuaSweepWork *work) {
  GCObject *objects[LUA_TRACE_BATCH], *links[LUA_TRACE_BATCH];
  lu_byte marks[LUA_TRACE_BATCH];
  unsigned count = 0, i;
  if (inspecting) {
    records[work->kind] += work->count;
    if (inspecting == 2 && work->phase == SWEEP_OBJECTS &&
        (work->kind == SWEEP_MAJOR || work->kind == SWEEP_OLD)) {
      GCObject *object;
      for (object = work->next; object != NULL;
           object = luaC_allocnext(object)) {
        if (work->kind == SWEEP_OLD && object->tt == LUA_VTHREAD)
          continue; /* Threads enter their watched queue during conversion. */
        require(count < LUA_TRACE_BATCH && !isdead(G(state), object));
        objects[count] = object;
        links[count] = luaC_allocnext(object);
        marks[count++] = getmarked(object);
      }
    }
  }
  __real_luaC_scansweep(state, work);
  for (i = 0; i < count; i++) {
    require(getmarked(objects[i]) == marks[i]);
    require(luaC_allocnext(objects[i]) == links[i]);
    unchanged++;
  }
}

static void execute(lua_State *state, const char *source) {
  if (luaL_loadstring(state, source) != LUA_OK ||
      lua_pcall(state, 0, 0, 0) != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(state, -1));
    abort();
  }
}

static void minor(lua_State *state) {
  unsigned steps = 0;
  do {
    lua_gc(state, LUA_GCSTEP, (size_t)0);
    require(++steps < 10000);
  } while (luaC_tracing(state) || luaC_sweeping(state));
}

static void reset(void) {
  memset(records, 0, sizeof(records));
  unchanged = 0;
}

static void live(lua_State *state, int generational) {
  if (generational)
    lua_gc(state, LUA_GCGEN);
  execute(state, "root={}; for i=1,2048 do root[i]={i} end");
  lua_gc(state, LUA_GCCOLLECT);
  lua_gc(state, LUA_GCCOLLECT);
  reset();
  inspecting = 2;
  lua_gc(state, LUA_GCCOLLECT);
  inspecting = 0;
  require(unchanged >= 2048);
  if (generational)
    require(records[SWEEP_MAJOR] == 0 && records[SWEEP_OLD] >= unchanged);
  else
    require(records[SWEEP_MAJOR] == unchanged);
  execute(state, "for i=1,2048 do assert(root[i][1]==i) end");
}

static void modes(lua_State *state) {
  execute(state, "root={}; for i=1,2048 do root[i]={i} end");
  lua_gc(state, LUA_GCGEN);
  reset();
  inspecting = 1;
  lua_gc(state, LUA_GCCOLLECT);
  inspecting = 0;
  require(records[SWEEP_MAJOR] == 0 && records[SWEEP_OLD] >= 2048);
  reset();
  inspecting = 1;
  require(lua_gc(state, LUA_GCINC) == LUA_GCGEN);
  inspecting = 0;
  require(records[SWEEP_MAJOR] == 0 && records[SWEEP_OLD] == 0 &&
          records[SWEEP_YOUNG] == 0);
  reset();
  inspecting = 1;
  require(lua_gc(state, LUA_GCGEN) == LUA_GCINC);
  inspecting = 0;
  require(records[SWEEP_MAJOR] == 0 && records[SWEEP_OLD] >= 2048);
  execute(state, "for i=1,2048 do assert(root[i][1]==i) end");
}

static void *release_string(void *context, void *pointer, size_t old,
                            size_t size) {
  unsigned *count = context;
  require(old == 65 && size == 0);
  (*count)++;
  free(pointer);
  return NULL;
}

static void string(lua_State *state, unsigned *count) {
  char *text = malloc(65);
  require(text != NULL);
  memset(text, 'x', 64);
  text[64] = '\0';
  lua_pushexternalstring(state, text, 64, release_string, count);
}

static void rotations(lua_State *state) {
  unsigned released = 0, retained = 0, i, j;
  execute(state, "anchor={}; weak=setmetatable({}, {__mode='v'}); "
                 "finalized=0; mt={__gc=function() finalized=finalized+1 end}");
  string(state, &retained);
  lua_setglobal(state, "kept");
  for (i = 0; i < 18; i++) {
    lua_gc(state, i % 3 == 0 ? LUA_GCINC : LUA_GCGEN);
    execute(state, "local n=#anchor+1; anchor[n]={n, function() return n end}; "
                   "weak[1]=setmetatable({}, mt)");
    string(state, &released);
    lua_pop(state, 1);
    lua_gc(state, LUA_GCCOLLECT);
    require(released == i + 1 && retained == 0);
    execute(state, "assert(weak[1]==nil and finalized==#anchor and #kept==64); "
                   "for i=1,#anchor do assert(anchor[i][1]==i and "
                   "anchor[i][2]()==i) end");
    if (i % 3 != 0) {
      for (j = 0; j < 8; j++) {
        execute(state, "anchor[1].young={#anchor}");
        minor(state);
        execute(state, "assert(anchor[1].young[1]==#anchor)");
        require(retained == 0);
      }
    }
  }
  lua_close(state);
  require(retained == 1);
}

static void weak_age(lua_State *state) {
  GCObject *weak;
  lua_gc(state, LUA_GCGEN);
  lua_gc(state, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  execute(state, "weak=setmetatable({}, {__mode='v'}); hold={}");
  lua_gc(state, LUA_GCCOLLECT);
  minor(state);
  execute(state, "hold[1]={17}; weak[1]=hold[1]");
  lua_getglobal(state, "weak");
  weak = (GCObject *)lua_topointer(state, -1);
  lua_pop(state, 1);
  minor(state);
  minor(state);
  lua_lock(state);
  require(getage(weak) == G_OLD && !isgray(weak));
  lua_unlock(state);
  execute(state, "assert(weak[1][1]==17); hold=nil");
  lua_gc(state, LUA_GCCOLLECT);
  execute(state, "assert(weak[1]==nil)");
}

int main(int argc, char **argv) {
  lua_State *state = luaL_newstate();
  require(state != NULL && argc == 2);
  luaL_openlibs(state);
  lua_gc(state, LUA_GCINC);
  lua_gc(state, LUA_GCCOLLECT);
  lua_gc(state, LUA_GCSTOP);
  if (strcmp(argv[1], "live") == 0 || strcmp(argv[1], "live_generational") == 0)
    live(state, strcmp(argv[1], "live_generational") == 0);
  else if (strcmp(argv[1], "modes") == 0)
    modes(state);
  else if (strcmp(argv[1], "rotations") == 0) {
    rotations(state);
    return 0;
  } else {
    require(strcmp(argv[1], "weak_age") == 0);
    weak_age(state);
  }
  lua_close(state);
  return 0;
}
