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
#include "ltablestorage.h"


/*
** Maximum size for string table.
*/
#define MAXSTRTB luaM_limitN(INT_MAX, TString*)

/* Low hash bits select a partition; the remaining bits select its slot. */
#define stringtableof(g,h) (&(g)->strt[(h) % LUA_STRING_SHARDS])
#define stringslot(h,n) lmod((h) / LUA_STRING_SHARDS, (n))

struct LuaStringIndex {
  unsigned size;
  TString *slots[];
};

static size_t indexbytes (unsigned size) {
  return sizeof(LuaStringIndex) + cast_sizet(size) * sizeof(TString *);
}

static LuaStringIndex *newindex (lua_State *L, unsigned size) {
  LuaStringIndex *image = luaM_alloccontrol(L, indexbytes(size));
  if (image != NULL) {
    image->size = size;
    memset(image->slots, 0, cast_sizet(size) * sizeof(TString *));
  }
  return image;
}

void luaS_freeindex (lua_State *L, LuaStringIndex *index) {
  if (index != NULL) luaM_freemem(L, index, indexbytes(index->size));
}

unsigned luaS_indexsize (const stringtable *table) {
  return __atomic_load_n(&table->index, __ATOMIC_ACQUIRE)->size;
}



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


unsigned luaS_hash (const char *str, size_t l, unsigned seed) {
  return hashbytes(str, l, seed ^ cast_uint(l));
}

unsigned luaS_computelonghash (TString *ts) {
  unsigned hash;
  if (__atomic_load_n(&ts->extra, __ATOMIC_RELAXED) & LSTR_ZEROHASH) return 0;
  lua_assert(ts->shrlen == LSTRREG && ts->lnglen <= LUA_NATIVE_WORK_SLICE);
  /* Both the seed and contents are immutable. Concurrent first users may
  ** compute the same hash without waiting or reading one another's input. */
  hash = luaS_hash(getlngstr(ts), ts->lnglen, ts->storage.seed);
  if (hash == 0) __atomic_fetch_or(&ts->extra, LSTR_ZEROHASH, __ATOMIC_RELAXED);
  else __atomic_store_n(&ts->hash, hash, __ATOMIC_RELAXED);
  return hash;
}

/* Lookup must be bounded even under table ownership. Hash larger strings
** before publication, while the constructor still owns the result. */
void luaS_finishlongstr (TString *ts) {
  unsigned seed = ts->hash;
  lua_assert(ts->tt == LUA_VLNGSTR);
  if (ts->shrlen == LSTRREG) {
    if (ts->lnglen <= LUA_NATIVE_WORK_SLICE) return;
    seed = ts->storage.seed;
  }
  ts->hash = luaS_hash(getlngstr(ts), ts->lnglen, seed);
  ts->extra = ts->hash == 0;
}


/* The result must remain rooted across polls. A source requests a copy;
** otherwise the caller has already filled the result in place. */
void luaS_buildlongstr (lua_State *L, TString *ts, const char *src,
                        unsigned *work) {
  size_t remaining = ts->lnglen;
  char *dest = getlngstr(ts);
  unsigned hash = ts->storage.seed ^ cast_uint(remaining);
  lua_assert(ts->tt == LUA_VLNGSTR && ts->shrlen == LSTRREG && ts->hash == 0);
  if (remaining <= LUA_NATIVE_WORK_SLICE) {
    if (src != NULL) {
      memcpy(dest, src, remaining);
      luaQ_workn(L, work, remaining);
    }
    return;
  }
  while (remaining != 0) {
    size_t chunk = remaining < 1024u ? remaining : 1024u;
    remaining -= chunk;
    if (src != NULL) memcpy(dest + remaining, src + remaining, chunk);
    hash = hashbytes(dest + remaining, chunk, hash);
    luaQ_workn(L, work, chunk);
  }
  ts->hash = hash;
  ts->extra = hash == 0;
}


/* Retire the old layout after its last optimistic reader leaves. */
static int resizeindex (lua_State *L, stringtable *tb, unsigned size,
                        int stopped) {
  LuaStringIndex *old, *image;
  LuaTableRetirement *retired = NULL;
  unsigned i;
  if (!stopped) {
    retired = luaM_realloc_(L, NULL, 0, sizeof(*retired));
    if (retired == NULL) return 0;
  }
  image = newindex(L, size);
  if (image == NULL) {
    if (retired != NULL) luaM_free(L, retired);
    return 0;
  }
  old = tb->index;
  for (i = 0; i < old->size; i++) {
    TString *string = old->slots[i];
    if (string != NULL) {
      unsigned slot = stringslot(string->hash, size);
      while (image->slots[slot] != NULL) slot = lmod(slot + 1, size);
      image->slots[slot] = string;
    }
  }
  __atomic_store_n(&tb->index, image, __ATOMIC_RELEASE);
  if (retired == NULL) luaS_freeindex(L, old);
  else {
    *retired = (LuaTableRetirement){
      .nodes = old, .node_bytes = indexbytes(old->size)
    };
    luaHS_publish(L, retired, 0);
  }
  return 1;
}

void luaS_shrink (lua_State *L) {
  global_State *g = G(L);
  unsigned i;
  for (i = 0; i < LUA_STRING_SHARDS; i++) {
    stringtable *tb = &g->strt[i];
    unsigned size = luaS_indexsize(tb);
    unsigned target = size;
    while (target > LUA_STRING_INITIAL_SLOTS && tb->nuse < target / 4)
      target /= 2;
    if (target == size) continue;
    /* A parked allocator can retain the shard across the stop. */
    if (luaA_lockowner(&tb->access) != NULL || !resizeindex(L, tb, target, 1))
      __atomic_store_n(&g->shrink_strings, 1, __ATOMIC_RELAXED);
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
         participant = participant->next) {
      LuaStringCache *cache = participant->thread->strcache;
      if (cache != NULL) cache->epoch = 0;
    }
  }
}


size_t luaS_sizelngstr (size_t len, int kind) {
  switch (kind) {
    case LSTRREG:  /* regular long string */
      /* don't need 'falloc'/'ud', but need space for content */
      return sizestrreg(len);
    case LSTRFIX:  /* fixed external long string */
      /* don't need 'falloc'/'ud' */
      return offsetof(TString, storage);
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
    tb->index = newindex(L, LUA_STRING_INITIAL_SLOTS);
    if (tb->index == NULL) luaM_error(L);
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


TString *luaS_createlngstrobj (lua_State *L, size_t l, int result) {
  TString *ts;
  GCObject *object;
  if (l_unlikely(l >= MAX_SIZE - sizeof(TString)))
    luaM_toobig(L);
  object = result ? luaC_newresultstring(L, sizestrreg(l))
                  : luaC_newobj(L, LUA_VLNGSTR, sizestrreg(l));
  ts = gco2ts(object);
  ts->hash = 0;
  ts->lnglen = l;
  ts->shrlen = LSTRREG;  /* signals that it is a regular long string */
  ts->storage.seed = G(L)->seed;
  ts->contents = cast_charp(ts) + REGULAR_STRING_HEADER;
  ts->contents[l] = '\0';  /* ending 0 */
  return ts;
}


void luaS_remove (lua_State *L, TString *ts) {
  stringtable *tb = stringtableof(G(L), ts->hash);
  LuaStringIndex *index = tb->index;
  unsigned size = index->size;
  unsigned hole = stringslot(ts->hash, size);
  unsigned slot;
  while (index->slots[hole] != ts) {
    lua_assert(index->slots[hole] != NULL);
    hole = lmod(hole + 1, size);
  }
  /* Sweep owns the shard and any optimistic reader joined the weak cutoff.
  ** Close the hole for every following key whose probe would cross it. */
  slot = lmod(hole + 1, size);
  while (index->slots[slot] != NULL) {
    TString *string = index->slots[slot];
    unsigned home = stringslot(string->hash, size);
    if (lmod(slot - home, size) >= lmod(slot - hole, size)) {
      __atomic_store_n(&index->slots[hole], string, __ATOMIC_RELEASE);
      hole = slot;
    }
    slot = lmod(slot + 1, size);
  }
  __atomic_store_n(&index->slots[hole], NULL, __ATOMIC_RELEASE);
  tb->nuse--;
  if (size > LUA_STRING_INITIAL_SLOTS && tb->nuse < size / 4 &&
      !__atomic_load_n(&G(L)->shrink_strings, __ATOMIC_RELAXED))
    __atomic_store_n(&G(L)->shrink_strings, 1, __ATOMIC_RELAXED);
}


l_sinline int matches (const TString *ts, const char *str, size_t len,
                       unsigned hash) {
  return ts->hash == hash &&
         len == cast_uint(ts->shrlen) && memcmp(str, getshrstr(ts), len) == 0;
}

/* Sweep starts at a stopped weak cutoff. Before that cutoff a borrowed
** string stays allocated until we have converted it to a strong reference.
** The caller holds managed admission through the lookup and return. */
static TString *tryinterned (lua_State *L, stringtable *tb,
                             const char *str, size_t len, unsigned hash) {
  LuaStringIndex *index;
  unsigned step, size, slot;
  if (luaC_sweeping(L) || luaW_iscollector(L)) return NULL;
  index = __atomic_load_n(&tb->index, __ATOMIC_ACQUIRE);
  size = index->size;
  slot = stringslot(hash, size);
  for (step = 0; step < 8 && step < size; step++) {
    TString *ts = __atomic_load_n(&index->slots[slot], __ATOMIC_ACQUIRE);
    if (ts == NULL) return NULL;
    if (matches(ts, str, len, hash)) {
      luaC_tracepointer(L, ts);
      return ts;
    }
    slot = lmod(slot + 1, size);
  }
  return NULL;
}

/* The shard is exclusive through allocation. Recompute the insertion slot
** afterward so it cannot refer to a retired image across a safepoint. */
static void insertshort (stringtable *tb, TString *ts) {
  LuaStringIndex *index = tb->index;
  unsigned size = index->size;
  unsigned slot = stringslot(ts->hash, size);
  while (index->slots[slot] != NULL)
    slot = lmod(slot + 1, size);
  __atomic_store_n(&index->slots[slot], ts, __ATOMIC_RELEASE);
  tb->nuse++;
}


static TString *internshrstr (lua_State *L, const char *str, size_t len) {
  TString *ts;
  global_State *g = G(L);
  unsigned hash, slot, step, size;
  stringtable *tb;
  LuaStringIndex *index;
  LuaObjectScope access;
  if (len == 0) return &g->empty_string;
  if (len == 1) return &g->byte_strings[cast_byte(*str)];
  hash = luaS_hash(str, len, g->seed);
  tb = stringtableof(g, hash);
  ts = tryinterned(L, tb, str, len, hash);
  if (ts != NULL) return ts;
  luaA_beginlock(L, &tb->access, &access);
  index = tb->index;
  size = index->size;
  slot = stringslot(hash, size);
  for (step = 0; step < size; step++) {
    ts = index->slots[slot];
    if (ts == NULL) break;
    if (matches(ts, str, len, hash)) {
      if (isdead(g, ts)) luaC_revive(g, obj2gco(ts));
      luaC_tracepointer(L, ts);
      luaA_end(L, &access);
      return ts;
    }
    slot = lmod(slot + 1, size);
  }
  if (tb->nuse >= size - size / 4) {
    if (size <= MAXSTRTB / 2)
      resizeindex(L, tb, size * 2, 0);
    if (l_unlikely(tb->nuse == tb->index->size - 1)) luaM_error(L);
  }
  ts = createstrobj(L, sizestrshr(len), LUA_VSHRSTR, hash);
  copyshrstr(ts, str, len);
  insertshort(tb, ts);
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
    TString *ts = luaS_createlngstrobj(L, l, 0);
    memcpy(getlngstr(ts), str, l * sizeof(char));
    luaS_finishlongstr(ts);
    return ts;
  }
}


/* A set may contain results from either string constructor. The tag lets
** a C-string lookup use strcmp without accepting a cached binary prefix. */
enum { CACHE_BINARY = 1 };
_Static_assert(_Alignof(TString) > CACHE_BINARY, "string cache tag alignment");

/* Sample the bytes to match short results from different source buffers.
** The lookup always checks the complete length and contents. */
l_sinline unsigned shortstringset (const char *str, size_t len) {
  return (cast_uint(len) * 23u + cast_byte(str[0]) * 19u +
          cast_byte(str[len / 2]) * 7u + cast_byte(str[len - 1])) % STRCACHE_N;
}

static uintptr_t *stringcacherow (lua_State *L, unsigned set) {
  LuaStringCache *cache = L->strcache;
  if (l_unlikely(cache == NULL)) {
    cache = cast(LuaStringCache *, luaM_tryalloc(L, sizeof(*cache)));
    if (cache == NULL) return NULL;
    cache->epoch = 0;
    L->strcache = cache;
  }
  if (l_unlikely(cache->epoch != G(L)->strcache_epoch)) {
    memset(cache->entries, 0, sizeof(cache->entries));
    cache->epoch = G(L)->strcache_epoch;
  }
  return cache->entries[set];
}

l_sinline void rememberstring (uintptr_t *row, TString *str, int binary) {
  int j;
  /* An interning miss may have crossed a collection. The current epoch
  ** remains invalid until the next row lookup clears the old entries. */
  if (row != NULL) {
    for (j = STRCACHE_M - 1; j > 0; j--)
      row[j] = row[j - 1];
    row[0] = cast(uintptr_t, str) | cast_uint(binary);
  }
}

l_sinline TString *cachedbytes (const uintptr_t *row, const char *str,
                                size_t len) {
  int j;
  for (j = 0; j < STRCACHE_M; j++) {
    TString *ts = cast(TString *, row[j] & ~cast(uintptr_t, CACHE_BINARY));
    if (ts != NULL && tsslen(ts) == len && memcmp(getstr(ts), str, len) == 0)
      return ts;
  }
  return NULL;
}

/* A bounded stack claim joins the cutoff that invalidates weak entries
** before sweep. The caller only uses this path outside concurrent marking. */
TString *luaS_trycachedshort (lua_State *L, const char *str, size_t len) {
  const LuaStringCache *cache = L->strcache;
  global_State *g = G(L);
  lua_assert(len <= LUAI_MAXSHORTLEN);
  if (len == 0) return &g->empty_string;
  if (len == 1) return &g->byte_strings[cast_byte(*str)];
  if (cache == NULL || cache->epoch != g->strcache_epoch) return NULL;
  return cachedbytes(cache->entries[shortstringset(str, len)], str, len);
}

TString *luaS_cachelstr (lua_State *L, const char *str, size_t len) {
  uintptr_t *row;
  TString *result;
  if (len <= 1 || len > LUAI_MAXSHORTLEN) return luaS_newlstr(L, str, len);
  row = stringcacherow(L, shortstringset(str, len));
  if (row != NULL && (result = cachedbytes(row, str, len)) != NULL) {
    luaC_tracepointer(L, result);
    return result;
  }
  result = luaS_newlstr(L, str, len);
  rememberstring(row, result, memchr(getshrstr(result), '\0', len) != NULL);
  return result;
}

l_sinline TString *cachedname (const uintptr_t *row, const char *str,
                                int shortonly) {
  int j;
  for (j = 0; j < STRCACHE_M; j++) {
    uintptr_t entry = row[j];
    TString *ts = cast(TString *, entry & ~cast(uintptr_t, CACHE_BINARY));
    if (ts != NULL && !(entry & CACHE_BINARY) &&
        (!shortonly || strisshr(ts)) && strcmp(str, getstr(ts)) == 0)
      return ts;
  }
  return NULL;
}

/* The same stack claim as luaS_trycachedshort protects the weak entries. */
TString *luaS_trycachedname (lua_State *L, const char *str) {
  const LuaStringCache *cache = L->strcache;
  if (cache == NULL || cache->epoch != G(L)->strcache_epoch) return NULL;
  return cachedname(cache->entries[point2uint(str) % STRCACHE_N], str, 1);
}

TString *luaS_new (lua_State *L, const char *str) {
  uintptr_t *row = stringcacherow(L, point2uint(str) % STRCACHE_N);
  TString *result;
  if (row != NULL && (result = cachedname(row, str, 0)) != NULL) {
    luaC_tracepointer(L, result);
    return result;
  }
  result = luaS_newlstr(L, str, strlen(str));
  rememberstring(row, result, 0);
  return result;
}


Udata *luaS_newudata (lua_State *L, size_t s, unsigned short nuvalue) {
  Udata *u;
  int i;
  GCObject *o;
  if (l_unlikely(s > MAX_SIZE - udatamemoffset(nuvalue)))
    luaM_toobig(L);
  o = luaC_newuserdata(L, sizeudata(nuvalue, s), nuvalue <= 1);
  u = gco2u(o);
  u->len = s;
  luaA_initlock(&u->access);
  luaA_initlock(&u->payload_access);
  luaA_initsharing(L, u);
  if (nuvalue != 0) u->sequence = 0;
  u->nuvalue = nuvalue;
  u->metatable = NULL;
  u->metatable_version = 0;
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
    ne.ts->storage.falloc = falloc;
    ne.ts->ud = ud;
  }
  ne.ts->shrlen = ne.kind;
  ne.ts->lnglen = len;
  ne.ts->contents = cast_charp(s);
  luaS_finishlongstr(ne.ts);
  return ne.ts;
}


/*
** Normalize an external string: If it is short, internalize it.
*/
TString *luaS_normstr (lua_State *L, TString *ts) {
  size_t len = ts->lnglen;
  if (len > LUAI_MAXSHORTLEN)
    return ts;  /* long string; keep the original */
  else {
    const char *str = getlngstr(ts);
    return internshrstr(L, str, len);
  }
}

