/* Concurrent collection pacing. See Copyright Notice in lua.h */
#ifndef lpacer_h
#define lpacer_h

#include "lua.h"
#include "llimits.h"

struct global_State;
typedef struct LuaGCPacer {
  l_mem goal;  /* soft target, independent of explicit step work quotas */
  l_mem trigger;
  l_mem hard;
  l_mem debt;  /* atomic remaining hard-limit allocation allowance */
  l_mem start;
  lu_mem expected;
  lu_mem work;  /* atomic shared credit: scans and sweeps already completed */
  lu_mem previous;
  unsigned nursery_scans;  /* atomic quota shared by all local collectors */
  unsigned parallelism;  /* observed simultaneous admissions, under quiescence */
} LuaGCPacer;

LUAI_FUNC void luaP_init (struct global_State *g);
LUAI_FUNC void luaP_idle (struct global_State *g);
LUAI_FUNC void luaP_begin (struct global_State *g);
/* Allocation allowance remains charged while phase requests seek admission. */
LUAI_FUNC void luaP_account (struct global_State *g, l_mem change);
/* Completed batches publish credit before releasing their participation. */
LUAI_FUNC void luaP_work (struct global_State *g, lu_mem work);
LUAI_FUNC lu_mem luaP_budget (lua_State *L, lu_mem requested);

#endif
