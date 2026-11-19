/*
** $Id: ltable.h $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#ifndef ltable_h
#define ltable_h

#include "lobject.h"


#define gnode(t,i)	(&(t)->node[i])
#define gval(n)		(&(n)->i_val)
#define gnext(n)	((n)->u.next)
#define setnext(n,v) __atomic_store_n(&(n)->u.next, (v), __ATOMIC_RELEASE)


/*
** Clear all bits of fast-access metamethods, which means that the table
** may have any of these metamethods. (First access that fails after the
** clearing will set the bit again.)
*/
#define gettableflags(t) __atomic_load_n(&(t)->flags, __ATOMIC_ACQUIRE)
#define settableflags(t,f) __atomic_store_n(&(t)->flags, (f), __ATOMIC_RELEASE)
#define invalidateTMcache(t) \
  __atomic_fetch_and(&(t)->flags, cast_byte(~maskflags), __ATOMIC_RELAXED)


/*
** Bit BITDUMMY set in 'flags' means the table is using the dummy node
** for its hash part.
*/

#define BITDUMMY		(1 << 6)
#define NOTBITDUMMY		cast_byte(~BITDUMMY)
#define isdummy(t)		(gettableflags(t) & BITDUMMY)

#define setnodummy(t) __atomic_fetch_and(&(t)->flags, NOTBITDUMMY, __ATOMIC_RELEASE)
#define setdummy(t) __atomic_fetch_or(&(t)->flags, BITDUMMY, __ATOMIC_RELEASE)



/* allocated size for hash nodes */
#define allocsizenode(t)	(isdummy(t) ? 0 : sizenode(t))


/* returns the Node, given the value of a table entry */
#define nodefromval(v)	cast(Node *, (v))



#define luaH_fastgeti(t,k,res,tag) \
  { tag = luaH_getint(L, t, k, res); }


/*
** 'luaH_get*' operations set 'res', unless the value is absent, and
** return the tag of the result.
*/


/*
** The array part of a table is represented by an inverted array of
** values followed by an array of tags, to avoid wasting space with
** padding. In between them there is an unsigned int, explained later.
** The 'array' pointer points between the two arrays, so that values are
** indexed with negative indices and tags with non-negative indices.

             Values                              Tags
  --------------------------------------------------------
  ...  |   Value 1     |   Value 0     |unsigned|0|1|...
  --------------------------------------------------------
                                       ^ t->array

** All accesses to 't->array' should be through the macros 'getArrTag'
** and 'getArrVal'.
*/

/* Computes the address of the tag for the abstract C-index 'k' */
#define getArrTag(t,k)	(cast(lu_byte*, (t)->array) + sizeof(unsigned) + (k))

/* Computes the address of the value for the abstract C-index 'k' */
#define getArrVal(t,k)	((t)->array - 1 - (k))


/*
** The unsigned between the two arrays is used as a hint for #t;
** see luaH_getn. It is stored there to avoid wasting space in
** the structure Table for tables with no array part.
*/
#define lenhint(t)	cast(unsigned*, (t)->array)


/*
** Move TValues to/from arrays, using C indices
*/
#define arr2obj(h,k,val)  \
  ((val)->tt_ = *getArrTag(h,(k)), (val)->value_ = *getArrVal(h,(k)))

#define obj2arr(h,k,val)  \
  (__atomic_store(getArrVal(h,(k)), &(val)->value_, __ATOMIC_RELEASE), \
   __atomic_store_n(getArrTag(h,(k)), (val)->tt_, __ATOMIC_RELEASE))


/*
** Often, we need to check the tag of a value before moving it. The
** following macros also move TValues to/from arrays, but receive the
** precomputed tag value or address as an extra argument.
*/
#define farr2val(h,k,tag,res)  \
  ((res)->tt_ = tag, (res)->value_ = *getArrVal(h,(k)))

#define fval2arr(h,k,tag,val)  \
  (__atomic_store(getArrVal(h,(k)), &(val)->value_, __ATOMIC_RELEASE), \
   __atomic_store_n((tag), (val)->tt_, __ATOMIC_RELEASE))


LUAI_FUNC lu_byte luaH_get (lua_State *L, Table *t, const TValue *key, TValue *res);
LUAI_FUNC lu_byte luaH_getshortstr (lua_State *L, Table *t, TString *key, TValue *res);
LUAI_FUNC lu_byte luaH_getstr (lua_State *L, Table *t, TString *key, TValue *res);
LUAI_FUNC lu_byte luaH_getint (lua_State *L, Table *t, lua_Integer key, TValue *res);

/* Caller owns 't'. Reads do not allocate; writes require rooted key/value
** storage that remains valid through allocation and collection. */
LUAI_FUNC lu_byte luaH_getowned (Table *t, const TValue *key, TValue *res);
/* Complete a write, including insertion/resize, under the caller's ownership.
** Unless raw, an absent key with a metatable returns zero for method lookup.
** Encoded storage locations never leave this operation. */
LUAI_FUNC int luaH_assignowned (lua_State *L, Table *t, const TValue *key,
                               TValue *value, int raw);

/* Borrowed lookup: requires object ownership or collector quiescence. */
LUAI_FUNC const TValue *luaH_Hgetshortstr (Table *t, TString *key);

LUAI_FUNC void luaH_setint (lua_State *L, Table *t, lua_Integer key,
                                                    TValue *value);
LUAI_FUNC void luaH_set (lua_State *L, Table *t, const TValue *key,
                                                 TValue *value);

LUAI_FUNC Table *luaH_new (lua_State *L);
LUAI_FUNC void luaH_resize (lua_State *L, Table *t, unsigned nasize,
                                                    unsigned nhsize);
LUAI_FUNC void luaH_resizearray (lua_State *L, Table *t, unsigned nasize);
LUAI_FUNC lu_mem luaH_size (Table *t);
LUAI_FUNC void luaH_free (lua_State *L, Table *t);
LUAI_FUNC void luaH_retire (lua_State *L, Table *t);
LUAI_FUNC int luaH_next (lua_State *L, Table *t, StkId key);
LUAI_FUNC lua_Unsigned luaH_getn (lua_State *L, Table *t);


#if defined(LUA_DEBUG)
LUAI_FUNC Node *luaH_mainposition (const Table *t, const TValue *key);
#endif

/* Collector snapshots borrow buffers under an admission storage epoch. Only
** validated chunks may be followed; captured pointers can otherwise be bits
** from a concurrently assigned numeric value. */
LUAI_FUNC unsigned luaH_tracechunk (const Table *, unsigned, unsigned, int,
                                    GCObject **);
LUAI_FUNC int luaH_gcgetmode (Table *, TString *, TValue *);

#endif
