/*
** $Id: ltable.h $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#ifndef ltable_h
#define ltable_h

#include <pthread.h>
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
  { if (__atomic_load_n(&(t)->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED) \
      __atomic_fetch_and(&(t)->flags, cast_byte(~maskflags), __ATOMIC_RELAXED); \
    else settableflags(t, gettableflags(t) & cast_byte(~maskflags)); }


LUAI_DDEC(const Node luaH_dummynode;)

/* A record keeps its payloads inline and uses the unused array pointer word
** for tags. The low bit distinguishes tags from an aligned array pointer.
** The node field points to the constructor's shared immutable key array. */
#define LUA_RECORD_TAG cast(uintptr_t, 2)
#define LUA_RECORD_TAGS cast(uintptr_t, 1)
enum { LUA_RECORD_MAX_FIELDS = 4 };
_Static_assert(sizeof(uintptr_t) > LUA_RECORD_MAX_FIELDS, "record tag word");

l_sinline Table *luaH_recordkeys (const Node *image) {
  uintptr_t address = cast(uintptr_t, image);
  return address & LUA_RECORD_TAG
    ? cast(Table *, address & ~LUA_RECORD_TAG) : NULL;
}

l_sinline Value *luaH_recordvalues (const Table *table) {
  return cast(Value *, table + 1);
}

l_sinline lu_byte luaH_recordtag (uintptr_t tags, unsigned index) {
  return cast_byte(tags >> ((index + 1) * 8));
}

l_sinline void luaH_setrecordtag (Table *table, unsigned index, lu_byte tag) {
  unsigned shift = (index + 1) * 8;
  uintptr_t tags = (table->arraydata & ~(cast(uintptr_t, 255) << shift)) |
                   (cast(uintptr_t, tag) << shift);
  __atomic_store_n(&table->arraydata, tags, __ATOMIC_RELEASE);
}

/* The logical array is empty throughout the compact layout's lifetime. */
l_sinline Value *luaH_readarray (const Table *table) {
  uintptr_t data = __atomic_load_n(&table->arraydata, __ATOMIC_ACQUIRE);
  return (data & LUA_RECORD_TAGS) ? NULL : cast(Value *, data);
}

l_sinline Value *luaH_array (const Table *table) {
  lua_assert(!(table->arraydata & LUA_RECORD_TAGS));
  return cast(Value *, table->arraydata);
}

/* Ownership or exclusive collector access protects the record image. */
l_sinline lu_byte luaH_recordvalue (const Table *table, unsigned index,
                                    TValue *result) {
  lu_byte tag = luaH_recordtag(table->arraydata, index);
  if (!tagisempty(tag)) {
    val_(result) = luaH_recordvalues(table)[index];
    settt_(result, tag);
  }
  return tag;
}

#define isdummy(t) ((t)->node == &luaH_dummynode || luaH_recordkeys((t)->node))

/* The tag distinguishes a directory from a single node image without
** expanding every small table. Once installed, the directory stays put. */
#define LUA_HASH_PARTS 16
enum {
  LUA_HASH_WRITING = 1,
  LUA_HASH_WAITING = 2
};
typedef struct LuaHashPart {
  Node *nodes;
  Value *array;  /* independently sized positive integer slots */
  uint64_t version;  /* odd during structural or split-value changes */
  unsigned gate;
  pthread_cond_t changed;
  struct LuaWaiter *waiters;  /* address-sharded hash wait mutex */
} LuaHashPart;

typedef struct LuaHashDirectory {
  LuaHashPart parts[LUA_HASH_PARTS];
} LuaHashDirectory;

enum { LUA_NEXT_PROBE_LIMIT = 128 };

typedef struct LuaHashCursor {
  Node *nodes;
  Value *array;
  const LuaHashPart *part;
  uint64_t version;
  unsigned partition, index;
} LuaHashCursor;

l_sinline LuaHashDirectory *luaH_hashdir (const Node *image) {
  uintptr_t address = cast(uintptr_t, image);
  return address & 1 ? cast(LuaHashDirectory *, address & ~cast(uintptr_t, 1))
                     : NULL;
}

/* The directory is retained by the reader's storage epoch. Part zero uses
** the table layout and element writer versions, like an ordinary hash. */
l_sinline int luaH_cursorpart (LuaHashCursor *cursor,
                               const LuaHashDirectory *dir) {
  const LuaHashPart *part = &dir->parts[cursor->partition];
  cursor->part = cursor->partition != 0 ? part : NULL;
  cursor->version = cursor->part != NULL ?
    __atomic_load_n(&part->version, __ATOMIC_ACQUIRE) : 0;
  if (cursor->version & 1) return 0;
  cursor->nodes = __atomic_load_n(&part->nodes, __ATOMIC_ACQUIRE);
  cursor->array = cursor->part != NULL
    ? __atomic_load_n(&part->array, __ATOMIC_ACQUIRE) : NULL;
  return 1;
}

l_sinline unsigned luaH_nodesize (const Node *nodes) {
  return nodes == &luaH_dummynode || luaH_recordkeys(nodes) != NULL
    ? 0 : twoto(nodes->u.lsize);
}

l_sinline unsigned luaH_hashparts (const Table *table) {
  return luaH_hashdir(__atomic_load_n(&table->node, __ATOMIC_ACQUIRE))
         != NULL ? LUA_HASH_PARTS : 1;
}

l_sinline Node *luaH_hashnodes (const Table *table, unsigned index) {
  Node *image = __atomic_load_n(&table->node, __ATOMIC_ACQUIRE);
  LuaHashDirectory *directory = luaH_hashdir(image);
  if (luaH_recordkeys(image) != NULL) return cast(Node *, &luaH_dummynode);
  return directory == NULL ? image :
    __atomic_load_n(&directory->parts[index].nodes, __ATOMIC_ACQUIRE);
}

LUAI_FUNC l_mem luaH_tablework (const Table *);
LUAI_FUNC l_mem luaH_arrayslots (const Table *);



/* allocated size for hash nodes */
#define allocsizenode(t)	(isdummy(t) ? 0 : sizenode(t))


/* returns the Node, given the value of a table entry */
#define nodefromval(v)	cast(Node *, (v))



#define luaH_fastgeti(t,k,res,tag) \
  { tag = luaH_getint(L, t, k, res); }


/*
** 'luaH_get*' operations set 'res' and return its tag on a hit or a miss
** resolved with no metatable. Otherwise LUA_VABSTKEY leaves 'res' intact
** so ordinary indexing can resolve a metamethod using the original key.
*/


/*
** The array part of a table is represented by an inverted array of
** values followed by an array of tags, to avoid wasting space with
** padding. In between them there is an unsigned int, explained later.
** The 'array' pointer points between the two arrays, so that values are
** indexed with negative indices and tags with non-negative indices.

             Values                              Tags
  --------------------------------------------------------
  ...  |   Value 1     |   Value 0     | header |0|1|...
  --------------------------------------------------------
                                       ^ luaH_array(t)

** Slot access should use the macros 'getArrTag'
** and 'getArrVal'.
*/

typedef struct LuaArrayHeader {
  unsigned hint;  /* length search hint, under layout ownership */
  unsigned size;  /* immutable capacity of this backing allocation */
} LuaArrayHeader;

#define arrayheader(a) cast(LuaArrayHeader *, (a))
#define arraytag(a,k) (cast(lu_byte *, (a)) + sizeof(LuaArrayHeader) + (k))
#define arrayval(a,k) ((a) - 1 - (k))

/* Adjacent indices share a lane until the end of their block. Keep the
** mapping stable as either the array or hash in that lane grows. */
enum { LUA_ARRAY_BLOCK = 64 };
l_sinline unsigned luaH_partindex (unsigned index) {
  unsigned offset = index - 1;
  return offset / (LUA_ARRAY_BLOCK * (LUA_HASH_PARTS - 1)) * LUA_ARRAY_BLOCK +
         offset % LUA_ARRAY_BLOCK + 1;
}

l_sinline lua_Integer luaH_indexfrompart (unsigned part, unsigned index) {
  unsigned offset = index - 1;
  return cast(lua_Integer, offset / LUA_ARRAY_BLOCK) *
    (LUA_ARRAY_BLOCK * (LUA_HASH_PARTS - 1)) +
    (part - 1) * LUA_ARRAY_BLOCK + offset % LUA_ARRAY_BLOCK + 1;
}

l_sinline unsigned luaH_arraycapacity (const Value *array) {
  return array != NULL ? arrayheader(array)->size : 0;
}

/* Part zero is the original dense prefix. A partitioned table never moves
** an index between that prefix and secondary parts. */
l_sinline Value *luaH_arraypart (const Table *table, unsigned part) {
  if (part == 0) return luaH_array(table);
  return __atomic_load_n(&luaH_hashdir(table->node)->parts[part].array,
                         __ATOMIC_ACQUIRE);
}

/* Computes the address of the tag for the abstract C-index 'k' */
#define getArrTag(t,k)	arraytag(luaH_array(t), (k))

/* Computes the address of the value for the abstract C-index 'k' */
#define getArrVal(t,k)	arrayval(luaH_array(t), (k))

l_sinline unsigned luaH_recordindex (const Table *keys, const TString *key) {
  unsigned i;
  for (i = 0; i < keys->asize; i++)
    if (cast(const void *, getArrVal(keys, i)->gc) == key) return i;
  return keys->asize;
}

l_sinline unsigned luaH_nextrecordslot (const Table *keys, uintptr_t tags,
                                         unsigned index) {
  while (index < keys->asize && tagisempty(luaH_recordtag(tags, index)))
    index++;
  return index;
}

/* Large arrays carry one stamp per group of slots. Scalar cards have
** never held a reference in this image. Other cards are zero when dirty,
** or retain the major epoch in which a young scan proved them clean. */
enum { LUA_ARRAY_CARD_SLOTS = 64, LUA_ARRAY_CARD_MIN = 256 };
#define LUA_ARRAY_CARD_SCALAR UINT64_MAX
#define LUA_ARRAY_CARD_SCANNING (UINT64_MAX - 1)

l_sinline size_t luaH_cardoffset (unsigned size) {
  size_t bytes = sizeof(LuaArrayHeader) + cast_sizet(size);
  return (bytes + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1);
}

l_sinline unsigned luaH_arraycardcount (unsigned size) {
  return size / LUA_ARRAY_CARD_SLOTS + (size % LUA_ARRAY_CARD_SLOTS != 0);
}

l_sinline uint64_t *luaH_arraycards (const Table *t) {
  return cast(uint64_t *, cast_charp(luaH_array(t)) + luaH_cardoffset(t->asize));
}

l_sinline void luaH_dirtyarray (Table *t, unsigned index, const TValue *value) {
  if (t->asize >= LUA_ARRAY_CARD_MIN && iscollectable(value))
    __atomic_store_n(&luaH_arraycards(t)[index / LUA_ARRAY_CARD_SLOTS], 0,
                     __ATOMIC_RELEASE);
}


/*
** The header between the two arrays holds the hint for #t;
** see luaH_getn. It is stored there to avoid wasting space in
** the structure Table for tables with no array part.
*/
#define lenhint(t)	(&arrayheader(luaH_array(t))->hint)


/*
** Move TValues to/from arrays, using C indices
*/
#define arr2obj(h,k,val)  \
  ((val)->tt_ = *getArrTag(h,(k)), (val)->value_ = *getArrVal(h,(k)))

#define obj2arr(h,k,val)  \
  (__atomic_store(getArrVal(h,(k)), &(val)->value_, __ATOMIC_RELEASE), \
   __atomic_store_n(getArrTag(h,(k)), (val)->tt_, __ATOMIC_RELEASE), \
   luaH_dirtyarray(h, (k), val))


/*
** Often, we need to check the tag of a value before moving it. The
** following macros also move TValues to/from arrays, but receive the
** precomputed tag value or address as an extra argument.
*/
#define farr2val(h,k,tag,res)  \
  ((res)->tt_ = tag, (res)->value_ = *getArrVal(h,(k)))

#define fval2arr(h,k,tag,val)  \
  (__atomic_store(getArrVal(h,(k)), &(val)->value_, __ATOMIC_RELEASE), \
   __atomic_store_n((tag), (val)->tt_, __ATOMIC_RELEASE), \
   luaH_dirtyarray(h, (k), val))


LUAI_FUNC lu_byte luaH_get (lua_State *L, Table *t, const TValue *key, TValue *res);
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
/* Try a non-allocating write. Failure leaves the operation
** for ordinary rooted access; no encoded storage location escapes. */
LUAI_FUNC int luaH_tryset (lua_State *, Table *, const TValue *, TValue *, int);
LUAI_FUNC int luaH_trysetrecord (lua_State *, Table *, TString *, unsigned,
                                 TValue *);
LUAI_FUNC int luaH_setshared (lua_State *, Table *, const TValue *, TValue *, int);
/* Store the list values immediately above the rooted constructor register. */
LUAI_FUNC void luaH_setlist (lua_State *L, Table *t, StkId base,
                            unsigned count, unsigned last);

/* Requires object ownership or collector quiescence. */
LUAI_FUNC lu_byte luaH_getshortowned (Table *t, TString *key, TValue *result);

LUAI_FUNC void luaH_setint (lua_State *L, Table *t, lua_Integer key,
                                                    TValue *value);
LUAI_FUNC void luaH_set (lua_State *L, Table *t, const TValue *key,
                                                 TValue *value);

LUAI_FUNC Table *luaH_new (lua_State *L, unsigned asize, unsigned hsize);
LUAI_FUNC Table *luaH_newrecord (lua_State *, Table *);
#define LUA_TABLE_INLINE_LIMIT 192
LUAI_FUNC void luaH_resize (lua_State *L, Table *t, unsigned nasize,
                                                    unsigned nhsize);
LUAI_FUNC void luaH_resizearray (lua_State *L, Table *t, unsigned nasize);
LUAI_FUNC lu_mem luaH_size (const Table *owner, const Table *layout);
/* A dead table and its buffers can be freed by a sweep worker if all are
** pooled. Returning those blocks cannot enter the host allocator. */
LUAI_FUNC size_t luaH_pooledsize (Table *t);
LUAI_FUNC void luaH_freeparts (lua_State *L, Table *t, int private);
LUAI_FUNC l_mem luaH_freepooled (lua_State *L, Table *t);
LUAI_FUNC void luaH_free (lua_State *L, Table *t);
LUAI_FUNC void luaH_retire (lua_State *L, Table *t);
LUAI_FUNC int luaH_next (lua_State *L, Table *t, StkId key);
LUAI_FUNC int luaH_trynext (lua_State *L, Table *t, TValue *key, TValue *value);
LUAI_FUNC int luaH_nextlocked (lua_State *L, Table *t, StkId key);
LUAI_FUNC int luaH_hashresume (Table *t, const LuaHashDirectory *dir,
                               const TValue *key, uint64_t layout,
                               LuaHashCursor *cursor);
LUAI_FUNC lua_Unsigned luaH_getn (lua_State *L, Table *t);


#if defined(LUA_DEBUG)
LUAI_FUNC Node *luaH_mainposition (const Table *t, const TValue *key);
#endif

/* Collector snapshots borrow buffers under an admission storage epoch. Only
** validated chunks may be followed; captured pointers can otherwise be bits
** from a concurrently assigned numeric value. A NULL global state omits row
** checks; the caller must validate the sequence and still-private sharing. */
LUAI_FUNC unsigned luaH_tracechunk (struct global_State *, const Table *, unsigned, unsigned, int,
                                    GCObject **);
/* Nonparking snapshot. NULL L is reserved for collector reads of weak mode. */
LUAI_FUNC int luaH_readshortstr (struct global_State *, lua_State *, Table *,
                                TString *, TValue *);

#endif
