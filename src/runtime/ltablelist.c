/* Bytecode list constructors. See lua.h for license. */
#define LUA_CORE
#include "lprefix.h"
#include "laccess.h"
#include "ldo.h"
#include "ltable.h"

/* Store one constructor batch. During tracing the caller closes the table
** version to readers and each displaced edge must be shaded before the store. */
l_sinline void storelist (lua_State *L, Table *t, StkId base,
                          unsigned count, unsigned last, int tracing) {
  for (; count != 0; count--, last--) {
    const TValue *value = s2v(base + count);
    unsigned index = last - 1;
    if (tracing && (*getArrTag(t, index) & BIT_ISCOLLECTABLE))
      luaC_traceobject(L, getArrVal(t, index)->gc);
    obj2arr(t, index, value);
    luaC_barrierback(L, obj2gco(t), value);
  }
}

static void __attribute__((noinline)) setlistrooted (lua_State *L, Table *t,
    StkId base, unsigned count, unsigned last, uintptr_t sharing) {
  LuaObjectScope scope;
  ptrdiff_t offset = savestack(L, base);
  unsigned i;
  /* A hook can publish a constructor while its expressions are still
  ** running. Share incoming graphs before taking the receiver's ownership. */
  if (sharing & LUA_OBJECT_SHARED) {
    for (i = 1; i <= count; i++)
      luaA_sharevalue(L, s2v(restorestack(L, offset) + i));
  }
  luaA_begin(L, obj2gco(t), &scope);
  if (last > t->asize) luaH_resizearray(L, t, last);
  storelist(L, t, restorestack(L, offset), count, last, luaC_tracing(L));
  luaA_end(L, &scope);
}

void luaH_setlist (lua_State *L, Table *t, StkId base,
                   unsigned count, unsigned last) {
  uintptr_t sharing = __atomic_load_n(&t->sharing, __ATOMIC_ACQUIRE);
  if (sharing == cast(uintptr_t, L) && last <= t->asize) {
    uint64_t sequence = __atomic_load_n(&t->sequence, __ATOMIC_RELAXED);
    int tracing = luaC_tracing(L);
    /* A private constructor with enough space cannot allocate or park.
    ** Match private slot writes and exclude only the concurrent marker. */
    if (!(sequence & 1) && (!tracing || luaC_strongtable(G(L), t))) {
      if (tracing) __atomic_store_n(&t->sequence, sequence + 1, __ATOMIC_RELEASE);
      storelist(L, t, base, count, last, tracing);
      if (tracing)
        __atomic_store_n(&t->sequence, sequence + LUA_TABLE_VERSION_STEP,
                         __ATOMIC_RELEASE);
      return;
    }
  }
  setlistrooted(L, t, base, count, last, sharing);
}
