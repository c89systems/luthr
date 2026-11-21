/*
** $Id: ltable.c $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#define ltable_c
#define LUA_CORE

#include "lprefix.h"


/*
** Implementation of tables (aka arrays, objects, or hash tables).
** Tables keep its elements in two parts: an array part and a hash part.
** Non-negative integer keys are all candidates to be kept in the array
** part. The actual size of the array is the largest 'n' such that
** more than half the slots between 1 and n are in use.
** Hash uses a mix of chained scatter table with Brent's variation.
** A main invariant of these tables is that, if an element is not
** in its main position (i.e. the 'original' position that its hash gives
** to it), then the colliding element is in its own main position.
** Hence even when the load factor reaches 100%, performance remains good.
*/

#include <math.h>
#include <limits.h>
#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "laccess.h"
#include "lroots.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lmempage.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltableaccess.h"
#include "ltablestorage.h"
#include "lvm.h"


/*
** Only hash parts with at least 2^LIMFORLAST have a 'lastfree' field
** that optimizes finding a free slot. That field is stored just before
** the array of nodes, in the same block. Smaller tables do a complete
** search when looking for a free slot.
*/
#define LIMFORLAST    3  /* log2 of real limit (8) */

/*
** The union 'Limbox' stores 'lastfree' and ensures that what follows it
** is properly aligned to store a Node.
*/
typedef struct { Node *dummy; Node follows_pNode; } Limbox_aux;

typedef union {
  Node *lastfree;
  char padding[offsetof(Limbox_aux, follows_pNode)];
} Limbox;

#define haslastfree(nodes)     ((nodes)->u.lsize >= LIMFORLAST)
#define getlastfree(nodes)     ((cast(Limbox *, (nodes)) - 1)->lastfree)


/*
** MAXABITS is the largest integer such that 2^MAXABITS fits in an
** unsigned int.
*/
#define MAXABITS	(l_numbits(int) - 1)


/*
** MAXASIZEB is the maximum number of elements in the array part such
** that the size of the array fits in 'size_t'.
*/
#define MAXASIZEB	(MAX_SIZET/(sizeof(Value) + 1))


/*
** MAXASIZE is the maximum size of the array part. It is the minimum
** between 2^MAXABITS and MAXASIZEB.
*/
#define MAXASIZE  \
    (((1u << MAXABITS) < MAXASIZEB) ? (1u << MAXABITS) : cast_uint(MAXASIZEB))

/*
** MAXHBITS is the largest integer such that 2^MAXHBITS fits in a
** signed int.
*/
#define MAXHBITS	(MAXABITS - 1)


/*
** MAXHSIZE is the maximum size of the hash part. It is the minimum
** between 2^MAXHBITS and the maximum size such that, measured in bytes,
** it fits in a 'size_t'.
*/
#define MAXHSIZE	luaM_limitN(1 << MAXHBITS, Node)
#define HASH_PARTITION_THRESHOLD 256


/*
** When the original hash value is good, hashing by a power of 2
** avoids the cost of '%'.
*/
#define hashstr(nodes,str)	((nodes) + lmod((str)->hash, nodesteps(nodes)))

/*
** for other types, it is better to avoid modulo by power of 2, as
** they can have many 2 factors.
*/
#define hashmod(size,n)	((n) % (((size) - 1u) | 1u))


/*
** Common hash part for tables with empty hash parts. That allows all
** tables to have a hash part, avoiding an extra check ("is there a hash
** part?") when indexing. Its sole node has an empty value and a key
** (DEADKEY, NULL) that is different from any valid TValue.
*/
const Node luaH_dummynode = {
  .u = { .tt_ = LUA_VEMPTY, .key_tt = LUA_TDEADKEY }
};


static const TValue absentkey = {ABSTKEYCONSTANT};


/*
** Negative integers are distributed across hash partitions. A second
** remainder can share factors with partition selection and put consecutive
** keys on one chain. Use a power-of-two mask and mix the high bits into it.
** The high-bit term is constant over a run of adjacent low bits.
*/
l_sinline unsigned hashint (unsigned size, lua_Integer i) {
  lua_Unsigned ui = l_castS2U(i);
  if (i < 0) {
    uint64_t bits = cast(uint64_t, ui);
    uint64_t high = (bits >> 16) * UINT64_C(0x9e3779b97f4a7c15);
    return lmod(cast_uint(bits ^ (high >> 32)), size);
  }
  if (ui <= cast_uint(INT_MAX)) return hashmod(size, cast_uint(ui));
  return cast_uint(hashmod(size, ui));
}


/*
** Hash for floating-point numbers.
** The main computation should be just
**     n = frexp(n, &i); return (n * INT_MAX) + i
** but there are some numerical subtleties.
** In a two-complement representation, INT_MAX may not have an exact
** representation as a float, but INT_MIN does; because the absolute
** value of 'frexp' is smaller than 1 (unless 'n' is inf/NaN), the
** absolute value of the product 'frexp * -INT_MIN' is smaller or equal
** to INT_MAX. Next, the use of 'unsigned int' avoids overflows when
** adding 'i'; the use of '~u' (instead of '-u') avoids problems with
** INT_MIN.
*/
#if !defined(l_hashfloat)
static unsigned l_hashfloat (lua_Number n) {
  int i;
  lua_Integer ni;
  n = l_mathop(frexp)(n, &i) * -cast_num(INT_MIN);
  if (!lua_numbertointeger(n, &ni)) {  /* is 'n' inf/-inf/NaN? */
    lua_assert(luai_numisnan(n) || l_mathop(fabs)(n) == cast_num(HUGE_VAL));
    return 0;
  }
  else {  /* normal case */
    unsigned int u = cast_uint(i) + cast_uint(ni);
    return (u <= cast_uint(INT_MAX) ? u : ~u);
  }
}
#endif


/*
** returns the 'main' position of an element in a table (that is,
** the index of its hash value).
*/
static unsigned mainindex (unsigned size, const TValue *key) {
  switch (ttypetag(key)) {
    case LUA_VNUMINT: return hashint(size, ivalue(key));
    case LUA_VNUMFLT: return hashmod(size, l_hashfloat(fltvalue(key)));
    case LUA_VSHRSTR: return lmod(tsvalue(key)->hash, size);
    case LUA_VLNGSTR: return lmod(luaS_hashlongstr(tsvalue(key)), size);
    case LUA_VFALSE: return 0;
    case LUA_VTRUE: return lmod(1u, size);
    case LUA_VLIGHTUSERDATA: return hashmod(size, point2uint(pvalue(key)));
    case LUA_VLCF: case LUA_VMCF: case LUA_VBCF:
      return hashmod(size, point2uint(fvalue(key)));
    default: return hashmod(size, point2uint(gcvalue(key)));
  }
}


l_sinline unsigned nodesteps (const Node *nodes) {
  return twoto(nodes->u.lsize);  /* one entry in the dummy image */
}

l_sinline Node *mainpositionTV (Node *nodes, const TValue *key) {
  return nodes + mainindex(nodesteps(nodes), key);
}

l_sinline Node *mainpositionfromnode (Node *nodes, Node *nd) {
  TValue key;
  getnodekey(cast(lua_State *, NULL), &key, nd);
  return mainpositionTV(nodes, &key);
}

/* All positive integer keys use part zero, including keys currently outside
** the array. Other keys can grow independently of array density changes. */
l_sinline unsigned distributedpart (lua_Unsigned bits) {
  return 1 + cast_uint((bits ^ (bits >> 16)) % (LUA_HASH_PARTS - 1));
}

l_sinline unsigned integerpart (lua_Integer integer) {
  return integer > 0 ? 0 : distributedpart(l_castS2U(integer));
}

static unsigned hashpart (const TValue *key) {
  lua_Integer integer;
  lua_Unsigned bits;
  switch (ttypetag(key)) {
    case LUA_VNIL: return 0;
    case LUA_VNUMINT: return integerpart(ivalue(key));
    case LUA_VNUMFLT:
      if (luaV_flttointeger(fltvalue(key), &integer, F2Ieq))
        return integerpart(integer);
      bits = l_hashfloat(fltvalue(key));
      break;
    case LUA_VSHRSTR: case LUA_VLNGSTR: bits = tsvalue(key)->hash; break;
    case LUA_VFALSE: bits = 0; break;
    case LUA_VTRUE: bits = 1; break;
    case LUA_VLIGHTUSERDATA: bits = point2uint(pvalue(key)); break;
    case LUA_VLCF: case LUA_VMCF: case LUA_VBCF:
      bits = point2uint(fvalue(key));
      break;
    default: bits = point2uint(gcvalue(key)); break;
  }
  return distributedpart(bits);
}

l_sinline Node *keynodes (const Table *table, const TValue *key) {
  Node *image = __atomic_load_n(&table->node, __ATOMIC_RELAXED);
  LuaHashDirectory *directory = luaH_hashdir(image);
  return directory == NULL ? image : directory->parts[hashpart(key)].nodes;
}


/*
** Check whether key 'k1' is equal to the key in node 'n2'. This
** equality is raw, so there are no metamethods. Floats with integer
** values have been normalized, so integers cannot be equal to
** floats. It is assumed that 'eqshrstr' is simply pointer equality,
** so that short strings are handled in the default case.  The flag
** 'deadok' means to accept dead keys as equal to their original values.
** (Only collectable objects can produce dead keys.) Note that dead
** long strings are also compared by identity.  Once a key is dead,
** its corresponding value may be collected, and then another value
** can be created with the same address. If this other value is given
** to 'next', 'equalkey' will signal a false positive. In a regular
** traversal, this situation should never happen, as all keys given to
** 'next' came from the table itself, and therefore could not have been
** collected. Outside a regular traversal, we have garbage in, garbage
** out. What is relevant is that this false positive does not break
** anything.  (In particular, 'next' will return some other valid item
** on the table or nil.)
*/
static int equalkeybits (const TValue *k1, lu_byte tag, Value bits, int deadok) {
  if (rawtt(k1) != tag) {  /* not the same variants? */
    if (tag == ctb(LUA_VSHRSTR) && ttislngstring(k1)) {
      /* an external string can be equal to a short-string key */
      return luaS_eqstr(tsvalue(k1), gco2ts(gcvalueraw(bits)));
    }
    else if (tagislcf(tag) && ttislcf(k1))
      return fvalue(k1) == fvalueraw(bits);
    else if (deadok && tag == LUA_TDEADKEY && iscollectable(k1)) {
      /* a collectable value can be equal to a dead key */
      return gcvalue(k1) == gcvalueraw(bits);
   }
   else
     return 0;  /* otherwise, different variants cannot be equal */
  }
  else {  /* equal variants */
    switch (tag) {
      case LUA_VNIL: case LUA_VFALSE: case LUA_VTRUE:
        return 1;
      case LUA_VNUMINT:
        return ivalue(k1) == ivalueraw(bits);
      case LUA_VNUMFLT:
        return luai_numeq(fltvalue(k1), fltvalueraw(bits));
      case LUA_VLIGHTUSERDATA:
        return pvalue(k1) == pvalueraw(bits);
      case LUA_VLCF: case LUA_VMCF: case LUA_VBCF:
        return fvalue(k1) == fvalueraw(bits);
      case ctb(LUA_VLNGSTR):
        return luaS_eqstr(tsvalue(k1), gco2ts(gcvalueraw(bits)));
      default:
        return gcvalue(k1) == gcvalueraw(bits);
    }
  }
}

l_sinline int equalkey (const TValue *k1, const Node *n2, int deadok) {
  return equalkeybits(k1, keytt(n2), keyval(n2), deadok);
}


/*
** "Generic" get version. (Not that generic: not valid for integers,
** which may be in array part, nor for floats with integral values.)
** See explanation about 'deadok' in function 'equalkey'.
*/
static const TValue *findkey (Node *n, const TValue *key, int deadok) {
  for (;;) {  /* check whether 'key' is somewhere in the chain */
    if (equalkey(key, n, deadok))
      return gval(n);  /* that's it */
    else {
      int nx = gnext(n);
      if (nx == 0)
        return &absentkey;  /* not found */
      n += nx;
    }
  }
}

l_sinline const TValue *getgeneric (Node *nodes, const TValue *key, int deadok) {
  return findkey(mainpositionTV(nodes, key), key, deadok);
}

/*
** Return the index 'k' (converted to an unsigned) if it is inside
** the range [1, limit].
*/
static unsigned checkrange (lua_Integer k, unsigned limit) {
  return (l_castS2U(k) - 1u < limit) ? cast_uint(k) : 0;
}


/*
** Return the index 'k' if 'k' is an appropriate key to live in the
** array part of a table, 0 otherwise.
*/
#define arrayindex(k)	checkrange(k, MAXASIZE)


/*
** Check whether an integer key is in the array part of a table and
** return its index there, or zero.
*/
#define ikeyinarray(t,k)	checkrange(k, (t)->asize)


/*
** Check whether a key is in the array part of a table and return its
** index there, or zero.
*/
static unsigned keyinarray (Table *t, const TValue *key) {
  return (ttisinteger(key)) ? ikeyinarray(t, ivalue(key)) : 0;
}


/*
** returns the index of a 'key' for table traversals. First goes all
** elements in the array part, then elements in the hash part. The
** beginning of a traversal is signaled by 0.
*/
static unsigned findindex (lua_State *L, Table *t, TValue *key,
                           unsigned asize, unsigned *part) {
  unsigned int i;
  if (ttisnil(key)) return 0;  /* first iteration */
  i = keyinarray(t, key);
  if (i != 0)  /* is 'key' inside array part? */
    return i;  /* yes; that's the index */
  else {
    Node *nodes;
    const TValue *n;
    if (luaH_hashdir(t->node) != NULL) *part = hashpart(key);
    nodes = luaH_hashnodes(t, *part);
    n = getgeneric(nodes, key, 1);
    if (l_unlikely(isabstkey(n)))
      luaG_runerror(L, "invalid key to 'next'");  /* key not found */
    i = cast_uint(nodefromval(n) - nodes);  /* key index in hash partition */
    /* hash elements are numbered after array ones */
    return (i + 1) + asize;
  }
}


static int rawnext (lua_State *L, Table *t, StkId key) {
  unsigned int asize = t->asize;
  unsigned part = 0, parts = luaH_hashparts(t);
  unsigned i = findindex(L, t, s2v(key), asize, &part);
  for (; i < asize; i++) {  /* try first array part */
    lu_byte tag = *getArrTag(t, i);
    if (!tagisempty(tag)) {  /* a non-empty entry? */
      setivalue(s2v(key), cast_int(i) + 1);
      farr2val(t, i, tag, s2v(key + 1));
      return 1;
    }
  }
  i -= asize;
  for (; part < parts; part++, i = 0) {
    Node *nodes = luaH_hashnodes(t, part);
    for (; i < luaH_nodesize(nodes); i++) {
      Node *n = nodes + i;
      if (!isempty(gval(n))) {
        getnodekey(L, s2v(key), n);
        setobj2s(L, key + 1, gval(n));
        return 1;
      }
    }
  }
  return 0;  /* no more elements */
}


/* Extra space in Node array if it has a lastfree entry */
#define extraLastfree(nodes)	(haslastfree(nodes) ? sizeof(Limbox) : 0)

/* 'node' size in bytes */
static size_t sizehash (const Node *nodes) {
  return cast_sizet(luaH_nodesize(nodes)) * sizeof(Node) + extraLastfree(nodes);
}

static char *hashbase (Node *nodes) {
  return cast_charp(nodes) - extraLastfree(nodes);
}

/* A displaced inline image stays allocated with its owner. Existing readers
** can validate their layout sequence even after the owner resizes. */
static int separate (const Table *owner, const void *base) {
  return cast(uintptr_t, base) - cast(uintptr_t, owner + 1) >= owner->inline_bytes;
}

static void freehash (lua_State *L, Table *t, Node *nodes) {
  if (luaH_nodesize(nodes) != 0 && separate(t, hashbase(nodes)))
    luaM_freemem(L, hashbase(nodes), sizehash(nodes));
}


/*
** {=============================================================
** Rehash
** ==============================================================
*/

static int insertkey (lua_State *L, Node *nodes, const TValue *key, TValue *value);
static void newcheckedkey (Table *t, const TValue *key, TValue *value);


/*
** Structure to count the keys in a table.
** 'total' is the total number of keys in the table.
** 'na' is the number of *array indices* in the table (see 'arrayindex').
** 'deleted' is true if there are deleted nodes in the hash part.
** 'nums' is a "count array" where 'nums[i]' is the number of integer
** keys between 2^(i - 1) + 1 and 2^i. Note that 'na' is the summation
** of 'nums'.
*/
typedef struct {
  unsigned total;
  unsigned na;
  int deleted;
  unsigned nums[MAXABITS + 1];
} Counters;


/*
** Check whether it is worth to use 'na' array entries instead of 'nh'
** hash nodes. (A hash node uses ~3 times more memory than an array
** entry: Two values plus 'next' versus one value.) Evaluate with size_t
** to avoid overflows.
*/
#define arrayXhash(na,nh)	(cast_sizet(na) <= cast_sizet(nh) * 3)

/*
** Compute the optimal size for the array part of table 't'.
** This size maximizes the number of elements going to the array part
** while satisfying the condition 'arrayXhash' with the use of memory if
** all those elements went to the hash part.
** 'ct->na' enters with the total number of array indices in the table
** and leaves with the number of keys that will go to the array part;
** return the optimal size for the array part.
*/
static unsigned computesizes (Counters *ct) {
  int i;
  unsigned int twotoi;  /* 2^i (candidate for optimal size) */
  unsigned int a = 0;  /* number of elements smaller than 2^i */
  unsigned int na = 0;  /* number of elements to go to array part */
  unsigned int optimal = 0;  /* optimal size for array part */
  /* traverse slices while 'twotoi' does not overflow and total of array
     indices still can satisfy 'arrayXhash' against the array size */
  for (i = 0, twotoi = 1;
       twotoi > 0 && arrayXhash(twotoi, ct->na);
       i++, twotoi *= 2) {
    unsigned nums = ct->nums[i];
    a += nums;
    if (nums > 0 &&  /* grows array only if it gets more elements... */
        arrayXhash(twotoi, a)) {  /* ...while using "less memory" */
      optimal = twotoi;  /* optimal size (till now) */
      na = a;  /* all elements up to 'optimal' will go to array part */
    }
  }
  ct->na = na;
  return optimal;
}


static void countint (lua_Integer key, Counters *ct) {
  unsigned int k = arrayindex(key);
  if (k != 0) {  /* is 'key' an array index? */
    ct->nums[luaO_ceillog2(k)]++;  /* count as such */
    ct->na++;
  }
}


l_sinline int arraykeyisempty (const Table *t, unsigned key) {
  int tag = *getArrTag(t, key - 1);
  return tagisempty(tag);
}


/*
** Count keys in array part of table 't'.
*/
static void numusearray (const Table *t, Counters *ct) {
  int lg;
  unsigned int ttlg;  /* 2^lg */
  unsigned int ause = 0;  /* summation of 'nums' */
  unsigned int i = 1;  /* index to traverse all array keys */
  unsigned int asize = t->asize;
  /* traverse each slice */
  for (lg = 0, ttlg = 1; lg <= MAXABITS; lg++, ttlg *= 2) {
    unsigned int lc = 0;  /* counter */
    unsigned int lim = ttlg;
    if (lim > asize) {
      lim = asize;  /* adjust upper limit */
      if (i > lim)
        break;  /* no more elements to count */
    }
    /* count elements in range (2^(lg - 1), 2^lg] */
    for (; i <= lim; i++) {
      if (!arraykeyisempty(t, i))
        lc++;
    }
    ct->nums[lg] += lc;
    ause += lc;
  }
  ct->total += ause;
  ct->na += ause;
}


/*
** Count keys in hash part of table 't'. As this only happens during
** a rehash, all nodes have been used. A node can have a nil value only
** if it was deleted after being created.
*/
static void numusehash (Node *nodes, Counters *ct) {
  unsigned i = luaH_nodesize(nodes);
  unsigned total = 0;
  while (i--) {
    Node *n = &nodes[i];
    if (isempty(gval(n))) {
      lua_assert(!keyisnil(n));  /* entry was deleted; key cannot be nil */
      ct->deleted = 1;
    }
    else {
      total++;
      if (keyisinteger(n))
        countint(keyival(n), ct);
    }
  }
  ct->total += total;
}


/*
** Convert an "abstract size" (number of slots in an array) to
** "concrete size" (number of bytes in the array).
*/
static size_t concretesize (unsigned int size) {
  if (size == 0)
    return 0;
  else  /* space for the two arrays plus an unsigned in between */
    return size * (sizeof(Value) + 1) + sizeof(unsigned);
}


/*
** Prepare a replacement array, leaving the old image readable until the
** layout commits. A resize to zero requires no backing allocation.
** We could reallocate the array, but we still would need to move the
** elements to their new position, so the copy implicit in realloc is a
** waste. Moreover, most allocators will move the array anyway when the
** new size is double the old one (the most common case).
*/
static Value *resizearray (lua_State *L, Table *t, unsigned oldasize,
                           unsigned newasize) {
  if (oldasize == newasize)
    return t->array;  /* nothing to be done */
  else if (newasize == 0) return NULL;
  else {
    size_t newasizeb = concretesize(newasize);
    Value *np = cast(Value *,
                  luaM_realloc_(L, NULL, 0, newasizeb));
    if (np == NULL)  /* allocation error? */
      return NULL;
    np += newasize;  /* shift pointer to the end of value segment */
    if (oldasize > 0) {
      /* move common elements to new position */
      size_t oldasizeb = concretesize(oldasize);
      Value *op = t->array;  /* original array */
      unsigned tomove = (oldasize < newasize) ? oldasize : newasize;
      size_t tomoveb = (oldasize < newasize) ? oldasizeb : newasizeb;
      lua_assert(tomoveb > 0);
      memcpy(np - tomove, op - tomove, tomoveb);
    }
    return np;
  }
}


/*
** Creates an array for the hash part of a table with the given
** size, or reuses the dummy node if size is zero.
** The computation for size overflow is in two steps: the first
** comparison ensures that the shift in the second one does not
** overflow.
*/
static Node *initnodes (char *storage, int lsize) {
  Node *base;
  if (lsize < 0) {  /* no elements to hash part? */
    base = cast(Node *, &luaH_dummynode);
  }
  else {
    int i;
    unsigned size = twoto(lsize);
    size_t offset = lsize >= LIMFORLAST ? sizeof(Limbox) : 0;
    base = cast(Node *, storage + offset);
    base->u.lsize = cast_byte(lsize);
    if (offset != 0)
      cast(Limbox *, storage)->lastfree = base + size;
    for (i = 0; i < cast_int(size); i++) {
      Node *n = base + i;
      setnext(n, 0);
      setnilkey(n);
      setempty(gval(n));
    }
  }
  return base;
}

static size_t nodebytes (int lsize) {
  if (lsize < 0) return 0;
  return cast_sizet(twoto(lsize)) * sizeof(Node) +
         (lsize >= LIMFORLAST ? sizeof(Limbox) : 0);
}

static int setnodevector (lua_State *L, Table *t, int lsize) {
  char *storage = NULL;
  if (lsize >= 0) {
    storage = luaM_realloc_(L, NULL, 0, nodebytes(lsize));
    if (storage == NULL) return 0;
  }
  __atomic_store_n(&t->node, initnodes(storage, lsize), __ATOMIC_RELEASE);
  return 1;
}


/*
** (Re)insert all elements from the hash part of 'ot' into table 't'.
*/
static void reinserthash (lua_State *L, Table *ot, Table *t) {
  unsigned j;
  unsigned size = sizenode(ot);
  for (j = 0; j < size; j++) {
    Node *old = gnode(ot, j);
    if (!isempty(gval(old))) {
      /* doesn't need barrier/invalidate cache, as entry was
         already present in the table */
      TValue k;
      getnodekey(L, &k, old);
      newcheckedkey(t, &k, gval(old));
    }
  }
}


/*
** Exchange hash images. Each allocation carries its capacity, and the
** pointer identifies the dummy image. Metamethod flags belong to the table.
*/
static void exchangehashpart (Table *t1, Table *t2) {
  LuaHashDirectory *directory = luaH_hashdir(t1->node);
  Node **primary = directory == NULL ? &t1->node : &directory->parts[0].nodes;
  Node *node = *primary;
  __atomic_store_n(primary, t2->node, __ATOMIC_RELEASE);
  __atomic_store_n(&t2->node, node, __ATOMIC_RELEASE);
}


/*
** Re-insert into the new hash part of a table the elements from the
** vanishing slice of the array part.
*/
static void reinsertOldSlice (Table *t, unsigned oldasize,
                                        unsigned newasize) {
  unsigned i;
  for (i = newasize; i < oldasize; i++) {  /* traverse vanishing slice */
    lu_byte tag = *getArrTag(t, i);
    if (!tagisempty(tag)) {  /* a non-empty entry? */
      TValue key, aux;
      setivalue(&key, l_castU2S(i) + 1);  /* make the key */
      farr2val(t, i, tag, &aux);  /* copy value into 'aux' */
      insertkey(NULL, luaH_hashnodes(t, 0), &key, &aux);
    }
  }
}


/*
** Clear new slice of the array.
*/
static void clearNewSlice (Table *t, unsigned oldasize, unsigned newasize) {
  for (; oldasize < newasize; oldasize++)
    __atomic_store_n(getArrTag(t, oldasize), LUA_VEMPTY, __ATOMIC_RELEASE);
}


/*
** Resize table 't' for the new given sizes. Both allocations (for
** the hash part and for the array part) can fail, which creates some
** subtleties. If the first allocation, for the hash part, fails, an
** error is raised and that is it. Otherwise, it copies the elements from
** the shrinking part of the array (if it is shrinking) into the new
** hash. Then it reallocates the array part.  If that fails, the table
** is in its original state; the function frees the new hash part and then
** raises the allocation error. Otherwise, it sets the new hash part
** into the table, initializes the new part of the array (if any) with
** nils and reinserts the elements of the old hash back into the new
** parts of the table.
** Note that if the new size for the array part ('newasize') is equal to
** the old one ('oldasize'), this function will do nothing with that
** part.
*/
static void rawresize (lua_State *L, Table *t, unsigned newasize,
                                          unsigned nhsize) {
  Table newt;  /* to keep the new hash part */
  LuaNativeRoot image;
  unsigned oldasize;
  Value *newarray;
  Value *oldarray = t->array;
  Node *oldnodes = luaH_hashnodes(t, 0);
  void *retired_array, *retired_nodes;
  LuaTableRetirement *retirement = NULL;
  int lsize = nhsize == 0 ? -1 : luaO_ceillog2(nhsize);
  luaHS_reclaim(L, LUAHS_ASSIST);
  luaC_traceview(L, t);
  oldasize = t->asize;
  if (newasize > MAXASIZE)
    luaG_runerror(L, "table overflow");
  if (lsize > MAXHBITS || (lsize >= 0 && (1u << lsize) > MAXHSIZE))
    luaG_runerror(L, "table overflow");
  /* create new hash part with appropriate size into 'newt' */
  newt.flags = 0;
  newt.inline_bytes = 0;
  newt.array = t->array;
  newt.asize = oldasize;
  newt.node = NULL;
  luaR_pushroot(L, &image);
  image.table_view = &newt;
  luaC_traceview(L, &newt);
  retired_array = oldasize != newasize && oldasize != 0 &&
                  separate(t, oldarray - oldasize) ? oldarray - oldasize : NULL;
  retired_nodes = luaH_nodesize(oldnodes) != 0 && separate(t, hashbase(oldnodes))
                  ? hashbase(oldnodes) : NULL;
  if (retired_array != NULL || retired_nodes != NULL)
    retirement = luaM_new(L, LuaTableRetirement);
  /* Once the descriptor exists, allocations report failure without throwing:
  ** every error releases it and leaves the original image installed. */
  if (!setnodevector(L, &newt, lsize)) {
    if (retirement != NULL) luaM_free(L, retirement);
    luaR_poproot(L, &image);
    luaM_error(L);
  }
  if (newasize < oldasize) {  /* will array shrink? */
    /* re-insert into the new hash the elements from vanishing slice */
    exchangehashpart(t, &newt);  /* pretend table has new hash */
    reinsertOldSlice(t, oldasize, newasize);
    exchangehashpart(t, &newt);  /* restore old hash (in case of errors) */
  }
  /* allocate new array */
  newarray = resizearray(L, t, oldasize, newasize);
  newt.array = NULL;
  newt.asize = 0;
  if (l_unlikely(newarray == NULL && newasize > 0)) {  /* allocation failed? */
    if (retirement != NULL) luaM_free(L, retirement);
    freehash(L, &newt, newt.node);
    luaR_poproot(L, &image);
    luaM_error(L);  /* raise error (with array unchanged) */
  }
  /* allocation ok; initialize new part of the array */
  exchangehashpart(t, &newt);  /* 't' has the new hash ('newt' has the old) */
  __atomic_store_n(&t->array, newarray, __ATOMIC_RELEASE);
  __atomic_store_n(&t->asize, newasize, __ATOMIC_RELEASE);
  if (newarray != NULL)
    *lenhint(t) = newasize / 2u;  /* set an initial hint */
  clearNewSlice(t, oldasize, newasize);
  /* re-insert elements from old hash part into new parts */
  reinserthash(L, &newt, t);  /* 'newt' now has the old hash */
  if (retirement != NULL) {
    retirement->array = retired_array;
    retirement->array_bytes = retirement->array != NULL ? concretesize(oldasize) : 0;
    retirement->nodes = retired_nodes;
    retirement->node_bytes = retirement->nodes != NULL ? sizehash(newt.node) : 0;
    luaHS_publish(L, retirement);
  }
  luaR_poproot(L, &image);
}


static void rawresizearray (lua_State *L, Table *t, unsigned int nasize) {
  unsigned nsize = luaH_nodesize(luaH_hashnodes(t, 0));
  rawresize(L, t, nasize, nsize);
}

static void destroyparts (LuaHashDirectory *directory, unsigned count) {
  while (count != 0) {
    LuaHashPart *part = &directory->parts[--count];
    lua_assert(part->waiters == NULL && part->gate == 0);
    luaX_checksync(pthread_cond_destroy(&part->changed));
  }
}

/* Partition only when a shared hash reaches a size that can amortize the
** directory. Allocate every empty destination before moving any key. */
static void partitionhash (lua_State *L, Table *table, const TValue *extra) {
  unsigned counts[LUA_HASH_PARTS] = {0};
  Node *source = table->node;
  unsigned i, size = luaH_nodesize(source);
  LuaHashDirectory *directory;
  LuaTableRetirement *retirement;
  counts[hashpart(extra)]++;
  for (i = 0; i < size; ++i) {
    Node *node = source + i;
    if (!isempty(gval(node))) {
      TValue key;
      getnodekey(L, &key, node);
      counts[hashpart(&key)]++;
    }
  }
  for (i = 0; i < LUA_HASH_PARTS; ++i)
    if (counts[i] > MAXHSIZE) luaG_runerror(L, "table overflow");
  retirement = luaM_new(L, LuaTableRetirement);
  directory = luaM_realloc_(L, NULL, 0, sizeof(*directory));
  if (directory == NULL) {
    luaM_free(L, retirement);
    luaM_error(L);
  }
  for (i = 0; i < LUA_HASH_PARTS; ++i) {
    unsigned count = counts[i];
    LuaHashPart *part = &directory->parts[i];
    int log = count != 0 ? luaO_ceillog2(count) : -1;
    char *storage = log >= 0 ? luaM_realloc_(L, NULL, 0, nodebytes(log)) : NULL;
    if (log >= 0 && storage == NULL) break;
    part->nodes = initnodes(storage, log);
    part->version = 0;
    part->gate = 0;
    part->waiters = NULL;
    if (pthread_cond_init(&part->changed, NULL) != 0) {
      freehash(L, table, part->nodes);
      break;
    }
  }
  if (i != LUA_HASH_PARTS) {
    destroyparts(directory, i);
    while (i != 0) freehash(L, table, directory->parts[--i].nodes);
    luaM_free(L, directory);
    luaM_free(L, retirement);
    luaM_error(L);
  }
  luaC_traceview(L, table);
  for (i = 0; i < size; ++i) {
    Node *node = source + i;
    if (!isempty(gval(node))) {
      TValue key;
      int inserted;
      getnodekey(L, &key, node);
      inserted = insertkey(NULL, directory->parts[hashpart(&key)].nodes,
                           &key, gval(node));
      lua_assert(inserted);
      UNUSED(inserted);
    }
  }
  __atomic_store_n(&table->node,
    cast(Node *, cast(uintptr_t, directory) | 1), __ATOMIC_RELEASE);
  retirement->array = NULL;
  retirement->array_bytes = 0;
  retirement->nodes = hashbase(source);
  retirement->node_bytes = sizehash(source);
  lua_assert(separate(table, retirement->nodes));
  luaHS_publish(L, retirement);
}


/*
** Rehash a table. First, count its keys. If there are array indices
** outside the array part, compute the new best size for that part.
** Then, resize the table.
*/
static void rehash (lua_State *L, Table *t, const TValue *ek) {
  unsigned asize;  /* optimal size for array part */
  Counters ct;
  unsigned i;
  unsigned nsize;  /* size for the hash part */
  /* reset counts */
  for (i = 0; i <= MAXABITS; i++) ct.nums[i] = 0;
  ct.na = 0;
  ct.deleted = 0;
  ct.total = 1;  /* count extra key */
  if (ttisinteger(ek))
    countint(ivalue(ek), &ct);  /* extra key may go to array */
  numusehash(luaH_hashnodes(t, 0), &ct);  /* count keys in the primary hash */
  if (ct.na == 0) {
    /* no new keys to enter array part; keep it with the same size */
    asize = t->asize;
  }
  else {  /* compute best size for array part */
    numusearray(t, &ct);  /* count keys in array part */
    asize = computesizes(&ct);  /* compute new size for array part */
  }
  /* all keys not in the array part go to the hash part */
  nsize = ct.total - ct.na;
  if (ct.deleted) {  /* table has deleted entries? */
    /* insertion-deletion-insertion: give hash some extra size to
       avoid repeated resizings */
    nsize += nsize >> 2;
  }
  /* resize the table to new computed sizes */
  if (asize == t->asize && nsize >= HASH_PARTITION_THRESHOLD &&
      luaH_hashdir(t->node) == NULL &&
      luaH_nodesize(t->node) >= HASH_PARTITION_THRESHOLD &&
      (__atomic_load_n(&t->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED))
    partitionhash(L, t, ek);
  else rawresize(L, t, asize, nsize);
}

/*
** }=============================================================
*/


_Static_assert(sizeof(Table) <= LUA_TABLE_INLINE_LIMIT &&
               LUA_TABLE_INLINE_LIMIT <= UCHAR_MAX, "inline table size");
_Static_assert(sizeof(Table) % _Alignof(Value) == 0 &&
               sizeof(Table) % _Alignof(Node) == 0 &&
               sizeof(Value) % _Alignof(Node) == 0, "inline table alignment");

Table *luaH_new (lua_State *L, unsigned asize, unsigned hsize) {
  const size_t limit = LUA_TABLE_INLINE_LIMIT - sizeof(Table);
  size_t bytes = 0, arraybytes = 0;
  int lsize = -1;
  GCObject *o;
  Table *t;
  /* A small empty layout can be fully initialized before another execution
  ** can see it. Its construction needs neither a layout lock nor retirement. */
  if (asize <= limit / (sizeof(Value) + 1) && hsize <= limit / sizeof(Node)) {
    size_t hashbytes;
    lsize = hsize != 0 ? luaO_ceillog2(hsize) : -1;
    hashbytes = nodebytes(lsize);
    arraybytes = concretesize(asize);
    if (hashbytes != 0)
      arraybytes = (arraybytes + sizeof(Value) - 1) & ~(sizeof(Value) - 1);
    if (arraybytes + hashbytes <= limit) bytes = arraybytes + hashbytes;
  }
  o = luaC_newobj(L, LUA_VTABLE, sizeof(Table) + bytes);
  t = gco2t(o);
  luaA_initlock(&t->access);
  luaA_initsharing(L, t);
  t->sequence = 0;
  t->metatable = NULL;
  t->inline_bytes = cast_byte(bytes);
  t->flags = maskflags;  /* table has no metamethod fields */
  t->array = NULL;
  t->asize = 0;
  if (bytes != 0) {
    char *storage = cast_charp(t + 1);
    if (asize != 0) {
      t->array = cast(Value *, storage) + asize;
      t->asize = asize;
      *lenhint(t) = asize / 2u;
      memset(getArrTag(t, 0), LUA_VEMPTY, asize);
    }
    t->node = initnodes(storage + arraybytes, lsize);
  }
  else {
    t->node = initnodes(NULL, -1);
    if (asize != 0 || hsize != 0) {
      /* luaH_resize roots its receiver across larger backing allocations. */
      luaH_resize(L, t, asize, hsize);
    }
  }
  return t;
}


l_sinline lu_mem hashbytes (const Table *owner, Node *nodes) {
  return (nodes != &luaH_dummynode && separate(owner, hashbase(nodes)))
    ? sizehash(nodes) : 0;
}

/* The caller holds the layout or provides a validated snapshot. */
lu_mem luaH_size (const Table *owner, const Table *layout) {
  lu_mem sz = cast(lu_mem, sizeof(Table) + owner->inline_bytes);
  Node *image = layout->node;
  LuaHashDirectory *directory = luaH_hashdir(image);
  if (layout->asize != 0 && separate(owner, layout->array - layout->asize))
    sz += concretesize(layout->asize);
  if (directory == NULL) sz += hashbytes(owner, image);
  else {
    unsigned i;
    sz += sizeof(*directory);
    for (i = 0; i < LUA_HASH_PARTS; ++i) {
      Node *nodes = __atomic_load_n(&directory->parts[i].nodes, __ATOMIC_ACQUIRE);
      sz += hashbytes(owner, nodes);
    }
  }
  return sz;
}

l_mem luaH_hashsize (const Table *table) {
  l_mem size = 0;
  unsigned i, parts = luaH_hashparts(table);
  for (i = 0; i < parts; ++i) size += luaH_nodesize(luaH_hashnodes(table, i));
  return size;
}


/* Every external part at most this size comes from a pool. A sweep batch
** never enters the host allocator while a collector could wait on it. */
size_t luaH_pooledsize (Table *t) {
  size_t size = sizeof(*t) + t->inline_bytes;
  if (luaH_hashdir(t->node) != NULL) return 0;
  if (t->asize != 0 && separate(t, t->array - t->asize)) {
    size_t bytes = concretesize(t->asize);
    if (bytes > LUA_ALLOC_MAX) return 0;
    size += bytes;
  }
  if (!isdummy(t) && separate(t, hashbase(t->node))) {
    size_t bytes = sizehash(t->node);
    if (bytes > LUA_ALLOC_MAX) return 0;
    size += bytes;
  }
  lua_assert(luaA_lockowner(&t->access) == NULL);
  return size;
}

l_mem luaH_freepooled (lua_State *L, Table *t) {
  size_t size = luaH_pooledsize(t);
  size_t header = sizeof(*t) + t->inline_bytes;
  lua_assert(luaMP_page(cast(LuaAllocBlock *, t) - 1) != NULL);
  if (size == 0) return 0;
  if (size != header) luaH_freeparts(L, t);
  luaM_freemem(L, t, header);
  return cast(l_mem, size);
}

void luaH_retire (lua_State *L, Table *t) {
  unsigned i, parts = luaH_hashparts(t);
  for (i = 0; i < parts; ++i) {
    Node *nodes = luaH_hashnodes(t, i);
    if (luaH_nodesize(nodes) != 0 && separate(t, hashbase(nodes)))
      luaM_retire_(L, hashbase(nodes), sizehash(nodes));
  }
  if (parts > 1) {
    LuaHashDirectory *directory = luaH_hashdir(t->node);
    destroyparts(directory, parts);
    luaM_retire_(L, directory, sizeof(*directory));
  }
  if (t->asize != 0 && separate(t, t->array - t->asize))
    luaM_retire_(L, t->array - t->asize, concretesize(t->asize));
  luaM_retire_(L, t, sizeof(*t) + t->inline_bytes);
}

void luaH_freeparts (lua_State *L, Table *t) {
  unsigned i, parts = luaH_hashparts(t);
  for (i = 0; i < parts; ++i) freehash(L, t, luaH_hashnodes(t, i));
  if (parts > 1) {
    LuaHashDirectory *directory = luaH_hashdir(t->node);
    destroyparts(directory, parts);
    luaM_free(L, directory);
  }
  if (t->asize != 0 && separate(t, t->array - t->asize))
    luaM_freemem(L, t->array - t->asize, concretesize(t->asize));
}

void luaH_free (lua_State *L, Table *t) {
  lua_assert(luaA_lockowner(&t->access) == NULL);
  luaH_freeparts(L, t);
  luaM_freemem(L, t, sizeof(*t) + t->inline_bytes);
}


static Node *getfreepos (Node *nodes) {
  if (haslastfree(nodes)) {  /* does it have 'lastfree' information? */
    /* look for a spot before 'lastfree', updating 'lastfree' */
    while (getlastfree(nodes) > nodes) {
      Node *free = --getlastfree(nodes);
      if (keyisnil(free))
        return free;
    }
  }
  else {  /* no 'lastfree' information */
    unsigned i = luaH_nodesize(nodes);
    while (i--) {  /* do a linear search */
      Node *free = nodes + i;
      if (keyisnil(free))
        return free;
    }
  }
  return NULL;  /* could not find a free place */
}



/*
** Inserts a new key into a hash table; first, check whether key's main
** position is free. If not, check whether colliding node is in its main
** position or not: if it is not, move colliding node to an empty place
** and put new key in its main position; otherwise (colliding node is in
** its main position), new key goes to an empty position. Return 0 if
** could not insert key (could not find a free space).
*/
static int insertat (lua_State *L, Node *nodes, const TValue *key, TValue *value,
                     Node *mp) {
  /* table cannot already contain the key */
  lua_assert(isabstkey(getgeneric(nodes, key, 0)));
  if (!isempty(gval(mp)) || nodes == &luaH_dummynode) {
    Node *othern;
    Node *f = getfreepos(nodes);  /* get a free place */
    if (f == NULL)  /* cannot find a free place? */
      return 0;
    othern = mainpositionfromnode(nodes, mp);
    if (othern != mp) {  /* is colliding node out of its main position? */
      /* A streaming marker may already have visited the destination. Resize
      ** shades its entire old image; an in-place relocation shades this pair. */
      if (L != NULL && luaC_tracing(L)) {
        if (keyiscollectable(mp)) luaC_traceobject(L, gckey(mp));
        luaC_tracevalue(L, gval(mp));
      }
      /* yes; move colliding node into free position */
      while (othern + gnext(othern) != mp)  /* find previous */
        othern += gnext(othern);
      setnext(othern, cast_int(f - othern));
      __atomic_store(&f->u.value_, &mp->u.value_, __ATOMIC_RELEASE);
      __atomic_store_n(&f->u.tt_, mp->u.tt_, __ATOMIC_RELEASE);
      __atomic_store(&f->u.key_val, &mp->u.key_val, __ATOMIC_RELEASE);
      __atomic_store_n(&f->u.key_tt, mp->u.key_tt, __ATOMIC_RELEASE);
      setnext(f, gnext(mp));
      if (gnext(mp) != 0) {
        setnext(f, gnext(f) + cast_int(mp - f));
        setnext(mp, 0);
      }
      setempty(gval(mp));
    }
    else {  /* colliding node is in its own main position */
      /* new node will go into free position */
      if (gnext(mp) != 0)
        setnext(f, cast_int((mp + gnext(mp)) - f));
      else lua_assert(gnext(f) == 0);
      setnext(mp, cast_int(f - mp));
      mp = f;
    }
  }
  setnodekey(mp, key);
  lua_assert(isempty(gval(mp)));
  setobj2t(cast(lua_State *, 0), gval(mp), value);
  return 1;
}

static int insertkey (lua_State *L, Node *nodes, const TValue *key, TValue *value) {
  return insertat(L, nodes, key, value, mainpositionTV(nodes, key));
}

/*
** Insert a key in a table where there is space for that key, the
** key is valid, and the value is not nil.
*/
static void newcheckedkey (Table *t, const TValue *key, TValue *value) {
  unsigned i = keyinarray(t, key);
  if (i > 0)  /* is key in the array part? */
    obj2arr(t, i - 1, value);  /* set value in the array */
  else {
    int done = insertkey(NULL, keynodes(t, key), key, value);
    lua_assert(done);  /* it cannot fail */
    cast(void, done);  /* to avoid warnings */
  }
}


static void luaH_newkey (lua_State *L, Table *t, const TValue *key,
                                                 TValue *value) {
  luaC_tracevalue(L, key);
  luaC_tracevalue(L, value);
  if (!ttisnil(value)) {  /* do not insert nil values */
    int done = insertkey(L, keynodes(t, key), key, value);
    if (!done) {  /* could not find a free place? */
      rehash(L, t, key);  /* grow table */
      newcheckedkey(t, key, value);  /* insert key in grown table */
    }
    luaC_barrierback(L, obj2gco(t), key);
    /* for debugging only: any new key may force an emergency collection */
    condchangemem(L, (void)0, (void)0, 1);
  }
}


l_sinline const TValue *findinteger (Node *n, lua_Integer key) {
  for (;;) {  /* check whether 'key' is somewhere in the chain */
    if (keyisinteger(n) && keyival(n) == key)
      return gval(n);  /* that's it */
    else {
      int nx = gnext(n);
      if (nx == 0) break;
      n += nx;
    }
  }
  return &absentkey;
}

static const TValue *getintfromhash (Table *t, lua_Integer key) {
  Node *nodes = t->node;
  LuaHashDirectory *directory = luaH_hashdir(nodes);
  if (directory != NULL)
    nodes = directory->parts[integerpart(key)].nodes;
  lua_assert(!ikeyinarray(t, key));
  return findinteger(nodes + hashint(nodesteps(nodes), key), key);
}

static int hashkeyisempty (Table *t, lua_Unsigned key) {
  const TValue *val = getintfromhash(t, l_castU2S(key));
  return isempty(val);
}


static lu_byte finishnodeget (const TValue *val, TValue *res) {
  if (!ttisnil(val)) {
    setobj(((lua_State*)NULL), res, val);
  }
  return ttypetag(val);
}


static lu_byte rawgetint (Table *t, lua_Integer key, TValue *res) {
  unsigned k = ikeyinarray(t, key);
  if (k > 0) {
    lu_byte tag = *getArrTag(t, k - 1);
    if (!tagisempty(tag))
      farr2val(t, k - 1, tag, res);
    return tag;
  }
  else
    return finishnodeget(getintfromhash(t, key), res);
}


/*
** search function for short strings
*/
l_sinline const TValue *findshortstr (Node *n, TString *key) {
  lua_assert(strisshr(key));
  for (;;) {  /* check whether 'key' is somewhere in the chain */
    if (keyisshrstr(n) && eqshrstr(keystrval(n), key))
      return gval(n);  /* that's it */
    else {
      int nx = gnext(n);
      if (nx == 0)
        return &absentkey;  /* not found */
      n += nx;
    }
  }
}

const TValue *luaH_Hgetshortstr (Table *t, TString *key) {
  Node *nodes = t->node;
  LuaHashDirectory *directory = luaH_hashdir(nodes);
  if (directory != NULL)
    nodes = directory->parts[distributedpart(key->hash)].nodes;
  return findshortstr(hashstr(nodes, key), key);
}

static lu_byte rawgetshortstr (Table *t, TString *key, TValue *res) {
  return finishnodeget(luaH_Hgetshortstr(t, key), res);
}


static const TValue *Hgetlongstr (Table *t, TString *key) {
  TValue ko;
  lua_assert(!strisshr(key));
  setsvalue(cast(lua_State *, NULL), &ko, key);
  return getgeneric(keynodes(t, &ko), &ko, 0);
}


/*
** main search function
*/
lu_byte luaH_getowned (Table *t, const TValue *key, TValue *res) {
  const TValue *slot;
  switch (ttypetag(key)) {
    case LUA_VSHRSTR:
      return rawgetshortstr(t, tsvalue(key), res);
    case LUA_VNUMINT:
      return rawgetint(t, ivalue(key), res);
    case LUA_VNIL:
      slot = &absentkey;
      break;
    case LUA_VNUMFLT: {
      lua_Integer k;
      if (luaV_flttointeger(fltvalue(key), &k, F2Ieq)) /* integral index? */
        return rawgetint(t, k, res);  /* use specialized version */
      /* else... */
    }  /* FALLTHROUGH */
    default:
      slot = getgeneric(keynodes(t, key), key, 0);
      break;
  }
  return finishnodeget(slot, res);
}


/*
** When a 'pset' cannot be completed, this function returns an encoding
** of its result, to be used by 'rawfinishset'.
*/
/* Private slot encodings remain valid only while ownership is retained. */
#define HOK 0
#define HNOTFOUND 1
#define HFIRSTNODE 3

static int retpsetcode (Table *t, const TValue *slot) {
  if (isabstkey(slot))
    return HNOTFOUND;  /* no slot with that key */
  else  /* return node encoded */
    return cast_int((cast(Node*, slot) - luaH_hashnodes(t, 0))) + HFIRSTNODE;
}


static void tracenodewrite (lua_State *L, const TValue *slot, TValue *val) {
  if (luaC_tracing(L)) {
    const Node *node = cast(const Node *, slot);
    if (keyiscollectable(node)) luaC_traceobject(L, gckey(node));
    luaC_prewrite(L, slot, val);
  }
}

static void setnodevalue (lua_State *L, Node *node, TValue *value) {
  setobj2t(L, gval(node), value);
  /* Read-only markers never edit hash storage. Retire the key tag as part of
  ** deletion, before an empty slot could retain a dangling collectable key. */
  if (ttisnil(value) && keyiscollectable(node)) setdeadkey(node);
}

static int finishnodeset (lua_State *L, Table *t, const TValue *slot, TValue *val) {
  if (!ttisnil(slot)) {
    tracenodewrite(L, slot, val);
    setnodevalue(NULL, cast(Node*, slot), val);
    return HOK;  /* success */
  }
  else
    return retpsetcode(t, slot);
}


static int rawfinishnodeset (lua_State *L, const TValue *slot, TValue *val) {
  if (isabstkey(slot))
    return 0;  /* no slot with that key */
  else {
    tracenodewrite(L, slot, val);
    setnodevalue(NULL, cast(Node*, slot), val);
    return 1;  /* success */
  }
}


static int rawpsetint (lua_State *L, Table *t, lua_Integer key, TValue *val) {
  lua_assert(!ikeyinarray(t, key));
  return finishnodeset(L, t, getintfromhash(t, key), val);
}


static void tracearraywrite (lua_State *L, Table *t, unsigned i, TValue *val) {
  if (luaC_tracing(L)) {
    if (*getArrTag(t, i) & BIT_ISCOLLECTABLE)
      luaC_traceobject(L, getArrVal(t, i)->gc);
    luaC_tracevalue(L, val);
  }
}

static int psetint (lua_State *L, Table *t, lua_Integer key, TValue *val) {
  lua_Unsigned u = l_castS2U(key) - 1u;
  if (u < t->asize) {
    lu_byte *tag = getArrTag(t, u);
    if (!tagisempty(*tag)) {
      tracearraywrite(L, t, cast_uint(u), val);
      fval2arr(t, u, tag, val);
      return HOK;
    }
    return ~cast_int(u);
  }
  return rawpsetint(L, t, key, val);
}


static int rawpset (lua_State *L, Table *t, const TValue *key, TValue *val) {
  luaC_tracevalue(L, key);
  switch (ttypetag(key)) {
    case LUA_VSHRSTR:
      return finishnodeset(L, t, luaH_Hgetshortstr(t, tsvalue(key)), val);
    case LUA_VLNGSTR:
      return finishnodeset(L, t, Hgetlongstr(t, tsvalue(key)), val);
    case LUA_VNUMINT: return psetint(L, t, ivalue(key), val);
    case LUA_VNIL: return HNOTFOUND;
    case LUA_VNUMFLT: {
      lua_Integer k;
      if (luaV_flttointeger(fltvalue(key), &k, F2Ieq)) /* integral index? */
        return psetint(L, t, k, val);  /* use specialized version */
      /* else... */
    }  /* FALLTHROUGH */
    default:
      return finishnodeset(L, t, getgeneric(keynodes(t, key), key, 0), val);
  }
}

/*
** Finish a raw "set table" operation, where 'hres' encodes where the
** value should have been (the result of a previous 'pset' operation).
** Beware: when using this function the caller probably need to check a
** GC barrier and invalidate the TM cache.
*/
static void rawfinishset (lua_State *L, Table *t, const TValue *key,
                                    TValue *value, int hres) {
  lua_assert(hres != HOK);
  if (hres == HNOTFOUND) {
    TValue aux;
    const TValue *actk = key;  /* actual key to insert */
    if (l_unlikely(ttisnil(key)))
      luaG_runerror(L, "table index is nil");
    else if (ttisfloat(key)) {
      lua_Number f = fltvalue(key);
      lua_Integer k;
      if (luaV_flttointeger(f, &k, F2Ieq)) {  /* is key equal to an integer? */
        setivalue(&aux, k);
        actk = &aux;  /* use the integer as the key */
      }
      else if (l_unlikely(luai_numisnan(f)))
        luaG_runerror(L, "table index is NaN");
    }
    else if (isextstr(key)) {  /* external string? */
      /* If string is short, must internalize it to be used as table key */
      TString *ts = luaS_normstr(L, tsvalue(key));
      setsvalue2s(L, L->top.p++, ts);  /* anchor 'ts' (EXTRA_STACK) */
      luaH_newkey(L, t, s2v(L->top.p - 1), value);
      L->top.p--;
      return;
    }
    luaH_newkey(L, t, actk, value);
  }
  else if (hres > 0) {  /* regular Node? */
    Node *node = luaH_hashnodes(t, 0) + hres - HFIRSTNODE;
    tracenodewrite(L, gval(node), value);
    setnodevalue(L, node, value);
  }
  else {  /* array entry */
    hres = ~hres;  /* real index */
    tracearraywrite(L, t, cast_uint(hres), value);
    obj2arr(t, cast_uint(hres), value);
  }
}


static int published (const TValue *value) {
  uintptr_t *sharing;
  if (!iscollectable(value) || (sharing = luaA_sharing(gcvalue(value))) == NULL)
    return 1;
  return (__atomic_load_n(sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED) != 0;
}

typedef struct LuaTableSlot {
  Value *payload;
  lu_byte *tag;
  Node *node;
  LuaHashPart *part;
  uint64_t revision;
} LuaTableSlot;

static int sameversion (Table *table, uint64_t sequence) {
  return __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) == sequence;
}

l_sinline int samepartversion (Table *table, uint64_t layout,
                               const LuaHashPart *part, uint64_t revision) {
  return sameversion(table, layout) && (part == NULL ||
    __atomic_load_n(&part->version, __ATOMIC_ACQUIRE) == revision);
}

/* Admission retains the image and its immutable capacity. Compare scalar
** and interned keys as raw bits. Validate before comparing a candidate that
** might require dereferencing it, and bound every link to the retained image. */
l_sinline int __attribute__((always_inline)) probehash (
    Table *table, Node *base, const TValue *key, lu_byte scalar, uint64_t layout,
    LuaTableSlot *slot, const LuaHashPart *part, uint64_t revision) {
  unsigned size = nodesteps(base);
  unsigned index, steps;
  if (scalar == LUA_VNUMINT) index = hashint(size, ivalue(key));
  else if (scalar == ctb(LUA_VSHRSTR)) index = lmod(tsvalue(key)->hash, size);
  else index = mainindex(size, key);
  for (steps = 0; steps < size; steps++) {
    Node *node = base + index;
    lu_byte tag = __atomic_load_n(&node->u.key_tt, __ATOMIC_ACQUIRE);
    int next;
    if (!scalar || tag == scalar) {
      Value bits;
      int match;
      __atomic_load(&node->u.key_val, &bits, __ATOMIC_ACQUIRE);
      if (scalar == LUA_VNUMINT)
        match = ivalueraw(bits) == ivalue(key);
      else if (scalar == ctb(LUA_VSHRSTR))
        match = memcmp(&bits, &val_(key).gc, sizeof(bits.gc)) == 0;
      else {
        if (!samepartversion(table, layout, part, revision)) return 0;
        match = equalkeybits(key, tag, bits, 0);
      }
      if (match) {
        /* The result is an address in retained storage. Its caller checks
        ** the version after reading the value or claiming the slot. */
        slot->payload = &node->u.value_;
        slot->tag = &node->u.tt_;
        slot->node = node;
        return 1;
      }
    }
    next = __atomic_load_n(&node->u.next, __ATOMIC_ACQUIRE);
    if (!samepartversion(table, layout, part, revision)) return 0;
    if (next == 0) return 1;
    index += cast_uint(next);
    if (index >= size) return 0;
  }
  return 0;
}

l_sinline int findhashslot (Table *table, const TValue *key, lu_byte scalar,
                            uint64_t layout, LuaTableSlot *slot, int writing) {
  Node *base = __atomic_load_n(&table->node, __ATOMIC_ACQUIRE);
  LuaHashDirectory *directory = luaH_hashdir(base);
  uint64_t revision = 0;
  if (directory != NULL) {
    unsigned index = scalar == LUA_VNUMINT ? integerpart(ivalue(key)) :
      scalar == ctb(LUA_VSHRSTR) ? distributedpart(tsvalue(key)->hash) :
      hashpart(key);
    LuaHashPart *part = &directory->parts[index];
    if (index != 0) {
      slot->part = part;
      if (writing) return 1;  /* probe after claiming this partition */
      revision = __atomic_load_n(&part->version, __ATOMIC_ACQUIRE);
      if (revision & 1) return 0;
      slot->revision = revision;
    }
    base = __atomic_load_n(&part->nodes, __ATOMIC_ACQUIRE);
  }
  return probehash(table, base, key, scalar, layout, slot, slot->part, revision);
}

l_sinline int locateinteger (Table *table, const TValue *key, uint64_t layout,
                             LuaTableSlot *slot, int writing) {
  unsigned asize = __atomic_load_n(&table->asize, __ATOMIC_ACQUIRE);
  unsigned k = checkrange(ivalue(key), asize);
  if (k != 0) {
    Value *array = __atomic_load_n(&table->array, __ATOMIC_ACQUIRE);
    if (!sameversion(table, layout)) return 0;
    slot->payload = array - k;
    slot->tag = cast(lu_byte *, array) + sizeof(unsigned) + k - 1;
    return 1;
  }
  return findhashslot(table, key, LUA_VNUMINT, layout, slot, writing);
}

static int __attribute__((noinline)) locategeneric (
    Table *table, const TValue *key, uint64_t layout, LuaTableSlot *slot,
    int writing) {
  lua_Integer integer;
  if (ttisnil(key)) return 1;
  if (ttisfloat(key) && luaV_flttointeger(fltvalue(key), &integer, F2Ieq)) {
    TValue normalized;
    setivalue(&normalized, integer);
    return locateinteger(table, &normalized, layout, slot, writing);
  }
  return findhashslot(table, key, 0, layout, slot, writing);
}

/* Route each access to one retained image. A writer in an independent hash
** partition resolves its slot under the partition claim. */
l_sinline int __attribute__((always_inline)) locateslot (
    Table *table, const TValue *key, lu_byte type, uint64_t layout,
    LuaTableSlot *slot, int writing) {
  slot->payload = NULL;
  slot->node = NULL;
  slot->part = NULL;
  if (type == LUA_VNUMINT) return locateinteger(table, key, layout, slot, writing);
  if (type == ctb(LUA_VSHRSTR))
    return findhashslot(table, key, ctb(LUA_VSHRSTR), layout, slot, writing);
  return locategeneric(table, key, layout, slot, writing);
}

typedef struct LuaHashGrowth {
  Node *nodes;
  LuaTableRetirement *retirement;
} LuaHashGrowth;

static void preparegrowth (lua_State *L, LuaHashGrowth *growth, int log) {
  char *storage;
  if (log > MAXHBITS || (1u << log) > MAXHSIZE)
    luaG_runerror(L, "table overflow");
  luaHS_reclaim(L, LUAHS_ASSIST);
  growth->retirement = luaM_new(L, LuaTableRetirement);
  storage = luaM_realloc_(L, NULL, 0, nodebytes(log));
  if (storage == NULL) {
    luaM_free(L, growth->retirement);
    luaM_error(L);
  }
  growth->nodes = initnodes(storage, log);
  growth->retirement->array = growth->retirement->nodes = NULL;
  growth->retirement->array_bytes = growth->retirement->node_bytes = 0;
}

static void finishgrowth (lua_State *L, LuaHashGrowth *growth) {
  if (growth->nodes != NULL)
    luaM_freemem(L, hashbase(growth->nodes), sizehash(growth->nodes));
  if (growth->retirement->nodes != NULL) luaHS_publish(L, growth->retirement);
  else luaM_free(L, growth->retirement);
}

static void invalidatemethod (lua_State *L, Table *table, TString *name) {
  if (name->shrlen >= 4 && getstr(name)[0] == '_' && getstr(name)[1] == '_') {
    unsigned tm;
    for (tm = 0; tm <= TM_EQ; ++tm)
      if (name == G(L)->tmname[tm]) {
        __atomic_fetch_and(&table->flags, cast_byte(~(1u << tm)),
                           __ATOMIC_RELAXED);
        break;
      }
  }
}

/* A held partition may mutate or publish a prepared image without crossing
** a safepoint. A negative result encodes the log capacity needed for growth. */
static int writepart (lua_State *L, Table *table, LuaHashPart *part,
                      const TValue *key, TValue *value, int raw,
                      LuaHashGrowth *growth) {
  Node *nodes = part->nodes;
  Node *position;
  const TValue *slot;
  int existing;
  uint64_t revision;
  if (ttisinteger(key)) {
    lua_Integer integer = ivalue(key);
    position = nodes + hashint(nodesteps(nodes), integer);
    slot = findinteger(position, integer);
  }
  else if (ttisshrstring(key)) {
    TString *name = tsvalue(key);
    position = hashstr(nodes, name);
    slot = findshortstr(position, name);
  }
  else {
    position = mainpositionTV(nodes, key);
    slot = findkey(position, key, 0);
  }
  existing = !isabstkey(slot);
  if (isempty(slot)) {
    if (!raw && table->metatable != NULL) return 0;
    if (ttisnil(value)) return 1;
  }
  if (existing && rawtt(slot) == rawtt(value)) {
    Node *node = nodefromval(slot);
    tracenodewrite(L, slot, value);
    __atomic_store(&node->u.value_, &value->value_, __ATOMIC_RELEASE);
    if (keyisshrstr(node)) invalidatemethod(L, table, keystrval(node));
    luaC_barrierback(L, obj2gco(table), value);
    return 1;
  }
  revision = __atomic_load_n(&part->version, __ATOMIC_RELAXED);
  __atomic_store_n(&part->version, revision + 1, __ATOMIC_RELEASE);
  luaC_tracevalue(L, key);
  luaC_tracevalue(L, value);
  if (existing) {
    Node *node = nodefromval(slot);
    tracenodewrite(L, slot, value);
    if (keyisshrstr(node)) invalidatemethod(L, table, keystrval(node));
    setnodevalue(L, node, value);
  }
  else {
    if (!insertat(L, nodes, key, value, position)) {
      unsigned i, live = 1, size = luaH_nodesize(nodes);
      int needed;
      for (i = 0; i < size; ++i)
        live += !isempty(gval(nodes + i));
      needed = luaO_ceillog2(live);
      if (growth == NULL || growth->nodes->u.lsize < needed) {
        __atomic_store_n(&part->version, revision + 2, __ATOMIC_RELEASE);
        return -1 - needed;
      }
      for (i = 0; i < size; ++i) {
        Node *entry = nodes + i;
        if (!isempty(gval(entry))) {
          TValue index;
          int inserted;
          getnodekey(L, &index, entry);
          luaC_tracevalue(L, &index);
          luaC_tracevalue(L, gval(entry));
          inserted = insertkey(NULL, growth->nodes, &index, gval(entry));
          lua_assert(inserted);
          UNUSED(inserted);
        }
      }
      growth->retirement->nodes = size != 0 ? hashbase(nodes) : NULL;
      growth->retirement->node_bytes = sizehash(nodes);
      nodes = growth->nodes;
      growth->nodes = NULL;
      {
        int inserted = insertkey(NULL, nodes, key, value);
        lua_assert(inserted);
        UNUSED(inserted);
      }
      __atomic_store_n(&part->nodes, nodes, __ATOMIC_RELEASE);
    }
    if (ttisshrstring(key)) invalidatemethod(L, table, tsvalue(key));
    luaC_barrierback(L, obj2gco(table), key);
  }
  luaC_barrierback(L, obj2gco(table), value);
  __atomic_store_n(&part->version, revision + 2, __ATOMIC_RELEASE);
  return 1;
}

static LuaHashPart *keypart (Table *table, const TValue *key) {
  LuaHashDirectory *directory = luaH_hashdir(
    __atomic_load_n(&table->node, __ATOMIC_ACQUIRE));
  unsigned index;
  if (directory == NULL) return NULL;
  index = hashpart(key);
  return index != 0 ? &directory->parts[index] : NULL;
}

static int partassignment (lua_State *L, Table *table, LuaHashPart *part,
                           const TValue *key, TValue *value, int raw) {
  LuaHashGrowth growth;
  int result = writepart(L, table, part, key, value, raw, NULL);
  if (result >= 0) return result;
  preparegrowth(L, &growth, -1 - result);
  result = writepart(L, table, part, key, value, raw, &growth);
  lua_assert(result >= 0);
  finishgrowth(L, &growth);
  return result;
}

static int __attribute__((noinline)) sharedpartset (
    lua_State *L, Table *table, LuaHashPart *part, const TValue *key,
    TValue *value, int raw, uint64_t layout, LuaHashPart **blocked,
    LuaHashGrowth *growth) {
  TValue normalized;
  lua_Integer integer;
  int result;
  if ((layout & 1) || part == NULL || isextstr(key)) return 0;
  if (ttisfloat(key)) {
    if (luai_numisnan(fltvalue(key))) return 0;
    if (luaV_flttointeger(fltvalue(key), &integer, F2Ieq)) {
      setivalue(&normalized, integer);
      key = &normalized;
    }
  }
  result = luaH_claimpart(L, table, part, layout);
  if (result <= 0) {
    if (result < 0 && blocked != NULL) *blocked = part;
    return 0;
  }
  result = writepart(L, table, part, key, value, raw, growth);
  luaH_releasepart(L, table, part);
  if (result < 0 && blocked != NULL) *blocked = part;
  return result;
}

l_sinline uint64_t sharedlayout (lua_State *L, Table *table,
                                 const TValue *key, const TValue *value) {
  if (luaW_stopped(&G(L)->world) ||
      (gettableflags(table) & LUA_TABLE_STRING_INDEX) ||
      !published(key) || !published(value) || !luaH_strongaccess(L, table))
    return 1;
  return __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
}

typedef struct LuaTableWriteRetry {
  LuaTableWriter *writer;
  LuaHashPart *part;
} LuaTableWriteRetry;

/* Keep the shared snapshot's frame out of ordinary private assignments. */
static int __attribute__((noinline)) sharedset (lua_State *L, Table *table,
                       const TValue *key, TValue *value, int raw,
                       LuaTableWriteRetry *retry) {
  LuaTableSlot slot;
  LuaTableWriter *writer;
  Node *node;
  uint64_t layout = sharedlayout(L, table, key, value);
  if (layout & 1) return 0;
  if (!locateslot(table, key, rawtt(key), layout, &slot, 1)) return 0;
  if (slot.part != NULL)
    return sharedpartset(L, table, slot.part, key, value, raw, layout,
                         retry != NULL ? &retry->part : NULL, NULL);
  if (slot.payload == NULL) return 0;
  node = slot.node;
  writer = luaH_claimwriter(L, table, slot.payload, layout);
  if (writer == NULL) {
    LuaTableWriter *row = luaH_writer(G(L), slot.payload);
    if (retry != NULL &&
        __atomic_load_n(&row->owner, __ATOMIC_SEQ_CST) == table)
      retry->writer = row;
    return 0;
  }
  /* Key retirement is a layout operation. A missing value may also invoke
  ** __newindex; resolve that under ordinary ordered metatable ownership. */
  if ((!raw && tagisempty(*slot.tag) && table->metatable != NULL) ||
      (node != NULL && ttisnil(value) && keyiscollectable(node)) ||
      (*slot.tag != rawtt(value) &&
       !luaH_beginsplit(table, writer, slot.payload, layout))) {
    luaH_releasewriter(L, table, writer);
    return 0;
  }
  luaC_tracevalue(L, key);
  if (node != NULL && luaC_tracing(L) && keyiscollectable(node))
    luaC_traceobject(L, gckey(node));
  luaH_elementstore(L, table, slot.payload, slot.tag, value);
  if (node != NULL && keyisshrstr(node))
    invalidatemethod(L, table, keystrval(node));
  luaH_releasewriter(L, table, writer);
  return 1;
}

/* Under the private table's mutation boundary, shade the displaced edge
** here. The common assignment barrier shades the incoming value before
** reopening the layout to the concurrent marker. */
l_sinline int privatestoreslot (lua_State *L, Table *table, const TValue *key,
                               TValue *value, int raw, int tracing) {
  unsigned index = keyinarray(table, key);
  int result;
  if (index != 0) {
    lu_byte *tag = getArrTag(table, index - 1);
    if (tagisempty(*tag) && !raw && table->metatable != NULL) return 0;
    if (tracing && (*tag & BIT_ISCOLLECTABLE))
      luaC_traceobject(L, getArrVal(table, index - 1)->gc);
    fval2arr(table, index - 1, tag, value);
    return 1;
  }
  if (ttisshrstring(key)) {
    TString *name = tsvalue(key);
    Node *position = hashstr(table->node, name);
    const TValue *slot = findshortstr(position, name);
    if (isempty(slot) && !raw && table->metatable != NULL) return 0;
    if (isabstkey(slot)) {
      if (ttisnil(value)) return 1;
      if (!insertat(L, table->node, key, value, position)) return 0;
      luaC_barrierback(L, obj2gco(table), key);
      return 1;
    }
    /* Replacing a value leaves its key in place. Deletion removes both. */
    if (tracing) {
      if (ttisnil(value)) luaC_traceobject(L, obj2gco(name));
      luaC_tracevalue(L, slot);
    }
    if (isempty(slot) && !ttisnil(value))
      luaC_barrierback(L, obj2gco(table), key);
    setnodevalue(NULL, nodefromval(slot), value);
    return 1;
  }
  result = rawpset(L, table, key, value);
  if (result == HOK) return 1;
  if (!raw && table->metatable != NULL) return 0;
  if (result == HNOTFOUND) {
    if (ttisnil(key) || ttisfloat(key) || isextstr(key)) return 0;
    if (!ttisnil(value)) {
      if (!insertkey(L, table->node, key, value)) return 0;
      luaC_barrierback(L, obj2gco(table), key);
    }
  }
  else rawfinishset(L, table, key, value, result);
  return 1;
}

/* The current gate is the private graph's only mutator. With no allocation,
** wait or callback, its live operands need no additional native root records.
** The collector only reads strong storage and validates our sequence. */
int luaH_tryset (lua_State *L, Table *t, const TValue *key, TValue *value,
                 int raw) {
  uint64_t sequence;
  uintptr_t sharing = __atomic_load_n(&t->sharing, __ATOMIC_ACQUIRE);
  int tracing, done;
  if (sharing & LUA_OBJECT_SHARED)
    return sharedset(L, t, key, value, raw, NULL) > 0;
  if (sharing != cast(uintptr_t, L)) return 0;
  sequence = __atomic_load_n(&t->sequence, __ATOMIC_RELAXED);
  if (sequence & 1) return 0;  /* an enclosing borrowed operation owns it */
  /* Only the owner and a concurrent marker can read these slots. Sweep
  ** reads the stable storage pointers and capacities. A new trace requires
  ** quiescence, and the write completes before reaching a safepoint. */
  tracing = luaC_tracing(L);
  if (tracing) {
    if (!luaH_strongaccess(L, t)) return 0;
    __atomic_store_n(&t->sequence, sequence + 1, __ATOMIC_RELEASE);
  }
  done = privatestoreslot(L, t, key, value, raw, tracing);
  if (done) {
    settableflags(t, gettableflags(t) & cast_byte(~maskflags));
    luaC_barrierback(L, obj2gco(t), value);
  }
  /* Failure has not changed any key, value or image. Restore that version;
  ** the rooted fallback will publish the operation's single committed image. */
  if (tracing)
    __atomic_store_n(&t->sequence, sequence + (done ? LUA_TABLE_VERSION_STEP : 0),
                     __ATOMIC_RELEASE);
  return done;
}

/* The receiver and arguments are rooted and already published. Each retry
** discards its old storage view before a possible wait or stack relocation. */
int luaH_setshared (lua_State *L, Table *table, const TValue *key,
                     TValue *value, int raw) {
  for (;;) {
    LuaTableWriteRetry retry = {NULL, NULL};
    int result;
    uint64_t sequence = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
    if (luaH_waitlayout(L, table)) continue;
    result = sharedset(L, table, key, value, raw, &retry);
    if (result > 0) return 1;
    if (result < 0) {
      LuaHashGrowth growth;
      LuaHashPart *part = retry.part;
      retry.part = NULL;
      preparegrowth(L, &growth, -1 - result);
      result = sharedpartset(L, table, part, key, value, raw,
                             sharedlayout(L, table, key, value), &retry.part, &growth);
      finishgrowth(L, &growth);
      if (result > 0) return 1;
      if (result < 0) continue;  /* another writer changed the needed capacity */
    }
    if (retry.part != NULL) luaH_waitpart(L, retry.part);
    else if (retry.writer != NULL) luaH_waitwriters(L, table, retry.writer);
    else {
      if (luaH_waitlayout(L, table)) continue;
      if (__atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) == sequence)
        return 0;
    }
  }
}

/* The ordinary no-metatable insertion needs no second ownership or lookup. */
int luaH_assignowned (lua_State *L, Table *t, const TValue *key, TValue *value,
                       int raw) {
  int hres;
  LuaHashPart *part;
  if (gettableflags(t) & LUA_TABLE_STRING_INDEX)
    luaA_invalidatestringindex(L, t);
  if (__atomic_load_n(&t->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED) {
    luaA_sharevalue(L, key);
    luaA_sharevalue(L, value);
  }
  invalidateTMcache(t);
  part = keypart(t, key);
  if (part != NULL) {
    TValue normalized;
    LuaNativeRoot anchor;
    lua_Integer integer;
    int assigned;
    if (ttisfloat(key)) {
      if (luai_numisnan(fltvalue(key))) {
        if (!raw && t->metatable != NULL) return 0;
        luaG_runerror(L, "table index is NaN");
      }
      if (luaV_flttointeger(fltvalue(key), &integer, F2Ieq)) {
        setivalue(&normalized, integer);
        key = &normalized;
      }
    }
    if (!isextstr(key)) return partassignment(L, t, part, key, value, raw);
    luaR_pushroot(L, &anchor);
    setsvalue(L, &anchor.value, luaS_normstr(L, tsvalue(key)));
    luaC_tracevalue(L, &anchor.value);
    assigned = partassignment(L, t, part, &anchor.value, value, raw);
    luaR_poproot(L, &anchor);
    return assigned;
  }
  hres = rawpset(L, t, key, value);
  if (hres != HOK) {
    if (!raw && t->metatable != NULL) return 0;
    rawfinishset(L, t, key, value, hres);
  }
  invalidateTMcache(t);
  luaC_barrierback(L, obj2gco(t), value);
  return 1;
}


/*
** Ditto for a GC barrier. (No need to invalidate the TM cache, as
** integers cannot be keys to metamethods.)
*/
static void rawsetint (lua_State *L, Table *t, lua_Integer key, TValue *value) {
  unsigned ik = ikeyinarray(t, key);
  if (luaH_hashdir(t->node) != NULL && key <= 0) {
    TValue index;
    setivalue(&index, key);
    luaH_assignowned(L, t, &index, value, 1);
    return;
  }
  if (__atomic_load_n(&t->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED)
    luaA_sharevalue(L, value);
  if (ik > 0) {
    tracearraywrite(L, t, ik - 1, value);
    obj2arr(t, ik - 1, value);
  }
  else {
    int ok = rawfinishnodeset(L, getintfromhash(t, key), value);
    if (!ok) {
      TValue k;
      setivalue(&k, key);
      luaH_newkey(L, t, &k, value);
    }
  }
}


/*
** Try to find a boundary in the hash part of table 't'. From the
** caller, we know that 'asize + 1' is present. We want to find a larger
** key that is absent from the table, so that we can do a binary search
** between the two keys to find a boundary. We keep doubling 'j' until
** we get an absent index.  If the doubling would overflow, we try
** LUA_MAXINTEGER. If it is absent, we are ready for the binary search.
** ('j', being max integer, is larger or equal to 'i', but it cannot be
** equal because it is absent while 'i' is present.) Otherwise, 'j' is a
** boundary. ('j + 1' cannot be a present integer key because it is not
** a valid integer in Lua.)
** About 'rnd': If we used a fixed algorithm, a bad actor could fill
** a table with only the keys that would be probed, in such a way that
** a small table could result in a huge length. To avoid that, we use
** the state's seed as a source of randomness. For the first probe,
** we "randomly double" 'i' by adding to it a random number roughly its
** width.
*/
static lua_Unsigned hash_search (lua_State *L, Table *t, unsigned asize) {
  lua_Unsigned i = asize + 1;  /* caller ensures t[i] is present */
  unsigned rnd = G(L)->seed;
  int n = (asize > 0) ? luaO_ceillog2(asize) : 0;  /* width of 'asize' */
  unsigned mask = (1u << n) - 1;  /* 11...111 with the width of 'asize' */
  unsigned incr = (rnd & mask) + 1;  /* first increment (at least 1) */
  lua_Unsigned j = (incr <= l_castS2U(LUA_MAXINTEGER) - i) ? i + incr : i + 1;
  rnd >>= n;  /* used 'n' bits from 'rnd' */
  while (!hashkeyisempty(t, j)) {  /* repeat until an absent t[j] */
    i = j;  /* 'i' is a present index */
    if (j <= l_castS2U(LUA_MAXINTEGER)/2 - 1) {
      j = j*2 + (rnd & 1);  /* try again with 2j or 2j+1 */
      rnd >>= 1;
    }
    else {
      j = LUA_MAXINTEGER;
      if (hashkeyisempty(t, j))  /* t[j] not present? */
        break;  /* 'j' now is an absent index */
      else  /* weird case */
        return j;  /* well, max integer is a boundary... */
    }
  }
  /* i < j  &&  t[i] present  &&  t[j] absent */
  while (j - i > 1u) {  /* do a binary search between them */
    lua_Unsigned m = (i + j) / 2;
    if (hashkeyisempty(t, m)) j = m;
    else i = m;
  }
  return i;
}


static unsigned int binsearch (Table *array, unsigned int i, unsigned int j) {
  lua_assert(i <= j);
  while (j - i > 1u) {  /* binary search */
    unsigned int m = (i + j) / 2;
    if (arraykeyisempty(array, m)) j = m;
    else i = m;
  }
  return i;
}


/* return a border, saving it as a hint for next call */
static lua_Unsigned newhint (Table *t, unsigned hint) {
  lua_assert(hint <= t->asize);
  *lenhint(t) = hint;
  return hint;
}


/*
** Try to find a border in table 't'. (A 'border' is an integer index
** such that t[i] is present and t[i+1] is absent, or 0 if t[1] is absent,
** or 'maxinteger' if t[maxinteger] is present.)
** If there is an array part, try to find a border there. First try
** to find it in the vicinity of the previous result (hint), to handle
** cases like 't[#t + 1] = val' or 't[#t] = nil', that move the border
** by one entry. Otherwise, do a binary search to find the border.
** If there is no array part, or its last element is non empty, the
** border may be in the hash part.
*/
static lua_Unsigned rawgetn (lua_State *L, Table *t) {
  unsigned asize = t->asize;
  if (asize > 0) {  /* is there an array part? */
    const unsigned maxvicinity = 4;
    unsigned limit = *lenhint(t);  /* start with the hint */
    if (limit == 0)
      limit = 1;  /* make limit a valid index in the array */
    if (arraykeyisempty(t, limit)) {  /* t[limit] empty? */
      /* there must be a border before 'limit' */
      unsigned i;
      /* look for a border in the vicinity of the hint */
      for (i = 0; i < maxvicinity && limit > 1; i++) {
        limit--;
        if (!arraykeyisempty(t, limit))
          return newhint(t, limit);  /* 'limit' is a border */
      }
      /* t[limit] still empty; search for a border in [0, limit) */
      return newhint(t, binsearch(t, 0, limit));
    }
    else {  /* 'limit' is present in table; look for a border after it */
      unsigned i;
      /* look for a border in the vicinity of the hint */
      for (i = 0; i < maxvicinity && limit < asize; i++) {
        limit++;
        if (arraykeyisempty(t, limit))
          return newhint(t, limit - 1);  /* 'limit - 1' is a border */
      }
      if (arraykeyisempty(t, asize)) {  /* last element empty? */
        /* t[limit] not empty; search for a border in [limit, asize) */
        return newhint(t, binsearch(t, limit, asize));
      }
    }
    /* last element non empty; set a hint to speed up finding that again */
    /* (keys in the hash part cannot be hints) */
    *lenhint(t) = asize;
  }
  /* no array part or t[asize] is not empty; check the hash part */
  lua_assert(asize == 0 || !arraykeyisempty(t, asize));
  if (isdummy(t) || hashkeyisempty(t, asize + 1))
    return asize;  /* 'asize + 1' is empty */
  else  /* 'asize + 1' is also non empty */
    return hash_search(L, t, asize);
}



static int readsequence (lua_State *L, Table *table, uint64_t *sequence) {
  *sequence = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
  return !(*sequence & 1) &&
    !(L != NULL && luaC_tracing(L) &&
      luaC_weakmode(luaC_tablestate(table)));
}

l_sinline int readslot (global_State *g, lua_State *L, Table *table,
                        const LuaTableSlot *slot, uint64_t sequence,
                        TValue *result, lu_byte *tag) {
  TValue value;
  if (slot->payload == NULL) {
    *tag = LUA_VABSTKEY;
    return 1;
  }
  if (slot->part != NULL) {
    luaH_readvalue(g, slot->payload, slot->tag, &value, 0);
    if (!samepartversion(table, sequence, slot->part, slot->revision)) return 0;
  }
  else if (!luaH_readvalue(g, slot->payload, slot->tag, &value, sequence) ||
           !sameversion(table, sequence)) return 0;
  if (slot->node != NULL) *tag = finishnodeget(&value, result);
  else {
    *tag = rawtt(&value);
    if (!tagisempty(*tag)) setobj(L, result, &value);
  }
  return 1;
}

static int __attribute__((noinline)) readshortfallback (
    global_State *g, lua_State *L, Table *table, TString *key,
    TValue *result, lu_byte *tag, uint64_t sequence) {
  LuaTableSlot slot;
  TValue index;
  setsvalue(L, &index, key);
  return locateslot(table, &index, ctb(LUA_VSHRSTR), sequence, &slot, 0) &&
         readslot(g, L, table, &slot, sequence, result, tag);
}

/* Interned-key hits in a single image usually need only the first node.
** Check the established layout version before interpreting the copied value. */
l_sinline int readshortslot (global_State *g, lua_State *L, Table *table,
                             TString *key, TValue *result, lu_byte *tag,
                             uint64_t *sequence) {
  Node *nodes;
  if (!readsequence(L, table, sequence)) return 0;
  nodes = __atomic_load_n(&table->node, __ATOMIC_ACQUIRE);
  if (!(*sequence & LUA_TABLE_SPLIT_VALUES) && luaH_hashdir(nodes) == NULL) {
    Node *node = hashstr(nodes, key);
    if (__atomic_load_n(&node->u.key_tt, __ATOMIC_ACQUIRE) == ctb(LUA_VSHRSTR) &&
        __atomic_load_n(&node->u.key_val.gc, __ATOMIC_ACQUIRE) == obj2gco(key)) {
      TValue value;
      luaH_readvalue(g, &node->u.value_, &node->u.tt_, &value, 0);
      if (!sameversion(table, *sequence)) return 0;
      *tag = finishnodeget(&value, result);
      return 1;
    }
  }
  return readshortfallback(g, L, table, key, result, tag, *sequence);
}

int luaH_readshortstr (global_State *g, lua_State *L, Table *table, TString *key,
                       TValue *result) {
  lu_byte tag;
  uint64_t sequence;
  if (!readshortslot(g, L, table, key, result, &tag, &sequence)) return 0;
  if (tagisempty(tag)) setnilvalue(result);
  return 1;
}

/* These are raw pointer representations until the caller validates the table
** sequence. Empty keys are retired by writers; scanning never edits storage. */
l_sinline unsigned __attribute__((always_inline)) tracechunk (
    global_State *g, const Table *view, unsigned first, unsigned count,
    int hash, GCObject **edges) {
  unsigned i, found = 0;
  for (i = first; i < first + count; i++) {
    const Node *node = hash ? gnode(view, i) : NULL;
    const Value *payload = hash ? &node->u.value_ : getArrVal(view, i);
    const lu_byte *tag = hash ? &node->u.tt_ : getArrTag(view, i);
    LuaTableWriter *writer = g != NULL ? luaH_writer(g, payload) : NULL;
    uint64_t sequence = 0;
    lu_byte valuetag;
    if (writer != NULL) {
      sequence = __atomic_load_n(&writer->sequence, __ATOMIC_ACQUIRE);
      if ((sequence & 1) &&
          __atomic_load_n(&writer->slot, __ATOMIC_ACQUIRE) == cast(uintptr_t, payload))
        return UINT_MAX;
    }
    /* GC needs raw edges, not numeric payloads or complete mutator values.
    ** They remain representations until both row and layout are validated. */
    valuetag = __atomic_load_n(tag, __ATOMIC_ACQUIRE);
    if (hash && !tagisempty(valuetag)) {
      lu_byte keytag = __atomic_load_n(&node->u.key_tt, __ATOMIC_ACQUIRE);
      if (keytag & BIT_ISCOLLECTABLE)
        __atomic_load(&node->u.key_val.gc, &edges[found++], __ATOMIC_ACQUIRE);
    }
    if (valuetag & BIT_ISCOLLECTABLE)
      __atomic_load(&payload->gc, &edges[found++], __ATOMIC_ACQUIRE);
    if (writer != NULL &&
        __atomic_load_n(&writer->sequence, __ATOMIC_ACQUIRE) != sequence)
      return UINT_MAX;
  }
  return found;
}

unsigned luaH_tracechunk (global_State *g, const Table *view, unsigned first,
                           unsigned count, int hash, GCObject **edges) {
  /* Select the slot layout once when there are no split writes to validate. */
  if (g == NULL) {
    if (hash) return tracechunk(NULL, view, first, count, 1, edges);
    else return tracechunk(NULL, view, first, count, 0, edges);
  }
  return tracechunk(g, view, first, count, hash, edges);
}

/* Keep roots and parking out of private read frames. A caller's result may
** name a Lua register, so rebase it after waits which can relocate the stack. */
static lu_byte __attribute__((noinline)) getrooted (lua_State *L, Table *table,
                                      const TValue *key, TValue *result) {
  LuaNativeRoot rooted_key, owner, value;
  LuaObjectScope scope;
  uintptr_t address = cast(uintptr_t, result);
  uintptr_t base = cast(uintptr_t, L->stack.p);
  uintptr_t limit = cast(uintptr_t, L->stack_last.p + EXTRA_STACK);
  ptrdiff_t offset = address >= base && address < limit
    ? cast(ptrdiff_t, address - base) : -1;
  unsigned retries;
  lu_byte tag;
  luaR_pushroot(L, &rooted_key);
  luaR_setroot(L, &rooted_key, key);
  luaR_pushroot(L, &owner);
  sethvalue(L, &owner.value, table);
  luaC_tracepointer(L, table);
  luaR_pushroot(L, &value);
  for (retries = 0; retries < 8; retries++) {
    LuaTableSlot slot;
    LuaTableWriter *writer;
    TValue snapshot;
    uint64_t sequence;
    if (!luaH_strongaccess(L, table)) break;
    if (luaH_waitlayout(L, table)) continue;
    if (!readsequence(L, table, &sequence)) {
      if (luaA_lockowner(&table->access) == luaX_token(L))
        break;
      continue;
    }
    if (locateslot(table, &rooted_key.value, rawtt(&rooted_key.value),
                   sequence, &slot, 0) &&
        readslot(G(L), L, table, &slot, sequence, &snapshot, &tag)) {
      if (!tagisempty(tag)) luaR_setroot(L, &value, &snapshot);
      goto done;
    }
    if (slot.part != NULL) {
      luaH_waitpart(L, slot.part);
      continue;
    }
    if (slot.payload == NULL) continue;
    writer = luaH_claimwriter(L, table, slot.payload, sequence);
    if (writer != NULL) {
      /* A failed optimistic read needs the element guard, not ownership of
      ** every element. Root the stable value before releasing that guard. */
      __atomic_load(slot.payload, &snapshot.value_, __ATOMIC_ACQUIRE);
      snapshot.tt_ = __atomic_load_n(slot.tag, __ATOMIC_ACQUIRE);
      tag = slot.node != NULL ? finishnodeget(&snapshot, &snapshot)
                             : rawtt(&snapshot);
      if (!tagisempty(tag)) luaR_setroot(L, &value, &snapshot);
      luaH_releasewriter(L, table, writer);
      goto done;
    }
    writer = luaH_writer(G(L), slot.payload);
    if (__atomic_load_n(&writer->owner, __ATOMIC_SEQ_CST) == table)
      luaH_waitwriters(L, table, writer);
  }
  luaA_begin(L, obj2gco(table), &scope);
  tag = luaH_getowned(table, &rooted_key.value, &value.value);
  if (!tagisempty(tag)) luaC_tracevalue(L, &value.value);
  luaA_end(L, &scope);
 done:
  if (!tagisempty(tag)) {
    luaC_tracevalue(L, &value.value);
    if (offset >= 0) result = s2v(restorestack(L, offset));
    setobj(L, result, &value.value);
  }
  luaR_poproot(L, &value);
  luaR_poproot(L, &owner);
  luaR_poproot(L, &rooted_key);
  return tagisempty(tag) ? LUA_VABSTKEY : tag;
}

/* A miss completes indexing if the same view also has no metatable.
** An unresolved miss must leave the key intact if it aliases the result. */
static lu_byte finishread (lua_State *L, Table *t, TValue *res, lu_byte tag,
                            int local, uint64_t sequence) {
  if (!tagisempty(tag)) {
    luaC_tracevalue(L, res);
    return tag;
  }
  if (__atomic_load_n(&t->metatable, __ATOMIC_ACQUIRE) == NULL &&
      (local || sameversion(t, sequence))) {
    setnilvalue(res);
    return LUA_VNIL;
  }
  return LUA_VABSTKEY;
}

l_sinline int __attribute__((always_inline)) trygetshared (
    lua_State *L, Table *t, const TValue *key, lu_byte type, TValue *res,
    lu_byte *tag) {
  LuaTableSlot slot;
  uint64_t sequence;
  if (!readsequence(L, t, &sequence)) return 0;
  if (!locateslot(t, key, type, sequence, &slot, 0) ||
      !readslot(G(L), L, t, &slot, sequence, res, tag)) return 0;
  *tag = finishread(L, t, res, *tag, 0, sequence);
  return 1;
}

int luaH_tryget (lua_State *L, Table *t, const TValue *key, TValue *res,
                 lu_byte *tag) {
  if (luaA_localtable(L, t)) {
    *tag = finishread(L, t, res, luaH_getowned(t, key, res), 1, 0);
    return 1;
  }
  if (ttisshrstring(key)) return luaH_trygetshortstr(L, t, tsvalue(key), res, tag);
  return trygetshared(L, t, key, rawtt(key), res, tag);
}

/* Complete mutator operations. Encoded slots never cross an unlock. */
lu_byte luaH_get (lua_State *L, Table *t, const TValue *key, TValue *res) {
  lu_byte tag;
  if (luaH_tryget(L, t, key, res, &tag)) return tag;
  return getrooted(L, t, key, res);
}

int luaH_trygetint (lua_State *L, Table *t, lua_Integer key, TValue *res,
                     lu_byte *tag) {
  TValue index;
  if (luaA_localtable(L, t)) {
    *tag = finishread(L, t, res, rawgetint(t, key, res), 1, 0);
    return 1;
  }
  setivalue(&index, key);
  return trygetshared(L, t, &index, LUA_VNUMINT, res, tag);
}

lu_byte luaH_getint (lua_State *L, Table *t, lua_Integer key, TValue *res) {
  lu_byte tag;
  TValue index;
  if (luaH_trygetint(L, t, key, res, &tag)) return tag;
  setivalue(&index, key);
  return getrooted(L, t, &index, res);
}

int luaH_trygetshortstr (lua_State *L, Table *t, TString *key, TValue *res,
                           lu_byte *tag) {
  uint64_t sequence;
  lua_assert(strisshr(key));
  if (luaA_localtable(L, t)) {
    Node *nodes = t->node;
    lua_assert(luaH_hashdir(nodes) == NULL);
    *tag = finishread(L, t, res,
                     finishnodeget(findshortstr(hashstr(nodes, key), key), res),
                     1, 0);
    return 1;
  }
  if (!readshortslot(G(L), L, t, key, res, tag, &sequence)) return 0;
  *tag = finishread(L, t, res, *tag, 0, sequence);
  return 1;
}

static int trygetstr (lua_State *L, Table *t, TString *key, TValue *res,
                       lu_byte *tag) {
  TValue index;
  if (strisshr(key)) return luaH_trygetshortstr(L, t, key, res, tag);
  if (luaA_localtable(L, t)) {
    *tag = finishread(L, t, res, finishnodeget(Hgetlongstr(t, key), res), 1, 0);
    return 1;
  }
  setsvalue(L, &index, key);
  return trygetshared(L, t, &index, ctb(LUA_VLNGSTR), res, tag);
}

lu_byte luaH_getstr (lua_State *L, Table *t, TString *key, TValue *res) {
  lu_byte tag;
  TValue index;
  if (trygetstr(L, t, key, res, &tag)) return tag;
  setsvalue(L, &index, key);
  return getrooted(L, t, &index, res);
}

void luaH_set (lua_State *L, Table *t, const TValue *key, TValue *value) {
  LuaNativeRoot rooted_key, rooted_value, owner;
  LuaObjectScope scope;
  if (luaH_tryset(L, t, key, value, 1)) return;
  luaR_pushroot(L, &rooted_key);
  luaR_setroot(L, &rooted_key, key);
  luaR_pushroot(L, &rooted_value);
  luaR_setroot(L, &rooted_value, value);
  luaR_pushroot(L, &owner);
  sethvalue(L, &owner.value, t);
  luaC_tracepointer(L, t);
  if (__atomic_load_n(&t->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED) {
    luaA_sharevalue(L, &rooted_key.value);
    luaA_sharevalue(L, &rooted_value.value);
    if (luaH_setshared(L, t, &rooted_key.value, &rooted_value.value, 1)) goto done;
  }
  luaA_begin(L, obj2gco(t), &scope);
  luaH_assignowned(L, t, &rooted_key.value, &rooted_value.value, 1);
  luaA_end(L, &scope);
 done:
  luaR_poproot(L, &owner);
  luaR_poproot(L, &rooted_value);
  luaR_poproot(L, &rooted_key);
}

void luaH_setint (lua_State *L, Table *t, lua_Integer key, TValue *value) {
  LuaNativeRoot rooted_value, owner;
  LuaObjectScope scope;
  TValue index;
  setivalue(&index, key);
  if (luaH_tryset(L, t, &index, value, 1)) return;
  luaR_pushroot(L, &rooted_value);
  luaR_setroot(L, &rooted_value, value);
  luaR_pushroot(L, &owner);
  sethvalue(L, &owner.value, t);
  luaC_tracepointer(L, t);
  if (__atomic_load_n(&t->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED) {
    luaA_sharevalue(L, &rooted_value.value);
    if (luaH_setshared(L, t, &index, &rooted_value.value, 1)) goto done;
  }
  luaA_begin(L, obj2gco(t), &scope);
  rawsetint(L, t, key, &rooted_value.value);
  luaC_barrierback(L, obj2gco(t), &rooted_value.value);
  luaA_end(L, &scope);
 done:
  luaR_poproot(L, &owner);
  luaR_poproot(L, &rooted_value);
}

void luaH_resize (lua_State *L, Table *t, unsigned asize, unsigned hsize) {
  LuaObjectScope scope;
  luaA_begin(L, obj2gco(t), &scope);
  rawresize(L, t, asize, hsize);
  luaA_end(L, &scope);
}

void luaH_resizearray (lua_State *L, Table *t, unsigned asize) {
  LuaObjectScope scope;
  luaA_begin(L, obj2gco(t), &scope);
  rawresizearray(L, t, asize);
  luaA_end(L, &scope);
}

int luaH_next (lua_State *L, Table *t, StkId key) {
  LuaObjectScope scope;
  int result;
  luaA_begin(L, obj2gco(t), &scope);
  result = rawnext(L, t, key);
  if (result) {
    luaC_tracevalue(L, s2v(key));
    luaC_tracevalue(L, s2v(key + 1));
  }
  luaA_end(L, &scope);
  return result;
}

lua_Unsigned luaH_getn (lua_State *L, Table *t) {
  LuaObjectScope scope;
  lua_Unsigned result;
  luaA_begin(L, obj2gco(t), &scope);
  result = rawgetn(L, t);
  luaA_end(L, &scope);
  return result;
}

#if defined(LUA_DEBUG)

/* export this function for the test library */

Node *luaH_mainposition (const Table *t, const TValue *key) {
  return mainpositionTV(keynodes(t, key), key);
}

#endif
