/*
** $Id: lstring.c $
** String table (keeps all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#define lstring_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "laccess.h"
#include "ldebug.h"
#include "ldo.h"
#include "lexec.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"


/*
** Maximum size for string table.
*/
#define MAXSTRTB	cast_int(luaM_limitN(INT_MAX, TString*))

/* Low hash bits select a partition; the remaining bits select its bucket. */
#define stringtableof(g,h) (&(g)->strt[(h) % LUA_STRING_SHARDS])
#define stringbucket(h,n) lmod((h) / LUA_STRING_SHARDS, (n))


/*
** generic equality for strings
*/
int luaS_eqstr (TString *a, TString *b) {
  size_t len1, len2;
  const char *s1 = getlstr(a, len1);
  const char *s2 = getlstr(b, len2);
  return ((len1 == len2) &&  /* equal length and ... */
          (memcmp(s1, s2, len1) == 0));  /* equal contents */
}


static unsigned hashbytes (const char *str, size_t l, unsigned h) {
  for (; l > 0; l--)
    h ^= ((h<<5) + (h>>2) + cast_byte(str[l - 1]));
  return h;
}


static unsigned luaS_hash (const char *str, size_t l, unsigned seed) {
  return hashbytes(str, l, seed ^ cast_uint(l));
}


unsigned luaS_hashlongstr (TString *ts) {
  lua_assert(ts->tt == LUA_VLNGSTR && ts->extra == 1);
  return ts->hash;
}

/* Finish immutable string contents before publishing them to other tasks.
** This removes the shared lazy hash mutation from table lookup. */
void luaS_finishlongstr (TString *ts) {
  lua_assert(ts->tt == LUA_VLNGSTR && ts->extra == 0);
  ts->hash = luaS_hash(getlngstr(ts), ts->u.lnglen, ts->hash);
  ts->extra = 1;
}


/* The caller keeps source and destination rooted across polls. Hash in the
** same order as a normal long string and publish only after copying. */
void luaS_copylongstr (lua_State *L, TString *ts, const char *src,
                       unsigned *work) {
  size_t remaining = ts->u.lnglen;
  char *dest = getlngstr(ts);
  unsigned hash = ts->hash ^ cast_uint(remaining);
  lua_assert(ts->tt == LUA_VLNGSTR && ts->shrlen == LSTRREG && ts->extra == 0);
  while (remaining != 0) {
    size_t chunk = remaining < 1024u ? remaining : 1024u;
    remaining -= chunk;
    memcpy(dest + remaining, src + remaining, chunk);
    hash = hashbytes(dest + remaining, chunk, hash);
    luaQ_workn(L, work, chunk);
  }
  ts->hash = hash;
  ts->extra = 1;
}


static void tablerehash (TString **vect, int osize, int nsize) {
  int i;
  for (i = osize; i < nsize; i++)  /* clear new elements */
    vect[i] = NULL;
  for (i = 0; i < osize; i++) {  /* rehash old part of the array */
    TString *p = vect[i];
    vect[i] = NULL;
    while (p) {  /* for each string in the list */
      TString *hnext = p->u.hnext;  /* save next */
      unsigned int h = stringbucket(p->hash, nsize);  /* new position */
      p->u.hnext = vect[h];  /* chain it into array */
      vect[h] = p;
      p = hnext;
    }
  }
}


/*
** Resize the string table. If allocation fails, keep the current size.
** (This can degrade performance, but any non-zero size should work
** correctly.)
*/
void luaS_resize (lua_State *L, stringtable *tb, int nsize) {
  int osize = tb->size;
  TString **newvect;
  if (nsize < osize)  /* shrinking table? */
    tablerehash(tb->hash, osize, nsize);  /* depopulate shrinking part */
  newvect = luaM_reallocvector(L, tb->hash, osize, nsize, TString*);
  if (l_unlikely(newvect == NULL)) {  /* reallocation failed? */
    if (nsize < osize)  /* was it shrinking table? */
      tablerehash(tb->hash, nsize, osize);  /* restore to original size */
    /* leave table as it was */
  }
  else {  /* allocation succeeded */
    tb->hash = newvect;
    tb->size = nsize;
    if (nsize > osize)
      tablerehash(newvect, osize, nsize);  /* rehash for new size */
  }
}


/*
** Invalidate the weak API caches before sweeping. Only the next owner
** needs to clear a coroutine's entries before it can dereference them.
*/
void luaS_clearcache (global_State *g) {
  if (l_unlikely(++g->strcache_epoch == 0)) {
    LuaParticipation *participant;
    g->strcache_epoch = 1;
    for (participant = g->world.participants; participant != NULL;
         participant = participant->next)
      participant->thread->strcache_epoch = 0;
  }
}


size_t luaS_sizelngstr (size_t len, int kind) {
  switch (kind) {
    case LSTRREG:  /* regular long string */
      /* don't need 'falloc'/'ud', but need space for content */
      return offsetof(TString, falloc) + (len + 1) * sizeof(char);
    case LSTRFIX:  /* fixed external long string */
      /* don't need 'falloc'/'ud' */
      return offsetof(TString, falloc);
    default:  /* external long string with deallocation */
      lua_assert(kind == LSTRMEM);
      return sizeof(TString);
  }
}


/*
** creates a new string object
*/
static TString *createstrobj (lua_State *L, size_t totalsize, lu_byte tag,
                              unsigned h) {
  TString *ts;
  GCObject *o;
  o = luaC_newobj(L, tag, totalsize);
  ts = gco2ts(o);
  ts->hash = h;
  ts->extra = 0;
  return ts;
}


static void copyshrstr (TString *ts, const char *str, size_t len) {
  ts->shrlen = cast(ls_byte, len);
  memcpy(getshrstr(ts), str, len);
  getshrstr(ts)[len] = '\0';
}


/* These values are part of global_State and never enter the GC lists. */
static void initfixedstr (TString *ts, const char *str, size_t len,
                           unsigned seed) {
  ts->tt = LUA_VSHRSTR;
  setmarked(obj2gco(ts), bitmask(QUEUEDBIT) | G_OLD);
  ts->next = NULL;
  ts->extra = 0;
  ts->hash = luaS_hash(str, len, seed);
  ts->u.hnext = NULL;
  copyshrstr(ts, str, len);
}


/*
** Initialize the dynamic intern table and the immutable byte strings.
*/
void luaS_init (lua_State *L) {
  global_State *g = G(L);
  int i;
  for (i = 0; i < LUA_STRING_SHARDS; i++) {
    stringtable *tb = &g->strt[i];
    tb->hash = luaM_newvector(L, LUA_STRING_INITIAL_BUCKETS, TString*);
    tablerehash(tb->hash, 0, LUA_STRING_INITIAL_BUCKETS);
    tb->size = LUA_STRING_INITIAL_BUCKETS;
  }
  /* pre-create memory-error message */
  g->memerrmsg = luaS_newliteral(L, MEMERRMSG);
  luaC_fix(L, obj2gco(g->memerrmsg));  /* it should never be collected */
  g->stoperrmsg = luaS_newliteral(L, "task stopped");
  luaC_fix(L, obj2gco(g->stoperrmsg));
  g->limiterrmsg = luaS_newliteral(L, "Lua instruction limit exceeded");
  luaC_fix(L, obj2gco(g->limiterrmsg));
  initfixedstr(&g->empty_string, "", 0, g->seed);
  for (i = 0; i <= UCHAR_MAX; i++) {
    char byte = cast_char(i);
    initfixedstr(&g->byte_strings[i], &byte, 1, g->seed);
  }
}


TString *luaS_createlngstrobj (lua_State *L, size_t l) {
  TString *ts;
  if (l_unlikely(l >= MAX_SIZE - sizeof(TString)))
    luaM_toobig(L);
  ts = createstrobj(L, luaS_sizelngstr(l, LSTRREG), LUA_VLNGSTR, G(L)->seed);
  ts->u.lnglen = l;
  ts->shrlen = LSTRREG;  /* signals that it is a regular long string */
  ts->contents = cast_charp(ts) + offsetof(TString, falloc);
  ts->contents[l] = '\0';  /* ending 0 */
  return ts;
}


void luaS_remove (lua_State *L, TString *ts) {
  stringtable *tb = stringtableof(G(L), ts->hash);
  TString **p = &tb->hash[stringbucket(ts->hash, tb->size)];
  while (*p != ts)  /* find previous element */
    p = &(*p)->u.hnext;
  *p = (*p)->u.hnext;  /* remove element from its list */
  tb->nuse--;
  if (tb->size > LUA_STRING_INITIAL_BUCKETS && tb->nuse < tb->size / 4 &&
      !__atomic_load_n(&G(L)->shrink_strings, __ATOMIC_RELAXED))
    __atomic_store_n(&G(L)->shrink_strings, 1, __ATOMIC_RELAXED);
}


static void growstrtab (lua_State *L, stringtable *tb) {
  if (l_unlikely(tb->nuse == INT_MAX)) {  /* too many strings? */
    luaC_fullgc(L, 1);  /* try to free some... */
    if (tb->nuse == INT_MAX)  /* still too many? */
      luaM_error(L);  /* cannot even create a message... */
  }
  if (tb->size <= MAXSTRTB / 2)  /* can grow string table? */
    luaS_resize(L, tb, tb->size * 2);
}


/*
** Checks whether short string exists and reuses it or creates a new one.
*/
static TString *internshrstr (lua_State *L, const char *str, size_t l) {
  TString *ts;
  global_State *g = G(L);
  unsigned int h;
  stringtable *tb;
  TString **list;
  LuaObjectScope access;
  if (l == 0) return &g->empty_string;
  if (l == 1) return &g->byte_strings[cast_byte(*str)];
  h = luaS_hash(str, l, g->seed);
  tb = stringtableof(g, h);
  luaA_beginlock(L, &tb->access, &access);
  list = &tb->hash[stringbucket(h, tb->size)];
  lua_assert(str != NULL);  /* otherwise 'memcmp'/'memcpy' are undefined */
  for (ts = *list; ts != NULL; ts = ts->u.hnext) {
    if (l == cast_uint(ts->shrlen) &&
        (memcmp(str, getshrstr(ts), l * sizeof(char)) == 0)) {
      /* found! */
      if (isdead(g, ts))  /* dead (but not collected yet)? */
        luaC_revive(g, obj2gco(ts));
      luaC_tracepointer(L, ts);  /* weak interning becomes a strong reference */
      luaA_end(L, &access);
      return ts;
    }
  }
  /* else must create a new string */
  if (tb->nuse >= tb->size) {  /* need to grow string table? */
    growstrtab(L, tb);
  }
  ts = createstrobj(L, sizestrshr(l), LUA_VSHRSTR, h);
  copyshrstr(ts, str, l);
  /* Allocation may rendezvous with collection. Revalidate the bucket after
  ** that boundary; no pointer into an old bucket array crosses the wait. */
  list = &tb->hash[stringbucket(h, tb->size)];
  ts->u.hnext = *list;
  *list = ts;
  tb->nuse++;
  luaA_end(L, &access);
  return ts;
}


/*
** new string (with explicit length)
*/
TString *luaS_newlstr (lua_State *L, const char *str, size_t l) {
  if (l <= LUAI_MAXSHORTLEN)  /* short string? */
    return internshrstr(L, str, l);
  else {
    TString *ts = luaS_createlngstrobj(L, l);
    memcpy(getlngstr(ts), str, l * sizeof(char));
    luaS_finishlongstr(ts);
    return ts;
  }
}


/*
** Create or reuse a zero-terminated string, first checking in the
** cache (using the string address as a key). The cache can contain
** only zero-terminated strings, so it is safe to use 'strcmp' to
** check hits.
*/
TString *luaS_new (lua_State *L, const char *str) {
  uint64_t epoch = G(L)->strcache_epoch;
  unsigned int i = point2uint(str) % STRCACHE_N;  /* hash */
  int j;
  TString **p = L->strcache[i];
  TString *result;
  if (l_unlikely(L->strcache_epoch != epoch)) {
    memset(L->strcache, 0, sizeof(L->strcache));
    L->strcache_epoch = epoch;
  }
  for (j = 0; j < STRCACHE_M; j++) {
    if (p[j] != NULL && strcmp(str, getstr(p[j])) == 0) { /* hit? */
      result = p[j];
      luaC_tracepointer(L, result);
      return result;  /* that is it */
    }
  }
  /* normal route */
  for (j = STRCACHE_M - 1; j > 0; j--)
    p[j] = p[j - 1];  /* move out last element */
  /* new element is first in the list */
  p[0] = luaS_newlstr(L, str, strlen(str));
  result = p[0];
  return result;
}


Udata *luaS_newudata (lua_State *L, size_t s, unsigned short nuvalue) {
  Udata *u;
  int i;
  GCObject *o;
  if (l_unlikely(s > MAX_SIZE - udatamemoffset(nuvalue)))
    luaM_toobig(L);
  o = luaC_newobj(L, LUA_VUSERDATA, sizeudata(nuvalue, s));
  u = gco2u(o);
  u->len = s;
  luaA_initlock(&u->access);
  luaA_initlock(&u->payload_access);
  luaA_initsharing(L, u);
  if (nuvalue != 0) u->sequence = 0;
  u->nuvalue = nuvalue;
  u->metatable = NULL;
  u->gclist = NULL;
  for (i = 0; i < nuvalue; i++)
    setnilvalue(&u->uv[i].uv);
  return u;
}


struct NewExt {
  ls_byte kind;
  const char *s;
   size_t len;
  TString *ts;  /* output */
};


static void f_newext (lua_State *L, void *ud) {
  struct NewExt *ne = cast(struct NewExt *, ud);
  size_t size = luaS_sizelngstr(0, ne->kind);
  ne->ts = createstrobj(L, size, LUA_VLNGSTR, G(L)->seed);
}


TString *luaS_newextlstr (lua_State *L,
	          const char *s, size_t len, lua_Alloc falloc, void *ud) {
  struct NewExt ne;
  if (!falloc) {
    ne.kind = LSTRFIX;
    f_newext(L, &ne);  /* just create header */
  }
  else {
    ne.kind = LSTRMEM;
    if (luaD_rawrunprotected(L, f_newext, &ne) != LUA_OK) {  /* mem. error? */
      luaAL_freeexternal(L, falloc, ud, cast_voidp(s), len + 1);
      luaM_error(L);  /* re-raise memory error */
    }
    ne.ts->falloc = falloc;
    ne.ts->ud = ud;
  }
  ne.ts->shrlen = ne.kind;
  ne.ts->u.lnglen = len;
  ne.ts->contents = cast_charp(s);
  luaS_finishlongstr(ne.ts);
  return ne.ts;
}


/*
** Normalize an external string: If it is short, internalize it.
*/
TString *luaS_normstr (lua_State *L, TString *ts) {
  size_t len = ts->u.lnglen;
  if (len > LUAI_MAXSHORTLEN)
    return ts;  /* long string; keep the original */
  else {
    const char *str = getlngstr(ts);
    return internshrstr(L, str, len);
  }
}

