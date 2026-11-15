/* Private task execution transitions. See Copyright Notice in lua.h */
#ifndef lexec_h
#define lexec_h

#include "lua.h"
#include "llimits.h"
#include <pthread.h>

struct lua_longjmp;
struct LuaWaiter;
typedef struct LuaExecution {
  void *context;
  void (*managed_enter)(struct LuaExecution *, lua_State *);
  /* Release only: this may run under a resource bookkeeping mutex. */
  void (*managed_leave)(struct LuaExecution *);
  void (*native_enter)(struct LuaExecution *, lua_State *);
  void (*native_leave)(struct LuaExecution *, lua_State *);
  void (*native_restore)(struct LuaExecution *, lua_State *, unsigned);
  void (*poll)(struct LuaExecution *, lua_State *);
  /* wait releases the supplied mutex and returns unlocked after notification
  ** removes its registration. wake only arranges resumption; it cannot enter
  ** Lua or switch the calling stack. */
  void (*wait)(struct LuaExecution *, pthread_mutex_t *, struct LuaWaiter *);
  void (*wake)(struct LuaExecution *, struct LuaWaiter *);
  struct lua_longjmp *protected;
  unsigned managed_depth;
  unsigned native_depth;
} LuaExecution;

typedef struct LuaOwner {
  LuaExecution *execution;
  pthread_t carrier;
} LuaOwner;

typedef struct LuaWaiter {
  struct LuaWaiter *next;
  LuaExecution *execution;
} LuaWaiter;

LUAI_FUNC LuaExecution *luaQ_current (void);
/* Trusted runtime code only: body and continuations cannot retain foreign TLS
** or perform opaque blocking work. Native functions remain foreign by default. */
LUAI_FUNC void luaQ_pushmanaged (lua_State *L, lua_CFunction function, int n);
LUAI_FUNC void luaQ_entermanaged (lua_State *L);
LUAI_FUNC void luaQ_leavemanaged (void);
LUAI_FUNC void luaQ_beginnative (lua_State *L);
LUAI_FUNC void luaQ_endnative (lua_State *L);
LUAI_FUNC void luaQ_restorenative (LuaExecution *execution, lua_State *L,
                                  unsigned depth);
LUAI_FUNC void luaQ_poll (lua_State *L);
LUAI_FUNC void luaQ_setmanagedhook (lua_State *L, lua_Hook hook);
LUAI_FUNC int luaQ_ismanagedhook (lua_State *L, lua_Hook hook);
LUAI_FUNC void luaQ_setowner (LuaOwner *owner);
LUAI_FUNC int luaQ_isowner (const LuaOwner *owner);
LUAI_FUNC void luaQ_wait (lua_State *L, pthread_mutex_t *mutex,
                          pthread_cond_t *changed, LuaWaiter **waiters);
LUAI_FUNC void luaQ_wake (pthread_cond_t *changed, LuaWaiter **waiters);

#endif
