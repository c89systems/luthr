/* Validated, read-only mutable object marking. See lua.h for license. */
#define LUA_CORE
#include "lprefix.h"
#include "laccess.h"
#include "lgc.h"


int luaC_scanvalues (lua_State *L, LuaTraceWork *work, GCObject *object) {
  unsigned first = 0, size;
  uint64_t *version = luaA_sequence(object);
  LuaObjectLock *access = NULL;
  if (object->tt == LUA_VLCL) {
    size = gco2lcl(object)->nupvalues;
    access = &gco2lcl(object)->access;
  }
  else if (object->tt == LUA_VCCL) {
    size = gco2ccl(object)->nupvalues;
    access = &gco2ccl(object)->access;
  }
  else if (object->tt == LUA_VUSERDATA) {
    size = gco2u(object)->nuvalue;
    access = &gco2u(object)->access;
  }
  else size = 1;
  do {
    GCObject *edges[33];
    unsigned slots = size - first, count;
    uint64_t sequence = version != NULL
      ? __atomic_load_n(version, __ATOMIC_ACQUIRE) : 0;
    if (sequence & 1) {
      if (access != NULL && luaA_lockowner(access) == luaX_token(L)) return -1;
      return 0;
    }
    if (slots > 32) slots = 32;
    count = luaC_objectchunk(object, first, slots, edges);
    /* A pointer-only payload can be scanned while its vector changes: each
    ** loaded pointer is valid and the hybrid barriers retain both old and new
    ** referents. Split TValue bits still require sequence validation. */
    if (version != NULL &&
        __atomic_load_n(version, __ATOMIC_ACQUIRE) != sequence) return 0;
    luaC_traceedges(work, edges, count);
    first += slots;
    if (luaW_stopped(&G(L)->world)) return 0;
  } while (first < size);
  work->marked += luaC_objsize(object);
  luaC_finishstrong(work, object);
  return 1;
}
