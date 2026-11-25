/*
** Scoped access to shared Lua objects.
** See Copyright Notice in lua.h
*/
#ifndef laccess_h
#define laccess_h

#include "lstate.h"
#include "lgc.h"

typedef struct LuaObjectScope {
  struct LuaObjectScope *previous;
  LuaObjectLock *lock;
  LuaNativeRoot root;
  ptrdiff_t saved_top;
  int held;
} LuaObjectScope;

/* A private graph belongs to a Lua state, independent of its current carrier.
** The shared bit is permanent. Its remaining bits temporarily link a graph
** that is being promoted, before any new cross-state reference is exposed. */
#define luaA_initsharing(L,o) ((o)->sharing = cast(uintptr_t, (L)))
l_sinline uintptr_t *luaA_sharing (GCObject *object) {
  switch (object->tt) {
    case LUA_VTABLE: return &gco2t(object)->sharing;
    case LUA_VLCL: return &gco2lcl(object)->sharing;
    case LUA_VCCL: return &gco2ccl(object)->sharing;
    case LUA_VUSERDATA: return &gco2u(object)->sharing;
    case LUA_VUPVAL: return &gco2upv(object)->sharing;
    default: return NULL;
  }
}
l_sinline uint64_t *luaA_sequence (GCObject *object) {
  switch (object->tt) {
    case LUA_VTABLE: return &gco2t(object)->sequence;
    case LUA_VUPVAL: return &gco2upv(object)->sequence;
    case LUA_VCCL: return &gco2ccl(object)->sequence;
    case LUA_VUSERDATA:
      return gco2u(object)->nuvalue ? &gco2u(object)->sequence : NULL;
    default: return NULL;  /* individually atomic pointer fields */
  }
}
l_sinline int luaA_localtable (lua_State *L, Table *table) {
  if (__atomic_load_n(&table->sharing, __ATOMIC_ACQUIRE) != cast(uintptr_t, L))
    return 0;
  return !luaC_tracing(L) || luaC_strongtable(G(L), table);
}
l_sinline int luaA_local (lua_State *L, GCObject *object) {
  if (object->tt == LUA_VTABLE) return luaA_localtable(L, gco2t(object));
  return __atomic_load_n(luaA_sharing(object), __ATOMIC_ACQUIRE) ==
         cast(uintptr_t, L);
}
LUAI_FUNC void luaA_share (lua_State *L, GCObject *object);
LUAI_FUNC void luaA_sharevalue (lua_State *L, const TValue *value);
/* Publish a small table without transitive promotion or a safepoint. */
LUAI_FUNC int luaA_tryshareleaf (lua_State *L, Table *table);
LUAI_FUNC void luaA_publish (lua_State *L, GCObject *owner,
                            const TValue *value);
/* Split values are validated by the owner's sequence before GC follows their
** pointer representations. The caller owns the mutation scope. */
#define luaA_store(L,to,from) setobj2t(L,to,from)

LUAI_FUNC void luaA_begin (lua_State *L, GCObject *object, LuaObjectScope *scope);
/* Opaque native bytes may stay borrowed while execution is detached. Managed
** user values and metatables retain ordinary object ownership. */
LUAI_FUNC void luaA_beginpayload (lua_State *L, Udata *object,
                                 LuaObjectScope *scope);
/* The caller registers interest and then rechecks its predicate under
** world.mutex. After a wake it must register again before waiting. */
l_sinline void luaA_watchlock (LuaObjectLock *lock) {
  __atomic_fetch_or(&lock->word, LUA_OBJECT_WAITING, __ATOMIC_SEQ_CST);
}
l_sinline int luaA_lockwatched (const LuaObjectLock *lock) {
  return (__atomic_load_n(&lock->word, __ATOMIC_SEQ_CST) &
          LUA_OBJECT_WAITING) != 0;
}
/* A scanner must defer busy objects; it cannot wait on a mutator which may
** itself be awaiting collection. Return the reservation to luaA_endscan. */
LUAI_FUNC int luaA_tryscan (lua_State *L, GCObject *object);
LUAI_FUNC void luaA_endscan (lua_State *L, GCObject *object, int reservation);
/* A VM-owned partition has permanent storage and needs no collectable root. */
LUAI_FUNC void luaA_beginlock (lua_State *L, LuaObjectLock *lock,
                              LuaObjectScope *scope);
LUAI_FUNC int luaA_trylock (lua_State *L, LuaObjectLock *lock);
LUAI_FUNC void luaA_endlock (lua_State *L, LuaObjectLock *lock, int reservation);
LUAI_FUNC void luaA_end (lua_State *L, LuaObjectScope *scope);
/* End access but retain scope->root; the caller later pops that native root. */
LUAI_FUNC void luaA_endrooted (lua_State *L, LuaObjectScope *scope);
LUAI_FUNC void luaA_unwind (lua_State *L, LuaObjectScope *target);
LUAI_FUNC void luaA_release (lua_State *L);
LUAI_FUNC void luaA_getmetatable (lua_State *L, const TValue *object,
                                 LuaNativeRoot *result);
LUAI_FUNC void luaA_setmetatable (lua_State *L, const TValue *object, Table *mt);
LUAI_FUNC void luaA_invalidatestringindex (lua_State *L, Table *mt);
/* Admission retains the published index during a nonparking lookup. */
l_sinline Table *luaA_stringindex (lua_State *L) {
  return __atomic_load_n(&G(L)->string_index, __ATOMIC_ACQUIRE);
}
LUAI_FUNC void luaA_metafield (lua_State *L, const TValue *object, TString *key,
                              LuaNativeRoot *result, int event);
/* Snapshot a primitive type's metafield without parking. The caller shades
** the result before use and roots it before any safepoint. */
LUAI_FUNC int luaA_readtypefield (lua_State *L, int type, TString *key,
                                  TValue *result);
/* Keys and assigned values must remain rooted across ownership waits. */
LUAI_FUNC lu_byte luaA_index (lua_State *L, Table *table, const TValue *key,
                             TValue *result, LuaNativeRoot *method);
/* The receiver, key and value are caller-rooted snapshots. */
LUAI_FUNC int luaA_assign (lua_State *L, const TValue *object, const TValue *key,
                          TValue *value, LuaNativeRoot *method);

#endif
