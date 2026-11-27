/*
** $Id: loadlib.c $
** Dynamic library loader for Lua
** See Copyright Notice in lua.h
**
** This module contains an implementation of loadlib for Unix systems
** that have dlfcn, an implementation for Windows, and a stub for other
** systems.
*/

#define loadlib_c
#define LUA_LIB

#include "lprefix.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "llimits.h"
#include "lapi.h"
#include "laccess.h"
#include "lauxowned.h"
#include "ldebug.h"
#include "ldo.h"
#include "lexec.h"
#include "lgc.h"
#include "lstring.h"
#include "ltable.h"
#include "ltableaccess.h"
#include "ltask.h"
#include "lvm.h"


/*
** LUA_CSUBSEP is the character that replaces dots in submodule names
** when searching for a C loader.
** LUA_LSUBSEP is the character that replaces dots in submodule names
** when searching for a Lua loader.
*/
#if !defined(LUA_CSUBSEP)
#define LUA_CSUBSEP		LUA_DIRSEP
#endif

#if !defined(LUA_LSUBSEP)
#define LUA_LSUBSEP		LUA_DIRSEP
#endif


/* prefix for open functions in C libraries */
#define LUA_POF		"luaopen_"

/* separator for open functions in C libraries */
#define LUA_OFSEP	"_"


/*
** key for table in the registry that keeps handles
** for all loaded C libraries
*/
static const char *const CLIBS = "_CLIBS";

#define LIB_FAIL	"open"


#define setprogdir(L)           ((void)0)


/* cast void* to a Lua function */
#define cast_Lfunc(p)	cast(lua_CFunction, cast_func(p))


/*
** system-dependent functions
*/

/*
** unload library 'lib'
*/
static void lsys_unloadlib (void *lib);

/*
** load C library in file 'path'. If 'seeglb', load with all names in
** the library global.
** Returns the library; in case of error, returns NULL plus an
** error string in the stack.
*/
static void *lsys_load (lua_State *L, const char *path, int seeglb);

/*
** Try to find a function named 'sym' in library 'lib'.
** Returns the function; in case of error, returns NULL plus an
** error string in the stack.
*/
static lua_CFunction lsys_sym (lua_State *L, void *lib, const char *sym);




#if defined(LUA_USE_DLOPEN)	/* { */
/*
** {========================================================================
** This is an implementation of loadlib based on the dlfcn interface,
** which is available in all POSIX systems.
** =========================================================================
*/

#include <dlfcn.h>


static void lsys_unloadlib (void *lib) {
  dlclose(lib);
}


static void *lsys_load (lua_State *L, const char *path, int seeglb) {
  void *lib = dlopen(path, RTLD_NOW | (seeglb ? RTLD_GLOBAL : RTLD_LOCAL));
  if (l_unlikely(lib == NULL))
    lua_pushstring(L, dlerror());
  return lib;
}


static lua_CFunction lsys_sym (lua_State *L, void *lib, const char *sym) {
  lua_CFunction f = cast_Lfunc(dlsym(lib, sym));
  if (l_unlikely(f == NULL))
    lua_pushstring(L, dlerror());
  return f;
}

/* }====================================================== */



#elif defined(LUA_DL_DLL)	/* }{ */
/*
** {======================================================================
** This is an implementation of loadlib for Windows using native functions.
** =======================================================================
*/

#include <windows.h>


/*
** optional flags for LoadLibraryEx
*/
#if !defined(LUA_LLE_FLAGS)
#define LUA_LLE_FLAGS	0
#endif


#undef setprogdir


/*
** Replace in the path (on the top of the stack) any occurrence
** of LUA_EXEC_DIR with the executable's path.
*/
static void setprogdir (lua_State *L) {
  char buff[MAX_PATH + 1];
  char *lb;
  DWORD nsize = sizeof(buff)/sizeof(char);
  DWORD n = GetModuleFileNameA(NULL, buff, nsize);  /* get exec. name */
  if (n == 0 || n == nsize || (lb = strrchr(buff, '\\')) == NULL)
    luaL_error(L, "unable to get ModuleFileName");
  else {
    *lb = '\0';  /* cut name on the last '\\' to get the path */
    luaL_gsub(L, lua_tostring(L, -1), LUA_EXEC_DIR, buff);
    lua_remove(L, -2);  /* remove original string */
  }
}




static void pusherror (lua_State *L) {
  int error = GetLastError();
  char buffer[128];
  if (FormatMessageA(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
      NULL, error, 0, buffer, sizeof(buffer)/sizeof(char), NULL))
    lua_pushstring(L, buffer);
  else
    lua_pushfstring(L, "system error %d\n", error);
}

static void lsys_unloadlib (void *lib) {
  FreeLibrary((HMODULE)lib);
}


static void *lsys_load (lua_State *L, const char *path, int seeglb) {
  HMODULE lib = LoadLibraryExA(path, NULL, LUA_LLE_FLAGS);
  (void)(seeglb);  /* not used: symbols are 'global' by default */
  if (lib == NULL) pusherror(L);
  return lib;
}


static lua_CFunction lsys_sym (lua_State *L, void *lib, const char *sym) {
  lua_CFunction f = cast_Lfunc(GetProcAddress((HMODULE)lib, sym));
  if (f == NULL) pusherror(L);
  return f;
}

/* }====================================================== */


#else				/* }{ */
/*
** {======================================================
** Fallback for other systems
** =======================================================
*/

#undef LIB_FAIL
#define LIB_FAIL	"absent"


#define DLMSG	"dynamic libraries not enabled; check your Lua installation"


static void lsys_unloadlib (void *lib) {
  (void)(lib);  /* not used */
}


static void *lsys_load (lua_State *L, const char *path, int seeglb) {
  (void)(path); (void)(seeglb);  /* not used */
  lua_pushliteral(L, DLMSG);
  return NULL;
}


static lua_CFunction lsys_sym (lua_State *L, void *lib, const char *sym) {
  (void)(lib); (void)(sym);  /* not used */
  lua_pushliteral(L, DLMSG);
  return NULL;
}

/* }====================================================== */
#endif				/* } */


/*
** {==================================================================
** Set Paths
** ===================================================================
*/

/*
** LUA_PATH_VAR and LUA_CPATH_VAR are the names of the environment
** variables that Lua check to set its paths.
*/
#if !defined(LUA_PATH_VAR)
#define LUA_PATH_VAR    "LUA_PATH"
#endif

#if !defined(LUA_CPATH_VAR)
#define LUA_CPATH_VAR   "LUA_CPATH"
#endif



/*
** return registry.LUA_NOENV as a boolean
*/
static int noenv (lua_State *L) {
  int b;
  lua_getfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
  b = lua_toboolean(L, -1);
  lua_pop(L, 1);  /* remove value */
  return b;
}


/*
** Set a path. (If using the default path, assume it is a string
** literal in C and create it as an external string.)
*/
static void setpath (lua_State *L, const char *fieldname,
                                   const char *envname,
                                   const char *dft) {
  const char *dftmark;
  const char *nver = lua_pushfstring(L, "%s%s", envname, LUA_VERSUFFIX);
  const char *path = getenv(nver);  /* try versioned name */
  if (path == NULL)  /* no versioned environment variable? */
    path = getenv(envname);  /* try unversioned name */
  if (path == NULL || noenv(L))  /* no environment variable? */
    lua_pushexternalstring(L, dft, strlen(dft), NULL, NULL);  /* use default */
  else if ((dftmark = strstr(path, LUA_PATH_SEP LUA_PATH_SEP)) == NULL)
    lua_pushstring(L, path);  /* nothing to change */
  else {  /* path contains a ";;": insert default path in its place */
    size_t len = strlen(path);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    if (path < dftmark) {  /* is there a prefix before ';;'? */
      luaL_addlstring(&b, path, ct_diff2sz(dftmark - path));  /* add it */
      luaL_addchar(&b, *LUA_PATH_SEP);
    }
    luaL_addstring(&b, dft);  /* add default */
    if (dftmark < path + len - 2) {  /* is there a suffix after ';;'? */
      luaL_addchar(&b, *LUA_PATH_SEP);
      luaL_addlstring(&b, dftmark + 2, ct_diff2sz((path + len - 2) - dftmark));
    }
    luaL_pushresult(&b);
  }
  setprogdir(L);
  lua_setfield(L, -3, fieldname);  /* package[fieldname] = path value */
  lua_pop(L, 1);  /* pop versioned variable name ('nver') */
}

/* }================================================================== */


/*
** External strings created by DLLs may need the DLL code to be
** deallocated. Library strings therefore have VM lifetime; shutdown destroys
** ordinary objects before these fixed tokens. Parallel destruction need not
** impose a global order on unrelated external-string callbacks.
** (The string itself is irrelevant; its userdata is the DLL pointer.)
*/


/*
** return registry.CLIBS[path]
*/
static void *checkclib (lua_State *L, const char *path) {
  void *plib;
  lua_getfield(L, LUA_REGISTRYINDEX, CLIBS);
  lua_getfield(L, -1, path);
  plib = lua_touserdata(L, -1);  /* plib = CLIBS[path] */
  lua_pop(L, 2);  /* pop CLIBS table and 'plib' */
  return plib;
}


/*
** Deallocate function for library strings.
** Unload the DLL associated with the string being deallocated.
*/
static void *freelib (void *ud, void *ptr, size_t osize, size_t nsize) {
  /* string itself is irrelevant and static */
  (void)ptr; (void)osize; (void)nsize;
  lsys_unloadlib(ud);  /* unload library represented by the string */
  return NULL;
}


/*
** Create a library string that, when deallocated, will unload 'plib'
*/
static void createlibstr (lua_State *L, void *plib) {
  /* common content for all library strings */
  static const char dummy[] = "01234567890";
  TString *token;
  lua_lock(L);
  /* Fix the token before publication, keeping its construction-list link
  ** stable across allocation and registration. */
  luaW_heapenter(L);
  token = luaS_newextlstr(L, dummy, sizeof(dummy) - 1, freelib, plib);
  luaC_fix(L, obj2gco(token));
  setsvalue2s(L, L->top.p, token);
  api_incr_top(L);
  luaW_heapleave(L);
  luaC_checkGC(L);
  lua_unlock(L);
}


/*
** registry.CLIBS[path] = plib          -- for queries.
** Keep a reference to the fixed library string alongside the handle.
*/
static void addtoclib (lua_State *L, const char *path, void *plib) {
  lua_getfield(L, LUA_REGISTRYINDEX, CLIBS);
  lua_pushlightuserdata(L, plib);
  lua_setfield(L, -2, path);  /* CLIBS[path] = plib */
  createlibstr(L, plib);
  luaL_ref(L, -2);  /* keep library string in CLIBS */
  lua_pop(L, 1);  /* pop CLIBS table */
}


/* error codes for 'lookforfunc' */
#define ERRLIB		1
#define ERRFUNC		2

/*
** Look for a C function named 'sym' in a dynamically loaded library
** 'path'.
** First, check whether the library is already loaded; if not, try
** to load it.
** Then, if 'sym' is '*', return true (as library has been loaded).
** Otherwise, look for symbol 'sym' in the library and push a
** C function with that symbol.
** Return 0 with 'true' or a function in the stack; in case of
** errors, return an error code with an error message in the stack.
*/
static int lookforfunc (lua_State *L, const char *path, const char *sym) {
  void *reg = checkclib(L, path);  /* check loaded C libraries */
  if (reg == NULL) {  /* must load library? */
    reg = lsys_load(L, path, *sym == '*');  /* global symbols if 'sym'=='*' */
    if (reg == NULL) return ERRLIB;  /* unable to load library */
    addtoclib(L, path, reg);
  }
  if (*sym == '*') {  /* loading only library (no function)? */
    lua_pushboolean(L, 1);  /* return 'true' */
    return 0;  /* no errors */
  }
  else {
    lua_CFunction f = lsys_sym(L, reg, sym);
    if (f == NULL)
      return ERRFUNC;  /* unable to find function */
    lua_pushcfunction(L, f);  /* else create new function */
    return 0;  /* no errors */
  }
}


static int ll_loadlib (lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  const char *init = luaL_checkstring(L, 2);
  int stat = lookforfunc(L, path, init);
  if (l_likely(stat == 0))  /* no errors? */
    return 1;  /* return the loaded function */
  else {  /* error; error message is on stack top */
    luaL_pushfail(L);
    lua_insert(L, -2);
    lua_pushstring(L, (stat == ERRLIB) ?  LIB_FAIL : "init");
    return 3;  /* return fail, error message, and where */
  }
}



/*
** {======================================================
** 'require' function
** =======================================================
*/


static int readable (const char *filename) {
  FILE *f = fopen(filename, "r");  /* try to open file */
  if (f == NULL) return 0;  /* open failed */
  fclose(f);
  return 1;
}


/*
** Get the next name in '*path' = 'name1;name2;name3;...', changing
** the ending ';' to '\0' to create a zero-terminated string. Return
** NULL when list ends.
*/
static const char *getnextfilename (char **path, char *end) {
  char *sep;
  char *name = *path;
  if (name == end)
    return NULL;  /* no more names */
  else if (*name == '\0') {  /* from previous iteration? */
    *name = *LUA_PATH_SEP;  /* restore separator */
    name++;  /* skip it */
  }
  sep = strchr(name, *LUA_PATH_SEP);  /* find next separator */
  if (sep == NULL)  /* separator not found? */
    sep = end;  /* name goes until the end */
  *sep = '\0';  /* finish file name */
  *path = sep;  /* will start next search from here */
  return name;
}


/*
** Given a path such as ";blabla.so;blublu.so", pushes the string
**
** no file 'blabla.so'
**	no file 'blublu.so'
*/
static void pusherrornotfound (lua_State *L, const char *path) {
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addstring(&b, "no file '");
  luaL_addgsub(&b, path, LUA_PATH_SEP, "'\n\tno file '");
  luaL_addstring(&b, "'");
  luaL_pushresult(&b);
}


static const char *searchpath (lua_State *L, const char *name,
                                             const char *path,
                                             const char *sep,
                                             const char *dirsep) {
  luaL_Buffer buff;
  char *pathname;  /* path with name inserted */
  char *endpathname;  /* its end */
  const char *filename;
  /* separator is non-empty and appears in 'name'? */
  if (*sep != '\0' && strchr(name, *sep) != NULL)
    name = luaL_gsub(L, name, sep, dirsep);  /* replace it by 'dirsep' */
  luaL_buffinit(L, &buff);
  /* add path to the buffer, replacing marks ('?') with the file name */
  luaL_addgsub(&buff, path, LUA_PATH_MARK, name);
  luaL_addchar(&buff, '\0');
  pathname = luaL_buffaddr(&buff);  /* writable list of file names */
  endpathname = pathname + luaL_bufflen(&buff) - 1;
  while ((filename = getnextfilename(&pathname, endpathname)) != NULL) {
    if (readable(filename))  /* does file exist and is readable? */
      return lua_pushstring(L, filename);  /* save and return name */
  }
  luaL_pushresult(&buff);  /* push path to create error message */
  pusherrornotfound(L, lua_tostring(L, -1));  /* create error message */
  return NULL;  /* not found */
}


static int ll_searchpath (lua_State *L) {
  const char *f = searchpath(L, luaL_checkstring(L, 1),
                                luaL_checkstring(L, 2),
                                luaL_optstring(L, 3, "."),
                                luaL_optstring(L, 4, LUA_DIRSEP));
  if (f != NULL) return 1;
  else {  /* error message is on top of the stack */
    luaL_pushfail(L);
    lua_insert(L, -2);
    return 2;  /* return fail + error message */
  }
}


static const char *findfile (lua_State *L, const char *name,
                                           const char *pname,
                                           const char *dirsep) {
  const char *path;
  lua_getfield(L, lua_upvalueindex(1), pname);
  path = lua_tostring(L, -1);
  if (l_unlikely(path == NULL))
    luaL_error(L, "'package.%s' must be a string", pname);
  return searchpath(L, name, path, ".", dirsep);
}


static int checkload (lua_State *L, int stat, const char *filename) {
  if (l_likely(stat)) {  /* module loaded successfully? */
    lua_pushstring(L, filename);  /* will be 2nd argument to module */
    return 2;  /* return open function and file name */
  }
  else
    return luaL_error(L, "error loading module '%s' from file '%s':\n\t%s",
                          lua_tostring(L, 1), filename, lua_tostring(L, -1));
}


static int searcher_Lua (lua_State *L) {
  const char *filename;
  const char *name = luaL_checkstring(L, 1);
  filename = findfile(L, name, "path", LUA_LSUBSEP);
  if (filename == NULL) return 1;  /* module not found in this path */
  return checkload(L, (luaL_loadfilex(L, filename, "bt") == LUA_OK), filename);
}


/*
** Try to find a load function for module 'modname' at file 'filename'.
** First, change '.' to '_' in 'modname'; then, if 'modname' has
** the form X-Y (that is, it has an "ignore mark"), build a function
** name "luaopen_X" and look for it. (For compatibility, if that
** fails, it also tries "luaopen_Y".) If there is no ignore mark,
** look for a function named "luaopen_modname".
*/
static int loadfunc (lua_State *L, const char *filename, const char *modname) {
  const char *openfunc;
  const char *mark;
  modname = luaL_gsub(L, modname, ".", LUA_OFSEP);
  mark = strchr(modname, *LUA_IGMARK);
  if (mark) {
    int stat;
    openfunc = lua_pushlstring(L, modname, ct_diff2sz(mark - modname));
    openfunc = lua_pushfstring(L, LUA_POF"%s", openfunc);
    stat = lookforfunc(L, filename, openfunc);
    if (stat != ERRFUNC) return stat;
    modname = mark + 1;  /* else go ahead and try old-style name */
  }
  openfunc = lua_pushfstring(L, LUA_POF"%s", modname);
  return lookforfunc(L, filename, openfunc);
}


static int searcher_C (lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  const char *filename = findfile(L, name, "cpath", LUA_CSUBSEP);
  if (filename == NULL) return 1;  /* module not found in this path */
  return checkload(L, (loadfunc(L, filename, name) == 0), filename);
}


static int searcher_Croot (lua_State *L) {
  const char *filename;
  const char *name = luaL_checkstring(L, 1);
  const char *p = strchr(name, '.');
  int stat;
  if (p == NULL) return 0;  /* is root */
  lua_pushlstring(L, name, ct_diff2sz(p - name));
  filename = findfile(L, lua_tostring(L, -1), "cpath", LUA_CSUBSEP);
  if (filename == NULL) return 1;  /* root not found */
  if ((stat = loadfunc(L, filename, name)) != 0) {
    if (stat != ERRFUNC)
      return checkload(L, 0, filename);  /* real error */
    else {  /* open function not found */
      lua_pushfstring(L, "no module '%s' in file '%s'", name, filename);
      return 1;
    }
  }
  lua_pushstring(L, filename);  /* will be 2nd argument to module */
  return 2;
}


static TString *packagekey (lua_State *L, unsigned index) {
  return __atomic_load_n(&G(L)->package_keys[index], __ATOMIC_ACQUIRE);
}

static void initpackagekeys (lua_State *L) {
  static const char *const names[LUA_PACKAGE_KEYS] = {
    [LUA_PACKAGE_LOADED] = LUA_LOADED_TABLE,
    [LUA_PACKAGE_PRELOAD] = LUA_PRELOAD_TABLE,
    [LUA_PACKAGE_SEARCHERS] = "searchers",
    [LUA_PACKAGE_PRELOAD_ORIGIN] = ":preload:"
  };
  unsigned i;
  lua_lock(L);
  for (i = 0; i < LUA_PACKAGE_KEYS; i++)
    luaS_anchorkey(L, &G(L)->package_keys[i], names[i]);
  lua_unlock(L);
}

/* These lookups use private snapshots, not debug-visible stack temporaries.
** The caller owns the state; slow paths retain the ordinary metamethod rules. */
static void getroot (lua_State *L, const LuaNativeRoot *object,
                     TValue *key, LuaNativeRoot *result) {
  lu_byte tag;
  luaV_fastget(&object->value, key, &result->value, luaH_get, tag);
  if (luaV_needsindex(tag)) {
    setnilvalue2s(L->top.p);
    api_incr_top(L);
    luaV_finishget(L, &object->value, key, L->top.p - 1);
    luaR_setroot(L, result, s2v(L->top.p - 1));
    L->top.p--;
  }
}

static void getfieldroot (lua_State *L, const LuaNativeRoot *object,
                          TString *field, LuaNativeRoot *result) {
  TValue key;
  setsvalue(L, &key, field);  /* the VM retains all package keys */
  getroot(L, object, &key, result);
}

static void setroot (lua_State *L, const LuaNativeRoot *object,
                     LuaNativeRoot *key, LuaNativeRoot *value) {
  luaV_settable(L, &object->value, &key->value, &value->value);
}

static void getregistryroot (lua_State *L, TString *field,
                             LuaNativeRoot *result) {
  LuaNativeRoot registry;
  luaR_pushroot(L, &registry);
  luaA_getregistry(L, &registry);
  getfieldroot(L, &registry, field, result);
  luaR_poproot(L, &registry);
}

static void pushroot (lua_State *L, const LuaNativeRoot *root) {
  setobj2s(L, L->top.p, &root->value);
  api_incr_top(L);
}

static void poproots (lua_State *L, LuaNativeRoot *roots, unsigned count) {
  while (count != 0)
    luaR_poproot(L, &roots[--count]);
}

/* The searcher and the cache use C-string keys. The loader still receives
** the original Lua name, including any bytes after an embedded zero. */
static TString *modulekey (lua_State *L, const LuaNativeRoot *name,
                           size_t len) {
  TString *string = tsvalue(&name->value);
  const char *bytes = getstr(string);
  if (strisinterned(string) && memchr(bytes, '\0', len) == NULL)
    return string;
  return luaS_new(L, bytes);
}

static int searcher_preload (lua_State *L) {
  LuaNativeRoot roots[4];  /* original name, C-string key, preload, result */
  size_t len;
  const char *name;
  unsigned index;
  lua_lock(L);
  name = luaL_ownedrootstring(L, 1, &roots[0], &len);
  for (index = 1; index < 4; ++index) luaR_pushroot(L, &roots[index]);
  setsvalue(L, &roots[1].value, modulekey(L, &roots[0], len));
  getregistryroot(L, packagekey(L, LUA_PACKAGE_PRELOAD), &roots[2]);
  getroot(L, &roots[2], &roots[1].value, &roots[3]);
  if (ttisnil(&roots[3].value)) {
    lua_unlock(L);
    lua_pushfstring(L, "no field package.preload['%s']", name);
    return luaL_rootresult(L, roots, 4, 1);
  }
  pushroot(L, &roots[3]);
  setsvalue2s(L, L->top.p, packagekey(L, LUA_PACKAGE_PRELOAD_ORIGIN));
  api_incr_top(L);
  luaC_checkGC(L);
  poproots(L, roots, 4);
  lua_unlock(L);
  return 2;
}


enum { RNAME, RKEY, RPACKAGE, RLOADED, RSEARCHERS, RLOADER, RDATA, RVALUE,
       REQUIRE_ROOTS };

static int tryregistryfield (lua_State *L, TString *key, TValue *value) {
  TValue registry;
  lu_byte tag;
  return luaA_tryregistry(G(L), &registry) && ttistable(&registry) &&
         luaH_trygetshortstr(L, hvalue(&registry), key, value, &tag) &&
         !tagisempty(tag);
}

#if !defined(HARDSTACKTESTS)
/* A successful built-in preload search has no Lua call or allocation. Use
** the live searcher and registry entries, and keep a real searcher frame
** for hooks, stack growth, collector work and any unresolved lookup. */
static int trypreload (lua_State *L, LuaNativeRoot *roots) {
  TValue *searcher = &roots[RLOADER].value;
  TValue preload, loader;
  lu_byte tag;
  if (!ttisCclosure(searcher) || !clCvalue(searcher)->managed ||
      clCvalue(searcher)->f != searcher_preload || !ttisshrstring(&roots[RKEY].value) ||
      gethookmask(L) || getCcalls(L) >= LUAI_MAXCCALLS - 1 ||
      L->ci->next == NULL || L->stack_last.p - L->top.p <= LUA_MINSTACK + 2 ||
      L->policy != NULL || luaTK_hasstops(L) || luaW_ownsheap(L) || luaC_tracing(L))
    return 0;
  if (!tryregistryfield(L, packagekey(L, LUA_PACKAGE_PRELOAD), &preload) ||
      !ttistable(&preload) ||
      !luaH_trygetshortstr(L, hvalue(&preload), tsvalue(&roots[RKEY].value),
                           &loader, &tag) || tagisempty(tag) || !ttisfunction(&loader))
    return 0;
  luaC_condGC(L, return 0, (void)0);
  luaD_checknativestack(L);
  luaR_copyroot(L, &roots[RLOADER], &loader);
  setsvalue(L, &roots[RDATA].value, packagekey(L, LUA_PACKAGE_PRELOAD_ORIGIN));
  return 1;
}
#endif

/* Take the caller's stack ownership through a successful search. */
static void findloader (lua_State *L, LuaNativeRoot *roots, const char *name) {
  lua_Unsigned i;
  unsigned work = 0;
  int top;
  int hasmessage = 0;
  luaL_Buffer msg;  /* to build error message */
  top = cast_int(L->top.p - (L->ci->func.p + 1));
  getfieldroot(L, &roots[RPACKAGE], packagekey(L, LUA_PACKAGE_SEARCHERS),
               &roots[RSEARCHERS]);
  if (l_unlikely(!ttistable(&roots[RSEARCHERS].value))) {
    lua_unlock(L);
    luaL_error(L, "'package.searchers' must be a table");
  }
  /*  iterate over available searchers to find a loader */
  for (i = 1; ; i++) {
    lu_byte tag;
    tag = luaH_getint(L, hvalue(&roots[RSEARCHERS].value), cast(lua_Integer, i),
                     &roots[RLOADER].value);
    if (tagisempty(tag)) setnilvalue(&roots[RLOADER].value);
    if (l_unlikely(ttisnil(&roots[RLOADER].value) || i > LUA_MAXINTEGER)) {
      lua_unlock(L);
      if (!hasmessage) luaL_error(L, "module '%s' not found:", name);
      luaL_buffsub(&msg, 2);  /* remove last prefix */
      luaL_pushresult(&msg);  /* create error message */
      luaL_error(L, "module '%s' not found:%s", name, lua_tostring(L, -1));
    }
#if !defined(HARDSTACKTESTS)
    if (i == 1 && trypreload(L, roots))
      return;
#endif
    pushroot(L, &roots[RLOADER]);
    pushroot(L, &roots[RKEY]);
    luaD_callnoyield(L, L->top.p - 2, 2);
    luaR_setroot(L, &roots[RLOADER], s2v(L->top.p - 2));
    luaR_setroot(L, &roots[RDATA], s2v(L->top.p - 1));
    L->top.p -= 2;
    if (ttisfunction(&roots[RLOADER].value)) {
      if (hasmessage) {
        lua_unlock(L);
        lua_settop(L, top);  /* close the message buffer */
        lua_lock(L);
      }
      return;
    }
    lua_unlock(L);
    if (ttisstring(&roots[RLOADER].value) || cvt2str(&roots[RLOADER].value)) {
      size_t len;
      const char *s = luaL_roottostring(L, &roots[RLOADER], &len);
      if (!hasmessage) {
        luaL_buffinit(L, &msg);
        luaL_addstring(&msg, "\n\t");
        hasmessage = 1;
      }
      while (len != 0) {
        size_t chunk = len < 1024u ? len : 1024u;
        luaL_addlstring(&msg, s, chunk);
        s += chunk; len -= chunk;
        luaQ_workn(L, &work, chunk);
      }
      luaL_addstring(&msg, "\n\t");  /* prefix for next message */
    }
    luaQ_work(L, &work);
    lua_lock(L);
  }
}


/* Consult the actual registry on every call. A raw lookup that would
** need a metamethod or a wait is completed by the ordinary rooted path. */
static int tryloaded (lua_State *L, TString *module, TValue *loaded,
                       TValue *value) {
  lu_byte tag;
  if (luaC_tracing(L) ||
      !tryregistryfield(L, packagekey(L, LUA_PACKAGE_LOADED), loaded) ||
      !ttistable(loaded))
    return 0;
  return luaH_trygetshortstr(L, hvalue(loaded), module, value, &tag) &&
         !luaV_needsindex(tag);
}


static int ll_require (lua_State *L) {
  LuaNativeRoot roots[REQUIRE_ROOTS];
  TValue loaded, value;
  int lookup = 0;
  size_t len;
  unsigned index;
  const char *name;
  lua_lock(L);
  if (L->top.p == L->ci->func.p + 2 && ttisshrstring(s2v(L->top.p - 1))) {
    TString *module = tsvalue(s2v(L->top.p - 1));
    if (memchr(getshrstr(module), '\0', module->shrlen) == NULL &&
        tryloaded(L, module, &loaded, &value)) {
      if (!l_isfalse(&value)) {
        setobj2s(L, L->top.p, &value);
        api_incr_top(L);
        lua_unlock(L);
        return 1;
      }
      lookup = 1;
    }
  }
  name = luaL_ownedrootstring(L, 1, &roots[RNAME], &len);
  if (L->top.p != L->ci->func.p + 2) {
    lua_unlock(L);
    lua_settop(L, 1);
    lua_lock(L);
  }
  for (index = 1; index < REQUIRE_ROOTS; ++index)
    luaR_pushroot(L, &roots[index]);
  if (lookup) {
    /* The probe used the interned name and no operation has parked since. */
    luaR_copyroot(L, &roots[RKEY], &roots[RNAME].value);
    luaR_copyroot(L, &roots[RLOADED], &loaded);
    luaR_copyroot(L, &roots[RVALUE], &value);
  }
  else
    setsvalue(L, &roots[RKEY].value, modulekey(L, &roots[RNAME], len));
  if (ttisCclosure(s2v(L->ci->func.p))) {
    CClosure *function = clCvalue(s2v(L->ci->func.p));
    if (function->nupvalues >= 1) {
      if (luaA_readslot(&function->sequence, &function->upvalue[0], &value))
        luaR_setroot(L, &roots[RPACKAGE], &value);
      else {
        LuaObjectScope scope;
        luaA_begin(L, obj2gco(function), &scope);
        luaR_setroot(L, &roots[RPACKAGE], &function->upvalue[0]);
        luaA_end(L, &scope);
      }
    }
  }
  if (!lookup) {
    getregistryroot(L, packagekey(L, LUA_PACKAGE_LOADED), &roots[RLOADED]);
    getroot(L, &roots[RLOADED], &roots[RKEY].value, &roots[RVALUE]);
  }
  if (!l_isfalse(&roots[RVALUE].value)) {
    pushroot(L, &roots[RVALUE]);
    poproots(L, roots, REQUIRE_ROOTS);
    lua_unlock(L);
    return 1;
  }
  findloader(L, roots, name);
  pushroot(L, &roots[RLOADER]);
  pushroot(L, &roots[RNAME]);
  pushroot(L, &roots[RDATA]);
  luaD_callnoyield(L, L->top.p - 3, 1);
  luaR_setroot(L, &roots[RVALUE], s2v(L->top.p - 1));
  L->top.p--;
  if (!ttisnil(&roots[RVALUE].value))
    setroot(L, &roots[RLOADED], &roots[RKEY], &roots[RVALUE]);
  getroot(L, &roots[RLOADED], &roots[RKEY].value, &roots[RVALUE]);
  if (ttisnil(&roots[RVALUE].value)) {
    setbtvalue(&roots[RVALUE].value);
    setroot(L, &roots[RLOADED], &roots[RKEY], &roots[RVALUE]);
  }
  pushroot(L, &roots[RVALUE]);
  pushroot(L, &roots[RDATA]);
  poproots(L, roots, REQUIRE_ROOTS);
  lua_unlock(L);
  return 2;
}

/* }====================================================== */




static const luaL_Reg pk_funcs[] = {
  {"loadlib", ll_loadlib},
  {"searchpath", ll_searchpath},
  /* placeholders */
  {"preload", NULL},
  {"cpath", NULL},
  {"path", NULL},
  {"searchers", NULL},
  {"loaded", NULL},
  {NULL, NULL}
};


static void createsearcherstable (lua_State *L) {
  static const lua_CFunction searchers[] = {
    searcher_preload,
    searcher_Lua,
    searcher_C,
    searcher_Croot,
    NULL
  };
  int i;
  /* create 'searchers' table */
  lua_createtable(L, sizeof(searchers)/sizeof(searchers[0]) - 1, 0);
  /* fill it with predefined searchers */
  for (i=0; searchers[i] != NULL; i++) {
    lua_pushvalue(L, -2);  /* set 'package' as upvalue for all searchers */
    if (searchers[i] == searcher_preload)
      luaQ_pushmanaged(L, searchers[i], 1);
    else lua_pushcclosure(L, searchers[i], 1);
    lua_rawseti(L, -2, i+1);
  }
  lua_setfield(L, -2, "searchers");  /* put it in field 'searchers' */
}


LUAMOD_API int luaopen_package (lua_State *L) {
  initpackagekeys(L);
  luaL_getsubtable(L, LUA_REGISTRYINDEX, CLIBS);  /* create CLIBS table */
  lua_pop(L, 1);  /* will not use it now */
  luaL_newlib(L, pk_funcs);  /* create 'package' table */
  createsearcherstable(L);
  /* set paths */
  setpath(L, "path", LUA_PATH_VAR, LUA_PATH_DEFAULT);
  setpath(L, "cpath", LUA_CPATH_VAR, LUA_CPATH_DEFAULT);
  /* store config information */
  lua_pushliteral(L, LUA_DIRSEP "\n" LUA_PATH_SEP "\n" LUA_PATH_MARK "\n"
                     LUA_EXEC_DIR "\n" LUA_IGMARK "\n");
  lua_setfield(L, -2, "config");
  /* set field 'loaded' */
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_setfield(L, -2, "loaded");
  /* set field 'preload' */
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  lua_setfield(L, -2, "preload");
  lua_pushglobaltable(L);
  lua_pushvalue(L, -2);  /* set 'package' as upvalue for next lib */
  luaQ_pushmanaged(L, ll_require, 1);
  lua_setfield(L, -2, "require");
  lua_pop(L, 1);  /* pop global table */
  return 1;  /* return 'package' table */
}

