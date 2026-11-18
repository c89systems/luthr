/* Backing-allocator admission, independent of collector metadata. */
#ifndef lallocator_h
#define lallocator_h

#include "lua.h"
#include "llimits.h"
#include "lexec.h"

typedef struct LuaAllocator {
  lua_Alloc function;
  void *context;
  void *owner;  /* atomic logical owner; callback calls remain serialized */
  unsigned depth;
  unsigned waiting;  /* atomic observer handshake with changed */
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  struct LuaWaiter *waiters;
} LuaAllocator;

LUAI_FUNC int luaAL_init (LuaAllocator *, lua_Alloc, void *);
LUAI_FUNC void luaAL_destroy (LuaAllocator *);
/* Entry can park and change the caller's allocation-cache lease. No Lua work
** or safepoint may run while held; failed requests release before collection. */
LUAI_FUNC void luaAL_enter (lua_State *L);
LUAI_FUNC void luaAL_leave (lua_State *L);
LUAI_FUNC void *luaAL_realloc (lua_State *, void *, size_t, size_t);

#endif
