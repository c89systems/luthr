/*
** $Id: lstring.h $
** String table (keep all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#ifndef lstring_h
#define lstring_h

#include "lgc.h"
#include "lobject.h"
#include "lstate.h"


/*
** Memory-allocation error message must be preallocated (it cannot
** be created after memory is exhausted)
*/
#define MEMERRMSG       "not enough memory"


/*
** Maximum length for short strings, that is, strings that are
** internalized. (Cannot be smaller than reserved words or tags for
** metamethods, as these strings must be internalized;
** #("function") = 8, #("__newindex") = 10.)
*/
#if !defined(LUAI_MAXSHORTLEN)
#define LUAI_MAXSHORTLEN	40
#endif

LUAI_FUNC unsigned luaS_hash (const char *str, size_t len, unsigned seed);


/*
** Size of a short TString: Size of the header plus space for the string
** itself (including final '\0').
*/
#define sizestrshr(l)  \
	(offsetof(TString, contents) + ((l) + 1) * sizeof(char))
#define REGULAR_STRING_HEADER \
  (offsetof(TString, storage) + sizeof(unsigned int))
#define sizestrreg(l) (REGULAR_STRING_HEADER + (l) + 1)


#define luaS_newliteral(L, s)	(luaS_newlstr(L, "" s, \
                                 (sizeof(s)/sizeof(char))-1))


/*
** test whether a string is a reserved word
*/
#define isreserved(s)	(strisshr(s) && (s)->extra > 0)


/*
** equality for short strings, which are always internalized
*/
#define eqshrstr(a,b)	check_exp((a)->tt == LUA_VSHRSTR, (a) == (b))


LUAI_FUNC unsigned luaS_computelonghash (TString *ts);

l_sinline unsigned luaS_hashlongstr (TString *ts) {
  unsigned hash = __atomic_load_n(&ts->hash, __ATOMIC_RELAXED);
  lua_assert(ts->tt == LUA_VLNGSTR);
  return l_unlikely(hash == 0) ? luaS_computelonghash(ts) : hash;
}

LUAI_FUNC void luaS_finishlongstr (TString *ts);
LUAI_FUNC void luaS_buildlongstr (lua_State *L, TString *ts, const char *src,
                                  unsigned *work);
LUAI_FUNC int luaS_eqstr (TString *a, TString *b);
LUAI_FUNC void luaS_shrink (lua_State *L);
LUAI_FUNC void luaS_freeindex (lua_State *L, LuaStringIndex *index);
LUAI_FUNC unsigned luaS_indexsize (const stringtable *table);
LUAI_FUNC void luaS_clearcache (global_State *g);
LUAI_FUNC void luaS_init (lua_State *L);
LUAI_FUNC void luaS_remove (lua_State *L, TString *ts);
LUAI_FUNC Udata *luaS_newudata (lua_State *L, size_t s,
                                              unsigned short nuvalue);
LUAI_FUNC TString *luaS_newlstr (lua_State *L, const char *str, size_t l);
LUAI_FUNC TString *luaS_cachelstr (lua_State *L, const char *str, size_t len);
LUAI_FUNC TString *luaS_trycachedshort (lua_State *L, const char *str, size_t len);
/* A managed caller can reuse an existing interned value without allocating. */
LUAI_FUNC TString *luaS_findshort (lua_State *L, const char *str, size_t len);
LUAI_FUNC TString *luaS_trytasklstr (lua_State *L, const char *str, size_t len);
LUAI_FUNC TString *luaS_trycachedname (lua_State *L, const char *str);
LUAI_FUNC TString *luaS_new (lua_State *L, const char *str);
LUAI_FUNC TString *luaS_createlngstrobj (lua_State *L, size_t l, int result);
LUAI_FUNC TString *luaS_newextlstr (lua_State *L,
		const char *s, size_t len, lua_Alloc falloc, void *ud);
LUAI_FUNC size_t luaS_sizelngstr (size_t len, int kind);
LUAI_FUNC TString *luaS_normstr (lua_State *L, TString *ts);

#endif
