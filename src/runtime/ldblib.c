/*
** $Id: ldblib.c $
** Interface from Lua to its debug API
** See Copyright Notice in lua.h
*/

#define ldblib_c
#define LUA_LIB

#include "lprefix.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lapi.h"
#include "lapiowned.h"
#include "lbuiltin.h"
#include "ldebug.h"
#include "ldo.h"
#include "lualib.h"
#include "llimits.h"
#include "lroots.h"
#include "lstring.h"
#include "ltable.h"


/*
** The hook table at registry[HOOKKEY] maps threads to their current
** hook function.
*/
static const char *const HOOKKEY = "_HOOKKEY";


/*
** If L1 != L, L1 can be in any state, and therefore there are no
** guarantees about its stack space; any push in L1 must be
** checked.
*/
static void checkstack (lua_State *L, lua_State *L1, int n) {
  if (l_unlikely(L != L1 && !lua_checkstack(L1, n)))
    luaL_error(L, "stack overflow");
}


static int db_getregistry (lua_State *L) {
  lua_pushvalue(L, LUA_REGISTRYINDEX);
  return 1;
}


static int db_getmetatable (lua_State *L) {
  luaL_checkany(L, 1);
  if (!lua_getmetatable(L, 1)) {
    lua_pushnil(L);  /* no metatable */
  }
  return 1;
}


static int db_setmetatable (lua_State *L) {
  int t = lua_type(L, 2);
  luaL_argexpected(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");
  lua_settop(L, 2);
  lua_setmetatable(L, 1);
  return 1;  /* return 1st argument */
}


static int db_getuservalue (lua_State *L) {
  int n = (int)luaL_optinteger(L, 2, 1);
  if (lua_type(L, 1) != LUA_TUSERDATA)
    luaL_pushfail(L);
  else if (lua_getiuservalue(L, 1, n) != LUA_TNONE) {
    lua_pushboolean(L, 1);
    return 2;
  }
  return 1;
}


static int db_setuservalue (lua_State *L) {
  int n = (int)luaL_optinteger(L, 3, 1);
  luaL_checktype(L, 1, LUA_TUSERDATA);
  luaL_checkany(L, 2);
  lua_settop(L, 2);
  if (!lua_setiuservalue(L, 1, n))
    luaL_pushfail(L);
  return 1;
}


/*
** Auxiliary function used by several library functions: check for
** an optional thread as function's first argument and set 'arg' with
** 1 if this argument is present (so that functions can skip it to
** access their other arguments)
*/
static lua_State *getthread (lua_State *L, int *arg) {
  if (lua_isthread(L, 1)) {
    *arg = 1;
    return lua_tothread(L, 1);
  }
  else {
    *arg = 0;
    return L;  /* function will operate over current thread */
  }
}


/* The new table and any function or line-table results stay on the owned
** stack while the record is filled. Interning a field name can allocate. */
static void setinfostring (lua_State *L, Table *t, const char *name,
                           const char *bytes, size_t len) {
  LuaNativeRoot value;
  TValue key;
  if (bytes == NULL) return;
  luaR_pushroot(L, &value);
  setsvalue(L, &value.value, luaS_newlstr(L, bytes, len));
  setsvalue(L, &key, luaS_new(L, name));
  luaH_set(L, t, &key, &value.value);
  luaR_poproot(L, &value);
}

static void setinfointeger (lua_State *L, Table *t, const char *name, int n) {
  TValue key, value;
  setsvalue(L, &key, luaS_new(L, name));
  setivalue(&value, n);
  luaH_set(L, t, &key, &value);
}

static void setinfoboolean (lua_State *L, Table *t, const char *name, int b) {
  TValue key, value;
  setsvalue(L, &key, luaS_new(L, name));
  if (b) setbtvalue(&value);
  else setbfvalue(&value);
  luaH_set(L, t, &key, &value);
}

static void setinforesult (lua_State *L, Table *t, const char *name,
                           int offset) {
  TValue key;
  setsvalue(L, &key, luaS_new(L, name));
  luaH_set(L, t, &key, s2v(L->top.p - offset));
}

static int inforesults (unsigned fields) {
  return ((fields & INFO_LINES) != 0) + ((fields & INFO_FUNCTION) != 0);
}

static void pushinfotable (lua_State *L, unsigned fields, const lua_Debug *ar) {
  int lines = (fields & INFO_LINES) != 0;
  int extra = inforesults(fields);
  unsigned capacity = cast_uint(extra) +
    5u * ((fields & INFO_SOURCE) != 0) + ((fields & INFO_LINE) != 0) +
    3u * ((fields & INFO_UPVALUES) != 0) + 2u * ((fields & INFO_NAME) != 0) +
    2u * ((fields & INFO_TRANSFER) != 0) + 2u * ((fields & INFO_TAIL) != 0);
  Table *t;
  t = luaH_new(L, 0, capacity);
  sethvalue2s(L, L->top.p, t);
  api_incr_top(L);
  if (fields & INFO_SOURCE) {
    setinfostring(L, t, "source", ar->source, ar->srclen);
    setinfostring(L, t, "short_src", ar->short_src, strlen(ar->short_src));
    setinfointeger(L, t, "linedefined", ar->linedefined);
    setinfointeger(L, t, "lastlinedefined", ar->lastlinedefined);
    setinfostring(L, t, "what", ar->what, strlen(ar->what));
  }
  if (fields & INFO_LINE)
    setinfointeger(L, t, "currentline", ar->currentline);
  if (fields & INFO_UPVALUES) {
    setinfointeger(L, t, "nups", ar->nups);
    setinfointeger(L, t, "nparams", ar->nparams);
    setinfoboolean(L, t, "isvararg", ar->isvararg);
  }
  if (fields & INFO_NAME) {
    if (ar->name != NULL)
      setinfostring(L, t, "name", ar->name, strlen(ar->name));
    setinfostring(L, t, "namewhat", ar->namewhat, strlen(ar->namewhat));
  }
  if (fields & INFO_TRANSFER) {
    setinfointeger(L, t, "ftransfer", ar->ftransfer);
    setinfointeger(L, t, "ntransfer", ar->ntransfer);
  }
  if (fields & INFO_TAIL) {
    setinfoboolean(L, t, "istailcall", ar->istailcall);
    setinfointeger(L, t, "extraargs", ar->extraargs);
  }
  /* The debug API returned the function (if any) before the line table. */
  if (lines) setinforesult(L, t, "activelines", 2);
  if (fields & INFO_FUNCTION) setinforesult(L, t, "func", 2 + lines);
  sethvalue2s(L, L->top.p - extra - 1, t);
  L->top.p -= extra;
  luaC_checkGC(L);
}


/*
** Inspect the selected frame and build the result while owning the
** stack. When inspecting another thread, use the API to move values
** between the stacks and acquire them separately.
*/
static int db_getinfo (lua_State *L) {
  lua_Debug ar;
  CallInfo *ci = NULL;
  const TValue *func = NULL;
  int arg, byfunction, level = 0;
  unsigned fields;
  lua_State *L1 = getthread(L, &arg);
  const char *options = luaL_optstring(L, arg+2, "flnSrtu");
  checkstack(L, L1, 3);
  luaL_argcheck(L, options[0] != '>', arg + 2, "invalid option '>'");
  byfunction = lua_isfunction(L, arg + 1);
  if (!byfunction)
    level = (int)luaL_checkinteger(L, arg + 1);
  else if (L != L1) {
    lua_pushvalue(L, arg + 1);
    lua_xmove(L, L1, 1);
  }
  fields = luaG_infofields(options);
  lua_lock(L1);
  if (byfunction) {
    if (L == L1)
      func = s2v(L->ci->func.p + arg + 1);
    else {
      L1->top.p--;
      func = s2v(L1->top.p);
    }
  }
  else {
    ci = luaG_getstack(L1, level);
    if (ci == NULL) {
      lua_unlock(L1);
      luaL_pushfail(L);
      return 1;
    }
  }
  luaG_info(L1, fields, &ar, ci, func);
  if (fields & INFO_INVALID) {
    lua_unlock(L1);
    return luaL_argerror(L, arg+2, "invalid option");
  }
  if (L != L1) {
    int extra = inforesults(fields);
    lua_unlock(L1);
    if (extra != 0)
      lua_xmove(L1, L, extra);
    lua_lock(L);
  }
  pushinfotable(L, fields, &ar);
  lua_unlock(L);
  return 1;
}


static int db_getlocal (lua_State *L) {
  int arg;
  lua_State *L1 = getthread(L, &arg);
  int nvar = (int)luaL_checkinteger(L, arg + 2);  /* local-variable index */
  if (lua_isfunction(L, arg + 1)) {  /* function argument? */
    lua_pushvalue(L, arg + 1);  /* push function */
    lua_pushstring(L, lua_getlocal(L, NULL, nvar));  /* push local name */
    return 1;  /* return only name (there is no value) */
  }
  else {  /* stack-level argument */
    lua_Debug ar;
    const char *name;
    int level = (int)luaL_checkinteger(L, arg + 1);
    if (l_unlikely(!lua_getstack(L1, level, &ar)))  /* out of range? */
      return luaL_argerror(L, arg+1, "level out of range");
    checkstack(L, L1, 1);
    name = lua_getlocal(L1, &ar, nvar);
    if (name) {
      lua_xmove(L1, L, 1);  /* move local value */
      lua_pushstring(L, name);  /* push name */
      lua_rotate(L, -2, 1);  /* re-order */
      return 2;
    }
    else {
      luaL_pushfail(L);  /* no name (nor value) */
      return 1;
    }
  }
}


static int db_setlocal (lua_State *L) {
  int arg;
  const char *name;
  lua_State *L1 = getthread(L, &arg);
  lua_Debug ar;
  int level = (int)luaL_checkinteger(L, arg + 1);
  int nvar = (int)luaL_checkinteger(L, arg + 2);
  if (l_unlikely(!lua_getstack(L1, level, &ar)))  /* out of range? */
    return luaL_argerror(L, arg+1, "level out of range");
  luaL_checkany(L, arg+3);
  lua_settop(L, arg+3);
  checkstack(L, L1, 1);
  lua_xmove(L, L1, 1);
  name = lua_setlocal(L1, &ar, nvar);
  if (name == NULL)
    lua_pop(L1, 1);  /* pop value (if not popped by 'lua_setlocal') */
  lua_pushstring(L, name);
  return 1;
}


/*
** get (if 'get' is true) or set an upvalue from a closure
*/
static int auxupvalue (lua_State *L, int get) {
  const char *name;
  int n = (int)luaL_checkinteger(L, 2);  /* upvalue index */
  luaL_checktype(L, 1, LUA_TFUNCTION);  /* closure */
  name = get ? lua_getupvalue(L, 1, n) : lua_setupvalue(L, 1, n);
  if (name == NULL) return 0;
  lua_pushstring(L, name);
  lua_insert(L, -(get+1));  /* no-op if get is false */
  return get + 1;
}


static int db_getupvalue (lua_State *L) {
  return auxupvalue(L, 1);
}


static int db_setupvalue (lua_State *L) {
  luaL_checkany(L, 3);
  return auxupvalue(L, 0);
}


/*
** Check whether a given upvalue from a given closure exists and
** returns its index
*/
static void *checkupval (lua_State *L, int argf, int argnup, int *pnup) {
  void *id;
  int nup = (int)luaL_checkinteger(L, argnup);  /* upvalue index */
  luaL_checktype(L, argf, LUA_TFUNCTION);  /* closure */
  id = lua_upvalueid(L, argf, nup);
  if (pnup) {
    luaL_argcheck(L, id != NULL, argnup, "invalid upvalue index");
    *pnup = nup;
  }
  return id;
}


static int db_upvalueid (lua_State *L) {
  void *id = checkupval(L, 1, 2, NULL);
  if (id != NULL)
    lua_pushlightuserdata(L, id);
  else
    luaL_pushfail(L);
  return 1;
}


static int db_upvaluejoin (lua_State *L) {
  int n1, n2;
  checkupval(L, 1, 2, &n1);
  checkupval(L, 3, 4, &n2);
  luaL_argcheck(L, !lua_iscfunction(L, 1), 1, "Lua function expected");
  luaL_argcheck(L, !lua_iscfunction(L, 3), 3, "Lua function expected");
  lua_upvaluejoin(L, 1, n1, 3, n2);
  return 0;
}


/*
** Call hook function registered at hook table for the current
** thread (if there is one)
*/
void luaB_debughook (lua_State *L, lua_Debug *ar) {
  static const char *const hooknames[] =
    {"call", "return", "line", "count", "tail call"};
  LuaNativeRoot registry;
  TValue key;
  const TValue *hooks;
  lu_byte tag;
  lua_lock(L);
  luaR_pushroot(L, &registry);
  luaA_getregistry(L, &registry);
  luaA_pushfield(L, &registry.value, HOOKKEY);
  luaR_poproot(L, &registry);
  hooks = s2v(L->top.p - 1);
  api_check(L, ttistable(hooks), "table expected");
  setthvalue(L, &key, L);
  tag = luaH_get(L, hvalue(hooks), &key, s2v(L->top.p));
  if (novariant(tag) == LUA_TFUNCTION) {
    TString *name;
    api_incr_top(L);
    name = luaS_new(L, hooknames[(int)ar->event]);
    setsvalue2s(L, L->top.p, name);
    api_incr_top(L);
    luaC_checkGC(L);
    if (ar->currentline >= 0) {
      setivalue(s2v(L->top.p), ar->currentline);
    }
    else setnilvalue2s(L->top.p);
    api_incr_top(L);
    luaD_callnoyield(L, L->top.p - 3, 0);
  }
  lua_unlock(L);
}


/*
** Convert a string mask (for 'sethook') into a bit mask
*/
static int makemask (const char *smask, int count) {
  int mask = 0;
  if (strchr(smask, 'c')) mask |= LUA_MASKCALL;
  if (strchr(smask, 'r')) mask |= LUA_MASKRET;
  if (strchr(smask, 'l')) mask |= LUA_MASKLINE;
  if (count > 0) mask |= LUA_MASKCOUNT;
  return mask;
}


/*
** Convert a bit mask (for 'gethook') into a string mask
*/
static char *unmakemask (int mask, char *smask) {
  int i = 0;
  if (mask & LUA_MASKCALL) smask[i++] = 'c';
  if (mask & LUA_MASKRET) smask[i++] = 'r';
  if (mask & LUA_MASKLINE) smask[i++] = 'l';
  smask[i] = '\0';
  return smask;
}


static int db_sethook (lua_State *L) {
  int arg, mask, count;
  lua_Hook func;
  lua_State *L1 = getthread(L, &arg);
  if (lua_isnoneornil(L, arg+1)) {  /* no hook? */
    lua_settop(L, arg+1);
    func = NULL; mask = 0; count = 0;  /* turn off hooks */
  }
  else {
    const char *smask = luaL_checkstring(L, arg+2);
    luaL_checktype(L, arg+1, LUA_TFUNCTION);
    count = (int)luaL_optinteger(L, arg + 3, 0);
    func = luaB_debughook; mask = makemask(smask, count);
  }
  if (!luaL_getsubtable(L, LUA_REGISTRYINDEX, HOOKKEY)) {
    /* table just created; initialize it */
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");  /** hooktable.__mode = "k" */
    lua_pushvalue(L, -1);
    lua_setmetatable(L, -2);  /* metatable(hooktable) = hooktable */
  }
  checkstack(L, L1, 1);
  lua_pushthread(L1); lua_xmove(L1, L, 1);  /* key (thread) */
  lua_pushvalue(L, arg + 1);  /* value (hook function) */
  lua_rawset(L, -3);  /* hooktable[L1] = new Lua hook */
  lua_sethook(L1, func, mask, count);
  return 0;
}


static int db_gethook (lua_State *L) {
  int arg;
  lua_State *L1 = getthread(L, &arg);
  char buff[5];
  int mask = lua_gethookmask(L1);
  lua_Hook hook = lua_gethook(L1);
  if (hook == NULL) {  /* no hook? */
    luaL_pushfail(L);
    return 1;
  }
  else if (hook != luaB_debughook)  /* external hook? */
    lua_pushliteral(L, "external hook");
  else {  /* hook table must exist */
    lua_getfield(L, LUA_REGISTRYINDEX, HOOKKEY);
    checkstack(L, L1, 1);
    lua_pushthread(L1); lua_xmove(L1, L, 1);
    lua_rawget(L, -2);   /* 1st result = hooktable[L1] */
    lua_remove(L, -2);  /* remove hook table */
  }
  lua_pushstring(L, unmakemask(mask, buff));  /* 2nd result = mask */
  lua_pushinteger(L, lua_gethookcount(L1));  /* 3rd result = count */
  return 3;
}


static int db_debug (lua_State *L) {
  for (;;) {
    char buffer[250];
    lua_writestringerror("%s", "lua_debug> ");
    if (fgets(buffer, sizeof(buffer), stdin) == NULL ||
        strcmp(buffer, "cont\n") == 0)
      return 0;
    if (luaL_loadbufferx(L, buffer, strlen(buffer), "=(debug command)", "t") ||
        lua_pcall(L, 0, 0, 0))
      lua_writestringerror("%s\n", luaL_tolstring(L, -1, NULL));
    lua_settop(L, 0);  /* remove eventual returns */
  }
}


static int db_traceback (lua_State *L) {
  int arg;
  lua_State *L1 = getthread(L, &arg);
  const char *msg = lua_tostring(L, arg + 1);
  if (msg == NULL && !lua_isnoneornil(L, arg + 1))  /* non-string 'msg'? */
    lua_pushvalue(L, arg + 1);  /* return it untouched */
  else {
    int level = (int)luaL_optinteger(L, arg + 2, (L == L1) ? 1 : 0);
    luaL_traceback(L, L1, msg, level);
  }
  return 1;
}


static const luaL_Reg dblib[] = {
  {"debug", db_debug},
  {"getuservalue", db_getuservalue},
  {"gethook", db_gethook},
  {"getinfo", db_getinfo},
  {"getlocal", db_getlocal},
  {"getregistry", db_getregistry},
  {"getmetatable", db_getmetatable},
  {"getupvalue", db_getupvalue},
  {"upvaluejoin", db_upvaluejoin},
  {"upvalueid", db_upvalueid},
  {"setuservalue", db_setuservalue},
  {"sethook", db_sethook},
  {"setlocal", db_setlocal},
  {"setmetatable", db_setmetatable},
  {"setupvalue", db_setupvalue},
  {"traceback", db_traceback},
  {NULL, NULL}
};


LUAMOD_API int luaopen_debug (lua_State *L) {
  luaL_newlib(L, dblib);
  return 1;
}

