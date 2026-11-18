#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
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

enum { OBJECTS = 768, CAPACITY = 8192 };
static GCObject *allocations[CAPACITY];
static GCObject *observed[CAPACITY];
static unsigned allocation_count, finalized, expected;
static int resurrected;
static unsigned registration_stops;
static int registering;

void __real_luaW_stop(lua_State *);
void __wrap_luaW_stop(lua_State *state) {
  if (registering)
    registration_stops++;
  __real_luaW_stop(state);
}

static int finalize(lua_State *state) {
  unsigned id;
  if (lua_istable(state, 1)) {
    lua_rawgeti(state, 1, 1);
    id = (unsigned)lua_tointeger(state, -1);
  } else
    id = *(unsigned *)lua_touserdata(state, 1);
  require(id == expected--);
  finalized++;
  lua_rawgeti(state, LUA_REGISTRYINDEX, resurrected);
  lua_pushvalue(state, 1);
  lua_rawseti(state, -2, id);
  return 0;
}

static int address_order(const void *a, const void *b) {
  uintptr_t first = (uintptr_t)*(GCObject *const *)a;
  uintptr_t second = (uintptr_t)*(GCObject *const *)b;
  return (first > second) - (first < second);
}

static unsigned capture(GCObject **out, unsigned count, GCObject *object,
                        GCObject *limit) {
  for (; object != limit; object = luaC_allocnext(object)) {
    require(object != NULL && count < CAPACITY);
    out[count++] = object;
  }
  return count;
}

/* Registration and callback dispatch preserve the allocation population.
 * Disjoint sweep batches can reorder it. Include the sweep's detached cohort
 * as well as newly published allocations and its unswept old-generation tail.
 */
static void membership(lua_State *state, int snapshot) {
  GCObject **out = snapshot ? allocations : observed;
  unsigned index, i;
  lua_lock(state);
  luaW_stop(state);
  luaC_publish(state);
  index = capture(out, 0, G(state)->allgc, NULL);
  require(index != 0 && out[index - 1] == obj2gco(state));
  if (luaC_sweeping(state)) {
    LuaSweepPlan *plan = &G(state)->sweep_plan;
    require(plan->busy == 0);
    index = capture(out, index, plan->next, plan->limit);
    index = capture(out, index, plan->live[0], NULL);
    index = capture(out, index, plan->live[1], NULL);
    index = capture(out, index, plan->retry[0], NULL);
    index = capture(out, index, plan->retry[1], NULL);
    index = capture(out, index, plan->threads, NULL);
  }
  qsort(out, index, sizeof(*out), address_order);
  for (i = 1; i < index; i++)
    require(out[i] != out[i - 1]);
  if (snapshot)
    allocation_count = index;
  else {
    require(index == allocation_count);
    for (i = 0; i < index; i++)
      require(allocations[i] == out[i]);
  }
  luaW_start(state);
  lua_unlock(state);
}

int main(int argc, char **argv) {
  lua_State *state = luaL_newstate();
  unsigned i;
  int generational, pending, sweeping, old;
  require(argc == 2 && state != NULL);
  generational = strstr(argv[1], "generational") != NULL;
  pending = strstr(argv[1], "pending") != NULL;
  sweeping = strstr(argv[1], "sweeping") != NULL;
  old = strstr(argv[1], "old") != NULL;
  lua_gc(state, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(state, LUA_GCCOLLECT);
  lua_gc(state, LUA_GCSTOP);
  require(lua_checkstack(state, 16));
  lua_createtable(state, OBJECTS, 0); /* strong roots */
  lua_newtable(state);                /* finalization metatable */
  lua_pushcfunction(state, finalize);
  lua_setfield(state, 2, "__gc");
  lua_createtable(state, OBJECTS, 0);
  resurrected = luaL_ref(state, LUA_REGISTRYINDEX);
  for (i = 1; i <= OBJECTS; i++) {
    if (i % 3 == 0) {
      lua_createtable(state, 1, 0);
      lua_pushinteger(state, i);
      lua_rawseti(state, -2, 1);
    } else {
      unsigned *payload =
          lua_newuserdatauv(state, i % 2 ? 32 : 1024, i % 3 == 1 ? 0 : 2);
      *payload = i;
      if (i % 3 == 2) {
        lua_newthread(state);
        require(lua_setiuservalue(state, -2, 1));
        lua_pushfstring(state, "membership string %d", (int)i);
        require(lua_setiuservalue(state, -2, 2));
      }
    }
    lua_rawseti(state, 1, i);
  }
  if (old)
    lua_gc(state, LUA_GCCOLLECT);
  if (sweeping) {
    lua_lock(state);
    luaW_stop(state);
    luaC_runtilstate(state, GCSswpallgc, 0);
    luaW_start(state);
    luaC_waittrace(state);
    require(G(state)->gcstate == GCSswpallgc);
    lua_unlock(state);
  }
  membership(state, 1);
  registering = 1;
  for (i = 1; i <= OBJECTS; i++) {
    lua_rawgeti(state, 1, i);
    lua_pushvalue(state, 2);
    require(lua_setmetatable(state, -2));
    /* Repeated registration before publication must neither duplicate the
     * callback nor create a cycle in a coroutine's private index. */
    lua_pushvalue(state, 2);
    require(lua_setmetatable(state, -2));
    lua_pop(state, 1);
  }
  registering = 0;
  require(registration_stops == 0);
  membership(state, 0);
  /* A live collection must age each allocation once even when the same
   * object also belongs to the finalization index. */
  lua_gc(state, LUA_GCCOLLECT);
  require(finalized == 0);
  for (i = 1; i <= OBJECTS; i++) {
    lua_rawgeti(state, 1, i);
    require(lua_getmetatable(state, -1) && lua_rawequal(state, -1, 2));
    lua_pop(state, 2);
  }
  lua_remove(state, 1); /* discard all strong roots */
  expected = OBJECTS;
  if (pending) {
    lua_lock(state);
    luaC_fullgc(state, 1);
    require(finalized == 0 && G(state)->tobefnz != NULL);
    lua_unlock(state);
    membership(state, 1);
  }
  lua_gc(state, LUA_GCCOLLECT);
  require(finalized == OBJECTS && expected == 0);
  if (pending)
    membership(state, 0);
  luaL_unref(state, LUA_REGISTRYINDEX, resurrected);
  lua_gc(state, LUA_GCCOLLECT);
  require(finalized == OBJECTS);
  lua_close(state);
  return 0;
}
