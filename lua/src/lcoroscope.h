/* Coroutine operation ownership. See Copyright Notice in lua.h */
#ifndef lcoroscope_h
#define lcoroscope_h

#include "lstate.h"

typedef struct LuaCoroutineScope {
  struct LuaCoroutineScope *previous;
  struct LuaCoroutineScope *next;
  struct LuaCoroutineScope **link;
  NativeStackContext *context;
  lua_State *source;
  lua_State *target;
  int held;
} LuaCoroutineScope;

enum { LUA_CO_RUNNING, LUA_CO_DEAD, LUA_CO_SUSPENDED, LUA_CO_NORMAL };
/* Called outside state gates by a C activation. Scope storage stays on its
** retained stack until end or protected unwinding; no physical mutex is held
** across user code. A failed claim does not change either Lua stack. */
LUAI_FUNC int luaO_begin (lua_State *L, lua_State *co, LuaCoroutineScope *scope);
LUAI_FUNC void luaO_end (LuaCoroutineScope *scope);
LUAI_FUNC void luaO_release (LuaCoroutineScope *boundary);
LUAI_FUNC void luaO_unwind (LuaCoroutineScope *boundary);
LUAI_FUNC int luaO_status (lua_State *L, lua_State *co);

#endif
