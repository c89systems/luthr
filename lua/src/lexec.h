/* Private task execution transitions. See Copyright Notice in lua.h */
#ifndef lexec_h
#define lexec_h

#include "lua.h"
#include "llimits.h"

struct lua_longjmp;
typedef struct LuaExecution {
  void *context;
  void (*managed_enter)(struct LuaExecution *, lua_State *);
  void (*managed_leave)(struct LuaExecution *);
  void (*native_enter)(struct LuaExecution *, lua_State *);
  void (*native_leave)(struct LuaExecution *, lua_State *);
  void (*native_restore)(struct LuaExecution *, lua_State *, unsigned);
  void (*poll)(struct LuaExecution *, lua_State *);
  struct lua_longjmp *protected;
  unsigned managed_depth;
  unsigned native_depth;
} LuaExecution;

LUAI_FUNC LuaExecution *luaQ_current (void);
LUAI_FUNC void luaQ_entermanaged (lua_State *L);
LUAI_FUNC void luaQ_leavemanaged (void);
LUAI_FUNC void luaQ_beginnative (lua_State *L);
LUAI_FUNC void luaQ_endnative (lua_State *L);
LUAI_FUNC void luaQ_restorenative (LuaExecution *execution, lua_State *L,
                                  unsigned depth);
LUAI_FUNC void luaQ_poll (lua_State *L);
LUAI_FUNC void luaQ_setmanagedhook (lua_State *L, lua_Hook hook);
LUAI_FUNC int luaQ_ismanagedhook (lua_State *L, lua_Hook hook);

#endif
