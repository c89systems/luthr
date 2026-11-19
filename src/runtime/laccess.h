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
    case LUA_VTABLE: case LUA_VLCL: case LUA_VCCL:
    case LUA_VUSERDATA: case LUA_VUPVAL:
      return &cast(union GCUnion *, object)->mutable_header.sharing;
    default: return NULL;
  }
}
l_sinline uint64_t *luaA_sequence (GCObject *object) {
  return &cast(union GCUnion *, object)->mutable_header.sequence;
}
l_sinline int luaA_localtable (lua_State *L, Table *table) {
  if (__atomic_load_n(&table->sharing, __ATOMIC_ACQUIRE) != cast(uintptr_t, L))
    return 0;
  if (!luaC_tracing(L)) return 1;
  if (__atomic_load_n(&table->weak_epoch, __ATOMIC_ACQUIRE) == G(L)->trace_epoch)
    return __atomic_load_n(&table->gcweak, __ATOMIC_ACQUIRE) == 0;
  return __atomic_load_n(&table->metatable, __ATOMIC_ACQUIRE) == NULL;
}
l_sinline int luaA_local (lua_State *L, GCObject *object) {
  if (object->tt == LUA_VTABLE) return luaA_localtable(L, gco2t(object));
  return __atomic_load_n(&cast(union GCUnion *, object)->mutable_header.sharing,
                          __ATOMIC_ACQUIRE) == cast(uintptr_t, L);
}
LUAI_FUNC void luaA_share (lua_State *L, GCObject *object);
LUAI_FUNC void luaA_sharevalue (lua_State *L, const TValue *value);
LUAI_FUNC void luaA_publish (lua_State *L, GCObject *owner,
                            const TValue *value);
/* Atomic components are validated by the owner's sequence before GC follows
** their pointer representations. The caller owns the mutation scope. */
#define luaA_store(L,to,from) setobj2t(L,to,from)

LUAI_FUNC void luaA_begin (lua_State *L, GCObject *object, LuaObjectScope *scope);
/* Opaque native bytes may stay borrowed while execution is detached. Managed
** user values and metatables retain ordinary object ownership. */
LUAI_FUNC void luaA_beginpayload (lua_State *L, Udata *object,
                                 LuaObjectScope *scope);
/* A scanner must defer busy objects; it cannot wait on a mutator which may
** itself be awaiting collection. Success is released with luaA_endscan. */
LUAI_FUNC int luaA_tryscan (lua_State *L, GCObject *object);
LUAI_FUNC void luaA_endscan (lua_State *L, GCObject *object);
/* A VM-owned partition has permanent storage and needs no collectable root. */
LUAI_FUNC void luaA_beginlock (lua_State *L, LuaObjectLock *lock,
                              LuaObjectScope *scope);
LUAI_FUNC int luaA_trylock (lua_State *L, LuaObjectLock *lock);
LUAI_FUNC void luaA_endlock (lua_State *L, LuaObjectLock *lock);
LUAI_FUNC void luaA_end (lua_State *L, LuaObjectScope *scope);
/* End access but retain scope->root; the caller later pops that native root. */
LUAI_FUNC void luaA_endrooted (lua_State *L, LuaObjectScope *scope);
LUAI_FUNC void luaA_unwind (lua_State *L, LuaObjectScope *target);
LUAI_FUNC void luaA_release (lua_State *L);
LUAI_FUNC void luaA_getmetatable (lua_State *L, const TValue *object,
                                 LuaNativeRoot *result);
LUAI_FUNC void luaA_setmetatable (lua_State *L, const TValue *object, Table *mt);
LUAI_FUNC void luaA_metafield (lua_State *L, const TValue *object, TString *key,
                              LuaNativeRoot *result, int event);
LUAI_FUNC void luaA_tabletm (lua_State *L, Table *mt, TMS event,
                            LuaNativeRoot *result);
/* Keys and assigned values must remain rooted across ownership waits. */
LUAI_FUNC lu_byte luaA_index (lua_State *L, Table *table, const TValue *key,
                             TValue *result, LuaNativeRoot *method);
/* The receiver, key and value are caller-rooted snapshots. */
LUAI_FUNC int luaA_assign (lua_State *L, const TValue *object, const TValue *key,
                          TValue *value, LuaNativeRoot *method);

#endif
