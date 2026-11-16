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
#include "llimits.h"
#include "lexec.h"
#include "lcontrol.h"
#include "lcoroscope.h"


static lua_State *getco (lua_State *L) {
  lua_State *co = lua_tothread(L, 1);
  luaL_argexpected(L, co, 1, "thread");
  return co;
}


/*
** Resumes a coroutine. Returns the number of results for non-error
** cases or -1 for errors.
*/
static int auxresume (lua_State *L, lua_State *co, int narg) {
  int status, nres;
  if (l_unlikely(!lua_checkstack(co, narg))) {
    lua_pushliteral(L, "too many arguments to resume");
    return -1;  /* error flag */
  }
  lua_xmove(L, co, narg);
  status = lua_resume(co, L, narg, &nres);
  if (l_likely(status == LUA_OK || status == LUA_YIELD)) {
    if (l_unlikely(!lua_checkstack(L, nres + 1))) {
      lua_pop(co, nres);  /* remove results anyway */
      lua_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */
    }
    lua_xmove(co, L, nres);  /* move yielded values */
    return nres;
  }
  else {
    lua_xmove(co, L, 1);  /* move error message */
    return -1;  /* error flag */
  }
}


int luaB_coresume (lua_State *L) {
  lua_State *co = getco(L);
  LuaCoroutineScope scope;
  int r;
  if (!luaO_begin(L, co, &scope)) {
    lua_pushboolean(L, 0);
    lua_pushliteral(L, "cannot resume non-suspended coroutine");
    return 2;
  }
  r = auxresume(L, co, lua_gettop(L) - 1);
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


int luaB_wrapresume (lua_State *L, int narg) {
  lua_State *co = getco(L);  /* stack argument retains the snapshot */
  LuaCoroutineScope scope;
  int r;
  if (!luaO_begin(L, co, &scope))
    return luaL_error(L, "cannot resume non-suspended coroutine");
  r = auxresume(L, co, narg);
  if (l_unlikely(r < 0)) {  /* error? */
    int stat = lua_status(co);
    if (stat != LUA_OK && stat != LUA_YIELD) {  /* error in the coroutine? */
      stat = lua_closethread(co, L);  /* close its tbc variables */
      lua_assert(stat != LUA_OK);
      lua_xmove(co, L, 1);  /* move error message to the caller */
    }
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
  int narg = lua_gettop(L);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);  /* stable root below arguments/results */
  return luaB_wrapresume(L, narg);
}


int luaB_cocreate (lua_State *L) {
  lua_State *NL;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  NL = lua_newthread(L);
  lua_pushvalue(L, 1);  /* move function to top */
  lua_xmove(L, NL, 1);  /* move function from L to NL */
  return 1;
}


int luaB_cowrap (lua_State *L) {
  luaB_cocreate(L);
  luaQ_pushmanaged(L, luaB_auxwrap, 1);
  return 1;
}


static int luaB_yield (lua_State *L) {
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


static int luaB_costatus (lua_State *L) {
  lua_State *co = getco(L);
  lua_pushstring(L, statname[auxstatus(L, co)]);
  return 1;
}


static lua_State *getoptco (lua_State *L) {
  return (lua_isnone(L, 1) ? L : getco(L));
}


static int luaB_yieldable (lua_State *L) {
  lua_State *co = getoptco(L);
  lua_pushboolean(L, lua_isyieldable(co));
  return 1;
}


static int luaB_corunning (lua_State *L) {
  int ismain = lua_pushthread(L);
  lua_pushboolean(L, ismain);
  return 2;
}


int luaB_close (lua_State *L) {
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

