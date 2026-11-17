/*
** Scoped access to shared Lua objects.
** See Copyright Notice in lua.h
*/
#ifndef laccess_h
#define laccess_h

#include "lstate.h"

typedef struct LuaObjectScope {
  struct LuaObjectScope *previous;
  LuaObjectLock *lock;
  LuaNativeRoot root;
  ptrdiff_t saved_top;
  int held;
} LuaObjectScope;

LUAI_FUNC void luaA_begin (lua_State *L, GCObject *object, LuaObjectScope *scope);
/* Opaque native bytes may stay borrowed while execution is detached. Managed
** user values and metatables retain ordinary object ownership. */
LUAI_FUNC void luaA_beginpayload (lua_State *L, Udata *object,
                                 LuaObjectScope *scope);
/* A scanner must defer busy objects; it cannot wait on a mutator which may
** itself be awaiting collection. Success is released with luaA_endscan. */
LUAI_FUNC int luaA_tryscan (GCObject *object);
LUAI_FUNC void luaA_endscan (lua_State *L, GCObject *object);
/* A VM-owned partition has permanent storage and needs no collectable root. */
LUAI_FUNC void luaA_beginlock (lua_State *L, LuaObjectLock *lock,
                              LuaObjectScope *scope);
LUAI_FUNC void luaA_end (lua_State *L, LuaObjectScope *scope);
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
LUAI_FUNC int luaA_assign (lua_State *L, Table *table, const TValue *key,
                          TValue *value, LuaNativeRoot *method);

#endif
