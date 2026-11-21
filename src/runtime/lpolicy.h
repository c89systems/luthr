/* Retained invocation controls, independent of a VM's mutable defaults. */
#ifndef lpolicy_h
#define lpolicy_h

#include "lua.h"
#include "llimits.h"

struct global_State;
struct LuaExecution;
typedef struct LuaPolicy {
  struct global_State *global;
  unsigned references;
  int active;  /* atomic; inactive inherited records never charge new work */
  int exhausted;  /* atomic terminal instruction-budget outcome */
  int hookmask;  /* atomic; requested events while active */
  void *owner;
  lua_Hook hook;
  void (*release)(lua_State *, struct LuaPolicy *);
} LuaPolicy;

typedef struct LuaPolicyScope {
  struct LuaExecution *execution;
  LuaPolicy *previous;
} LuaPolicyScope;

LUAI_FUNC void luaCP_retain (LuaPolicy *);
LUAI_FUNC void luaCP_release (lua_State *, LuaPolicy *);
LUAI_FUNC void luaCP_bind (lua_State *, LuaPolicy *);
/* State ownership/admission already exists. A compatible explicit execution or
** host scope wins; otherwise a coroutine retains its inherited invocation. */
LUAI_FUNC void luaCP_apply (lua_State *, LuaPolicy *);
LUAI_FUNC void luaCP_begin (LuaPolicy *, LuaPolicyScope *);
LUAI_FUNC void luaCP_end (LuaPolicyScope *);
LUAI_FUNC LuaPolicy *luaCP_current (void);
LUAI_FUNC int luaCP_exhausted (lua_State *);
LUAI_FUNC l_noret luaCP_throwlimit (lua_State *);
LUAI_FUNC void luaCP_checklimit (lua_State *);

#endif
