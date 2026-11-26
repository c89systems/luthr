/*
** $Id: lcorolib.c $
** Coroutine Library
** See Copyright Notice in lua.h
*/

#define lcorolib_c
#define LUA_LIB

#include "lprefix.h"


#include <stdlib.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "lapi.h"
#include "lapiowned.h"
#include "lauxowned.h"
#include "ldo.h"
#include "llimits.h"
#include "lexec.h"
#include "lcontrol.h"
#include "lcoroscope.h"
#include "lroots.h"


static lua_State *getco (lua_State *L) {
  lua_State *co = lua_tothread(L, 1);
  luaL_argexpected(L, co, 1, "thread");
  return co;
}

/* These operations reserve another coroutine. Keep the C activation pinned,
** but release its state before entering another stack's ownership protocol. */
static int otherstate (lua_State *L, lua_CFunction function) {
  LuaWorldScope scope = luaW_suspendapi();
  int results = function(L);
  luaW_restoreapi(scope);
  return results;
}


static void finishresume (lua_State *L, lua_State *co, LuaApiRegion *region,
                          LuaWorldScope suspended) {
  if (region != NULL) {
    luaX_transferpair(co, L);
    luaX_boundcontext(L)->api_region = region;
  }
  else {
    luaX_unlockpair(L, co);
    luaW_restoreapi(suspended);
  }
}

/*
** Resumes a coroutine. Returns the number of results for non-error
** cases, -1 for errors, or -2 when another operation owns the target.
*/
static int auxresume (lua_State *L, lua_State *co, int narg,
                      LuaCoroutineScope *scope) {
  NativeStackContext *context = luaE_getnativestackcontext();
  LuaApiRegion *region = context->api_region;
  LuaWorldScope suspended = {0};
  int status, nres;
  l_uint32 calls;
  if (L == co) return -2;
  /* A local continuation can use the caller's slot. If its stack is being
  ** borrowed, release the caller and take the gates in canonical order. */
  if (region != NULL && region->state == L && L->stack_peer == NULL &&
      !luaW_ownsheap(L) && luaX_joinpair(L, co))
    context->api_region = NULL;
  else {
    suspended = luaW_suspendapi();
    region = NULL;
    luaX_lockpair(L, co);
  }
  if (!luaO_claim(L, co, scope)) {
    finishresume(L, co, region, suspended);
    return -2;
  }
  if (l_unlikely(!luaA_checkstack(co, narg))) {
    finishresume(L, co, region, suspended);
    lua_pushliteral(L, "too many arguments to resume");
    return -1;  /* error flag */
  }
  luaA_move(L, co, narg);
  calls = getCcalls(L);
  /* Same-carrier resumes can run with both stacks retained. Safepoints
  ** and opaque calls withdraw the suspended caller before leaving Lua. */
  if (region != NULL)
    co->resume_with_parent = 1;
  else
    luaX_transferpair(L, co);
  status = luaD_resume(co, calls, narg, &nres);
  if (co->resume_with_parent)
    co->resume_with_parent = 0;
  else if (!luaX_joinpair(co, L)) {
    luaX_unlockstate(co);
    luaX_lockpair(L, co);
  }
  if (l_likely(status == LUA_OK || status == LUA_YIELD)) {
    if (l_unlikely(!luaA_checkstack(L, nres + 1))) {
      LuaWorldScope depth;
      finishresume(L, co, region, suspended);
      depth = luaW_suspendapi();
      lua_pop(co, nres);  /* remove results anyway */
      luaW_restoreapi(depth);
      lua_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */
    }
    luaA_move(co, L, nres);
    finishresume(L, co, region, suspended);
    return nres;
  }
  else {
    luaA_move(co, L, 1);  /* move error message */
    finishresume(L, co, region, suspended);
    return -1;  /* error flag */
  }
}


int luaB_coresume (lua_State *L) {
  lua_State *co = getco(L);
  LuaCoroutineScope scope;
  int r = auxresume(L, co, lua_gettop(L) - 1, &scope);
  if (r == -2) {
    lua_pushboolean(L, 0);
    lua_pushliteral(L, "cannot resume non-suspended coroutine");
    return 2;
  }
  luaO_end(&scope);
  if (l_unlikely(r < 0)) {
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2;  /* return false + error message */
  }
  else {
    lua_pushboolean(L, 1);
    lua_insert(L, -(r + 1));
    return r + 1;  /* return true + 'resume' returns */
  }
}


static int wrapresume (lua_State *L, lua_State *co, int narg) {
  LuaCoroutineScope scope;
  int r = auxresume(L, co, narg, &scope);
  if (r == -2)
    return luaL_error(L, "cannot resume non-suspended coroutine");
  if (l_unlikely(r < 0)) {  /* error? */
    LuaWorldScope depth = luaW_suspendapi();
    int stat = lua_status(co);
    if (stat != LUA_OK && stat != LUA_YIELD) {  /* error in the coroutine? */
      stat = lua_closethread(co, L);  /* close its tbc variables */
      lua_assert(stat != LUA_OK);
      lua_xmove(co, L, 1);  /* move error message to the caller */
    }
    luaW_restoreapi(depth);
    if (stat != LUA_ERRMEM &&  /* not a memory error and ... */
        lua_type(L, -1) == LUA_TSTRING) {  /* ... error object is a string? */
      luaL_where(L, 1);  /* add extra info, if available */
      lua_insert(L, -2);
      lua_concat(L, 2);
    }
    luaO_end(&scope);
    return lua_error(L);  /* propagate error */
  }
  luaO_end(&scope);
  return r;
}


int luaB_auxwrap (lua_State *L) {
  LuaNativeRoot target;
  const TValue *value;
  lua_State *co;
  int narg, nres;
  lua_lock(L);
  value = luaA_indexvalue(L, lua_upvalueindex(1), &target);
  if (l_unlikely(!ttisthread(value))) {
    /* Diagnose a capture changed with the debug library. */
    setobj2s(L, L->top.p, value);
    api_incr_top(L);
    luaR_poproot(L, &target);
    lua_unlock(L);
    lua_insert(L, 1);
    return luaL_typeerror(L, 1, "thread");
  }
  co = thvalue(value);
  narg = cast_int(L->top.p - (L->ci->func.p + 1));
  lua_unlock(L);
  nres = wrapresume(L, co, narg);
  return luaL_rootresult(L, &target, 1, nres);
}


int luaB_cocreate (lua_State *L) {
  StkId function;
  lua_lock(L);
  function = L->ci->func.p + 1;
  if (l_unlikely(function >= L->top.p || !ttisfunction(s2v(function)))) {
    lua_unlock(L);
    return luaL_typeerror(L, 1, "function");
  }
  luaE_newthread(L, 0, savestack(L, function));
  lua_unlock(L);
  return 1;
}


int luaB_cowrap (lua_State *L) {
  luaB_cocreate(L);
  luaQ_pushmanaged(L, luaB_auxwrap, 1);
  return 1;
}


int luaB_yield (lua_State *L) {
  return lua_yield(L, lua_gettop(L));
}


#define COS_RUN LUA_CO_RUNNING
#define COS_DEAD LUA_CO_DEAD
#define COS_YIELD LUA_CO_SUSPENDED
#define COS_NORM LUA_CO_NORMAL


static const char *const statname[] =
  {"running", "dead", "suspended", "normal"};


static int auxstatus (lua_State *L, lua_State *co) {
  return luaO_status(L, co);
}


static int costatus (lua_State *L) {
  lua_State *co = getco(L);
  lua_pushstring(L, statname[auxstatus(L, co)]);
  return 1;
}


static lua_State *getoptco (lua_State *L) {
  return (lua_isnone(L, 1) ? L : getco(L));
}


static int luaB_costatus (lua_State *L) {
  return otherstate(L, costatus);
}


static int coyieldable (lua_State *L) {
  lua_State *co = getoptco(L);
  lua_pushboolean(L, lua_isyieldable(co));
  return 1;
}


static int luaB_corunning (lua_State *L) {
  int ismain = lua_pushthread(L);
  lua_pushboolean(L, ismain);
  return 2;
}


static int luaB_yieldable (lua_State *L) {
  return otherstate(L, coyieldable);
}


static int closeco (lua_State *L) {
  lua_State *co = getoptco(L);
  LuaCoroutineScope scope;
  int status;
  if (co != L) {
    if (!luaO_begin(L, co, &scope))
      return luaL_error(L, "cannot close a normal coroutine");
    status = lua_closethread(co, L);
    if (status != LUA_OK) {
      lua_pushboolean(L, 0);
      lua_xmove(co, L, 1);
    }
    luaO_end(&scope);
    if (status == LUA_OK) {
      lua_pushboolean(L, 1);
      return 1;
    }
    return 2;
  }
  lua_geti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
  if (lua_tothread(L, -1) == co)
    return luaL_error(L, "cannot close main thread");
  status = lua_closethread(co, L);  /* ordinarily does not return */
  if (status != LUA_OK) return lua_error(L);
  lua_assert(0);
  return 0;
}


int luaB_close (lua_State *L) {
  return otherstate(L, closeco);
}


static const luaL_Reg co_funcs[] = {
  {"create", luaB_cocreate},
  {"resume", luaB_coresume},
  {"running", luaB_corunning},
  {"status", luaB_costatus},
  {"wrap", luaB_cowrap},
  {"yield", luaB_yield},
  {"isyieldable", luaB_yieldable},
  {"close", luaB_close},
  {NULL, NULL}
};



LUAMOD_API int luaopen_coroutine (lua_State *L) {
  const luaL_Reg *entry;
  luaL_checkversion(L);
  luaL_newlibtable(L, co_funcs);
  for (entry = co_funcs; entry->name != NULL; ++entry) {
    luaQ_pushmanaged(L, entry->func, 0);
    lua_setfield(L, -2, entry->name);
  }
  return 1;
}

