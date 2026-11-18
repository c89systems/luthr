/* Versioned table storage and quiescent-state reclamation. See lua.h. */
#ifndef ltablestorage_h
#define ltablestorage_h

#include "llimits.h"
#include "lua.h"

typedef struct LuaTableRetirement {
  struct LuaTableRetirement *next;
  void *array;
  size_t array_bytes;
  void *nodes;
  size_t node_bytes;
  size_t bytes;
  uint64_t epoch;
} LuaTableRetirement;

/* A descriptor is reserved before fallible resize construction. Failed
** construction frees it; successful construction publishes it after the last
** use of the replaced buffers. New buffers carry no retirement overhead. */
LUAI_FUNC void luaHS_publish (lua_State *L, LuaTableRetirement *retirement);
/* Collector rendezvous, with world.mutex held and all mutators withdrawn. */
LUAI_FUNC void luaHS_publishstopped (lua_State *L);

/* No optimistic storage borrow survives these execution boundaries. */
LUAI_FUNC void luaHS_quiescent (lua_State *L);
enum { LUAHS_ASSIST, LUAHS_DRAIN, LUAHS_WITHDRAW };

/* Assist one batch, drain a finite snapshot, or publish small private batches
** before API withdrawal. Only producers
** wait under pressure, with managed capacity released. Readers never wait. */
LUAI_FUNC void luaHS_reclaim (lua_State *L, int mode);

#endif
