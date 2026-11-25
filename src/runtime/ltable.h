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

/* Writer groups only help the next layout owner find outstanding claims.
** Readers still validate the layout generation and split-value flag. */
l_sinline int luaH_samelayout (const Table *table, uint64_t layout) {
  uint64_t current = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
  return ((current ^ layout) & ~cast(uint64_t, LUA_TABLE_WRITER_GROUP_MASK)) == 0;
}

/* The node field points to the constructor's immutable key array. The array
** word holds either inline tags or a pointer to tags and full-width slots.
** Both forms keep the low bit set, unlike an ordinary array pointer. */
#define LUA_RECORD_TAG cast(uintptr_t, 2)
#define LUA_RECORD_TAGS cast(uintptr_t, 1)
#define LUA_RECORD_BACKING cast(uintptr_t, 2)
/* The packed marker is outside the Lua tag range and carries no GC edge. */
enum { LUA_RECORD_MAX_FIELDS = 4, LUA_RECORD_PACKED = 0x3f,
       LUA_RECORD_WIDE = 0x3e, LUA_RECORD_WIDE_FROZEN = 0x3d };
#define LUA_RECORD_FROZEN UINT64_MAX
_Static_assert(sizeof(uintptr_t) > LUA_RECORD_MAX_FIELDS, "record tag word");

/* The low byte is the Lua tag. Bit 8 reserves the payload for a writer.
** Each publication advances the version above those bits. */
enum { LUA_RECORD_BUSY = 1 << 8, LUA_RECORD_VERSION_STEP = 1 << 9 };

typedef struct LuaRecordField {
  Value payload;
  uint64_t stamp;
} LuaRecordField;

typedef struct LuaRecordStorage {
  uintptr_t tags;
  LuaRecordField fields[LUA_RECORD_MAX_FIELDS];
} LuaRecordStorage;

l_sinline LuaRecordStorage *luaH_recordbacking (uintptr_t image) {
  uintptr_t flags = LUA_RECORD_TAGS | LUA_RECORD_BACKING;
  return (image & flags) == flags ? cast(LuaRecordStorage *, image & ~flags) : NULL;
}

l_sinline uintptr_t luaH_recordtags (uintptr_t image) {
  LuaRecordStorage *backing = luaH_recordbacking(image);
  return backing != NULL ? __atomic_load_n(&backing->tags, __ATOMIC_ACQUIRE) : image;
}

l_sinline Table *luaH_recordkeys (const Node *image) {
  uintptr_t address = cast(uintptr_t, image);
  return address & LUA_RECORD_TAG
    ? cast(Table *, address & ~LUA_RECORD_TAG) : NULL;
}

l_sinline Value *luaH_inlinerecordvalues (const Table *table) {
  return cast(Value *, table + 1);
}

l_sinline lu_byte luaH_unpackwide (LuaRecordField field, TValue *result) {
  lu_byte tag = cast_byte(field.stamp);
  if (!tagisempty(tag)) {
    val_(result) = field.payload;
    settt_(result, tag);
  }
  return tag;
}

l_sinline int luaH_tryloadwide (const LuaRecordField *slot,
                                  LuaRecordField *field) {
  uint64_t stamp = __atomic_load_n(&slot->stamp, __ATOMIC_ACQUIRE);
  if (stamp & LUA_RECORD_BUSY) return 0;
  field->stamp = stamp;
  /* Observing a newer payload also orders the second stamp load after
  ** the reservation that preceded that payload's release store. */
  __atomic_load(&slot->payload, &field->payload, __ATOMIC_ACQUIRE);
  return __atomic_load_n(&slot->stamp, __ATOMIC_ACQUIRE) == stamp;
}

/* Layout ownership prevents new fast writers. A previously admitted
** reservation has only the two publication stores left to perform. */
l_sinline LuaRecordField luaH_loadownedwide (const LuaRecordField *slot) {
  LuaRecordField field;
  while (!luaH_tryloadwide(slot, &field)) __builtin_ia32_pause();
  return field;
}

l_sinline int luaH_replacewide (LuaRecordField *slot, uint64_t stamp,
                                  lu_byte tag, uint64_t bits) {
  uint64_t published = (stamp & ~UINT64_C(255)) + LUA_RECORD_VERSION_STEP;
  lua_assert(!(stamp & LUA_RECORD_BUSY));
  if (!__atomic_compare_exchange_n(&slot->stamp, &stamp,
          stamp | LUA_RECORD_BUSY, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return 0;
  __atomic_store_n(&slot->payload.bits, bits, __ATOMIC_RELEASE);
  __atomic_store_n(&slot->stamp, published | tag, __ATOMIC_RELEASE);
  return 1;
}

l_sinline LuaRecordField luaH_exchangewide (LuaRecordField *slot, lu_byte tag,
                                             uint64_t bits) {
  LuaRecordField old;
  do { old = luaH_loadownedwide(slot); }
  while (!luaH_replacewide(slot, old.stamp, tag, bits));
  return old;
}

l_sinline lu_byte luaH_recordtag (uintptr_t tags, unsigned index) {
  return cast_byte(tags >> ((index + 1) * 8));
}

l_sinline void luaH_setrecordtag (uintptr_t *word, unsigned index, lu_byte tag) {
  unsigned shift = (index + 1) * 8;
  uintptr_t tags = (*word & ~(cast(uintptr_t, 255) << shift)) |
                   (cast(uintptr_t, tag) << shift);
  __atomic_store_n(word, tags, __ATOMIC_RELEASE);
}

/* The low byte holds the Lua tag. The high 56 bits hold a signed integer
** or zero for values whose tag fully describes them. Packed slots never
** contain GC references, and their inline storage cannot be reused after
** materialization because a reader or writer may retain its address. */
l_sinline uint64_t luaH_packrecord (const TValue *value) {
  lu_byte tag = rawtt(value);
  if (tag == LUA_VNUMINT) {
    uint64_t shifted = l_castS2U(ivalue(value)) + (UINT64_C(1) << 55);
    if (shifted < (UINT64_C(1) << 56))
      return (shifted << 8) | LUA_VNUMINT;
  }
  else if (tag == LUA_VTRUE || tag == LUA_VFALSE || tagisempty(tag))
    return tag == LUA_VNIL ? LUA_VEMPTY : tag;
  return LUA_RECORD_FROZEN;
}

l_sinline lu_byte luaH_unpackrecord (uint64_t bits, TValue *result) {
  lu_byte tag = cast_byte(bits);
  lua_assert(bits != LUA_RECORD_FROZEN);
  val_(result).i = l_castU2S((bits >> 8) - (UINT64_C(1) << 55));
  settt_(result, tag);
  return tag;
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

l_sinline lu_byte luaH_recordlocalvalue (const Value *payload, lu_byte tag,
                                         TValue *result) {
  if (!tagisempty(tag)) {
    val_(result) = *payload;
    settt_(result, tag);
  }
  return tag;
}

/* Ownership protects the image. Published scalar slots remain atomic even
** under ownership because an earlier writer may still be finishing. */
l_sinline lu_byte luaH_ownedrecordvalue (const Table *table, uintptr_t image,
                                         unsigned index, lu_byte tag,
                                         TValue *result) {
  if (tag == LUA_RECORD_PACKED) {
    const Value *payload = luaH_inlinerecordvalues(table) + index;
    return luaH_unpackrecord(__atomic_load_n(&payload->bits, __ATOMIC_ACQUIRE),
                             result);
  }
  if (tag == LUA_RECORD_WIDE) {
    const LuaRecordStorage *backing = luaH_recordbacking(image);
    return luaH_unpackwide(luaH_loadownedwide(&backing->fields[index]), result);
  }
  return luaH_recordlocalvalue(luaH_inlinerecordvalues(table) + index, tag,
                                result);
}

l_sinline lu_byte luaH_recordvalue (const Table *table, unsigned index,
                                    TValue *result) {
  uintptr_t image = table->arraydata;
  return luaH_ownedrecordvalue(table, image, index,
                                 luaH_recordtag(luaH_recordtags(image), index),
                                 result);
}

#define isdummy(t) ((t)->node == &luaH_dummynode || luaH_recordkeys((t)->node))

/* The tag distinguishes a directory from a single node image without
** expanding every small table. Once installed, the directory stays put. */
#define LUA_HASH_PARTS 16
enum {
  LUA_HASH_WRITING = 1,
  LUA_HASH_WAITING = 2
};
/* Growth appends blocks without moving published slots. A retained directory
** keeps its original bound. A type change can replace one integer block. */
typedef struct LuaPartArray {
  unsigned size;
  Value *blocks[];
} LuaPartArray;

typedef struct LuaHashPart {
  Node *nodes;
  LuaPartArray *array;
  uint64_t version;  /* odd during structural or split-value changes */
  uint64_t block_version;  /* advances when a published block changes type */
  unsigned gate;
  unsigned firsthashindex;  /* lower bound on the array indices in the hash */
  unsigned general_array;  /* an array assignment needed the full value format */
  pthread_cond_t changed;
  struct LuaWaiter *waiters;  /* address-sharded hash wait mutex */
} LuaHashPart;

typedef struct LuaHashDirectory {
  LuaHashPart parts[LUA_HASH_PARTS - 1];
} LuaHashDirectory;

/* Index zero denotes the flat array prefix and has no directory entry. */
#define luaH_part(dir, index) (&(dir)->parts[(index) - 1])

enum { LUA_NEXT_PROBE_LIMIT = 128 };

typedef struct LuaHashCursor {
  Node *nodes;
  const LuaPartArray *array;
  const LuaHashPart *part;
  uint64_t version;
  unsigned partition, index;
} LuaHashCursor;

l_sinline LuaHashDirectory *luaH_hashdir (const Node *image) {
  uintptr_t address = cast(uintptr_t, image);
  return address & 1 ? cast(LuaHashDirectory *, address & ~cast(uintptr_t, 1))
                     : NULL;
}

/* The directory is retained by the reader's storage epoch. */
l_sinline int luaH_cursorpart (LuaHashCursor *cursor,
                               const LuaHashDirectory *dir) {
  const LuaHashPart *part = luaH_part(dir, cursor->partition);
  cursor->part = part;
  cursor->version = __atomic_load_n(&part->version, __ATOMIC_ACQUIRE);
  if (cursor->version & 1) return 0;
  cursor->nodes = __atomic_load_n(&part->nodes, __ATOMIC_ACQUIRE);
  cursor->array = __atomic_load_n(&part->array, __ATOMIC_ACQUIRE);
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
  if (luaH_recordkeys(image) != NULL || (directory != NULL && index == 0))
    return cast(Node *, &luaH_dummynode);
  return directory == NULL ? image :
    __atomic_load_n(&luaH_part(directory, index)->nodes, __ATOMIC_ACQUIRE);
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
  unsigned hint;  /* flat length hint, or immutable integer block flag */
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

l_sinline const LuaPartArray *luaH_partarray (const Table *table, unsigned part) {
  return __atomic_load_n(&luaH_part(luaH_hashdir(table->node), part)->array,
                         __ATOMIC_ACQUIRE);
}

l_sinline unsigned luaH_partcapacity (const LuaPartArray *array) {
  return array != NULL ? array->size : 0;
}

/* Start with geometric blocks. Cap a block so a representation change
** never copies an unbounded number of slots under layout ownership. */
enum { LUA_PART_BLOCK_SHIFT = 8, LUA_PART_BLOCK_SIZE = 1 << LUA_PART_BLOCK_SHIFT };
l_sinline unsigned luaH_partblock (unsigned index) {
  if (index >= LUA_PART_BLOCK_SIZE)
    return (index >> LUA_PART_BLOCK_SHIFT) + LUA_PART_BLOCK_SHIFT;
  return index != 0 ? l_numbits(unsigned) - __builtin_clz(index) : 0;
}

l_sinline unsigned luaH_partblockstart (unsigned block) {
  if (block > LUA_PART_BLOCK_SHIFT)
    return (block - LUA_PART_BLOCK_SHIFT) << LUA_PART_BLOCK_SHIFT;
  return block != 0 ? 1u << (block - 1) : 0;
}

l_sinline unsigned luaH_partblocksize (unsigned block) {
  return block > LUA_PART_BLOCK_SHIFT ? LUA_PART_BLOCK_SIZE :
    block != 0 ? luaH_partblockstart(block) : 1;
}

l_sinline unsigned luaH_partblocks (unsigned capacity) {
  return capacity != 0 ? luaH_partblock(capacity - 1) + 1 : 0;
}

l_sinline Value *luaH_partblockview (const LuaPartArray *array, unsigned block) {
  return __atomic_load_n(&array->blocks[block], __ATOMIC_ACQUIRE);
}

l_sinline Value *luaH_partslot (const LuaPartArray *array, unsigned *index) {
  unsigned block = luaH_partblock(*index);
  lua_assert(*index < array->size);
  *index -= luaH_partblockstart(block);
  return luaH_partblockview(array, block);
}

/* The integer format uses the existing payload lane as the whole value.
** The ordinary tag lane remains empty, so it never supplies collector edges. */
#define LUA_PART_EMPTY LUA_MININTEGER
#define LUA_PART_FROZEN (LUA_MININTEGER + 1)

l_sinline int luaH_partinteger (const Value *block) {
  return arrayheader(block)->hint != 0;
}

l_sinline int luaH_partintfits (const TValue *value) {
  return ttisinteger(value) && ivalue(value) > LUA_PART_FROZEN;
}

l_sinline lua_Integer luaH_partintload (const Value *cell) {
  return __atomic_load_n(&cell->i, __ATOMIC_ACQUIRE);
}

/* Keep occupancy updates local to a range of slots. A growth heuristic can
** sample the counters while existing slots are being filled or cleared. */
enum { LUA_PART_COUNT_SLOTS = 64 };
l_sinline unsigned luaH_partcounts (unsigned size) {
  return (size + LUA_PART_COUNT_SLOTS - 1) / LUA_PART_COUNT_SLOTS;
}

l_sinline size_t luaH_partcountoffset (unsigned size) {
  return (sizeof(LuaArrayHeader) + size + sizeof(unsigned) - 1) &
         ~(sizeof(unsigned) - 1);
}

l_sinline unsigned *luaH_partcountbase (const Value *array, unsigned size) {
  return cast(unsigned *, cast_charp(array) + luaH_partcountoffset(size));
}

l_sinline unsigned *luaH_partcount (const Value *array, unsigned index) {
  return luaH_partcountbase(array, luaH_arraycapacity(array)) +
         index / LUA_PART_COUNT_SLOTS;
}

/* Part zero remains the original flat prefix. */
l_sinline Value *luaH_arrayblock (const Table *table, unsigned part,
                                  unsigned block) {
  const LuaPartArray *array;
  if (part == 0) return block == 0 ? luaH_array(table) : NULL;
  array = luaH_partarray(table, part);
  return block < luaH_partblocks(luaH_partcapacity(array))
    ? luaH_partblockview(array, block) : NULL;
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
LUAI_FUNC int luaH_trysetfield (lua_State *, Table *, const TValue *, TValue *, int);
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

#define luaH_new(L,asize,hsize) luaH_newtable(L, asize, hsize, 1)
LUAI_FUNC Table *luaH_newtable (lua_State *, unsigned, unsigned, int);
LUAI_FUNC Table *luaH_newrecord (lua_State *, Table *, int);
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
