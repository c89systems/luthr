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
