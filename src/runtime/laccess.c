/*
** Scoped access to shared Lua objects.
** See Copyright Notice in lua.h
*/
#define laccess_c
#define LUA_CORE
#include "lprefix.h"

#include "laccess.h"
#include "lgc.h"
#include "ldo.h"
#include "lroots.h"
#include "ltable.h"
#include "ltableaccess.h"
#include "ltm.h"

static LuaObjectLock *objectlock (GCObject *object) {
  lua_assert(object->tt == LUA_VTABLE || object->tt == LUA_VLCL ||
             object->tt == LUA_VCCL || object->tt == LUA_VUSERDATA);
  switch (object->tt) {
    case LUA_VTABLE: return &gco2t(object)->access;
    case LUA_VLCL: return &gco2lcl(object)->access;
    case LUA_VCCL: return &gco2ccl(object)->access;
    default: return &gco2u(object)->access;
  }
}

enum { VERSION_ENTER_LOCAL, VERSION_ENTER_SHARED, VERSION_LEAVE };
enum { ACCESS_OWNED = 1, ACCESS_BORROWED };

l_sinline void version (GCObject *object, LuaObjectLock *lock, int transition) {
  if (object != NULL && lock == objectlock(object)) {
    uint64_t *sequence = luaA_sequence(object);
    if (sequence != NULL) {
      if (transition == VERSION_ENTER_SHARED && object->tt == LUA_VTABLE) {
        /* Preserve a first writer's advertisement if it published before
        ** our layout admission closed. The drain uses the resulting flag. */
        __atomic_fetch_or(sequence, 1, __ATOMIC_RELEASE);
      }
      else {
        unsigned delta = transition == VERSION_LEAVE && object->tt == LUA_VTABLE
                         ? LUA_TABLE_VERSION_STEP - 1 : 1;
        __atomic_store_n(sequence, __atomic_load_n(sequence, __ATOMIC_RELAXED) +
                                     delta, __ATOMIC_RELEASE);
      }
    }
  }
}

static int tryaccess (lua_State *, LuaObjectLock *, GCObject *);

static int mutatoraccess (lua_State *L, LuaObjectLock *lock, GCObject *object) {
  if (object != NULL && lock == objectlock(object) && luaA_local(L, object)) {
    uintptr_t token = cast(uintptr_t, luaX_token(L));
    uintptr_t word = __atomic_load_n(&lock->word, __ATOMIC_RELAXED);
    if ((word & ~LUA_OBJECT_WAITING) == token) return ACCESS_BORROWED;
    lua_assert((word & ~LUA_OBJECT_WAITING) == 0 && !(token & LUA_OBJECT_WAITING));
    __atomic_store_n(&lock->word, token | (word & LUA_OBJECT_WAITING),
                     __ATOMIC_RELEASE);
    version(object, lock, VERSION_ENTER_LOCAL);
    return ACCESS_OWNED;
  }
  return tryaccess(L, lock, object);
}

static int tryaccess (lua_State *L, LuaObjectLock *lock, GCObject *object) {
  uintptr_t token = cast(uintptr_t, luaX_token(L));
  uintptr_t word = __atomic_load_n(&lock->word, __ATOMIC_SEQ_CST);
  uintptr_t owner = word & ~LUA_OBJECT_WAITING;
  lua_assert(!(token & LUA_OBJECT_WAITING));
  if (owner == token) return ACCESS_BORROWED;
  if (owner == 0 && __atomic_compare_exchange_n(&lock->word, &word,
          token | (word & LUA_OBJECT_WAITING), 0, __ATOMIC_SEQ_CST,
          __ATOMIC_RELAXED)) {
    version(object, lock, VERSION_ENTER_SHARED);
    return ACCESS_OWNED;
  }
  return 0;
}

int luaA_tryscan (lua_State *L, GCObject *object) {
  int reservation = tryaccess(L, objectlock(object), object);
  if (reservation == ACCESS_OWNED && object->tt == LUA_VTABLE &&
      !luaH_quiescewriters(L, gco2t(object), 0)) {
    luaA_endscan(L, object, reservation);
    return 0;
  }
  return reservation;
}

static void unlockwatched (lua_State *L, LuaObjectLock *lock) {
  /* Either the release observes the waiter bit, or the observer's next
  ** predicate check sees the released ownership. */
  if (__atomic_exchange_n(&lock->word, 0, __ATOMIC_SEQ_CST) & LUA_OBJECT_WAITING) {
    LuaWorld *world = &G(L)->world;
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    luaQ_wake(&world->changed, &world->waiters);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
}

void luaA_endscan (lua_State *L, GCObject *object, int reservation) {
  LuaObjectLock *lock = objectlock(object);
  lua_assert(reservation != 0 && luaA_lockowner(lock) == luaX_token(L));
  if (reservation == ACCESS_OWNED) {
    version(object, lock, VERSION_LEAVE);
    unlockwatched(L, lock);
  }
}

static void beginaccess (lua_State *L, LuaObjectLock *lock, GCObject *object,
                         LuaObjectScope *scope) {
  LuaWorld *world = &G(L)->world;
  scope->lock = lock;
  scope->held = 0;
  scope->saved_top = -1;
  luaR_pushroot(L, &scope->root);
  if (object != NULL) { setgcovalue(L, &scope->root.value, object); }
  if (object != NULL) luaC_tracepointer(L, object);
  scope->previous = L->object_scopes;
  L->object_scopes = scope;
  /* A collector reads/mutates objects under quiescence, including ones whose
  ** owner is pinned at an allocation boundary. It must not wait on that owner. */
  if (!luaW_iscollector(L)) {
    if (isLua(L->ci) && L->top.p < L->ci->top.p) {
      scope->saved_top = savestack(L, L->top.p);
      L->top.p = L->ci->top.p;  /* publish all registers before a possible wait */
    }
    scope->held = mutatoraccess(L, lock, object);
    if (!scope->held) {
      luaX_checksync(pthread_mutex_lock(&world->mutex));
      luaA_watchlock(lock);
      if (luaW_stopped(world) || !(scope->held = tryaccess(L, lock, object))) {
        /* Never acquire an unrelated object behind metadata ownership. */
        lua_assert(world->heap_depth == 0 ||
                   !luaX_isowner(L, &world->heap_owner));
        lua_assert(luaW_active(L->admission) != 0);
        L->native_depth++;
        L->object_waiting++;
        luaW_deactivate(L);
        luaQ_wake(&world->changed, &world->waiters);
        do {
          luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
          luaA_watchlock(lock);
        } while (luaW_stopped(world) ||
                 !(scope->held = tryaccess(L, lock, object)));
        luaW_resume(L);
        luaC_checksnapshot(L);
        L->object_waiting--;
        L->native_depth--;
      }
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
    }
    if (object != NULL && object->tt == LUA_VTABLE && scope->held == ACCESS_OWNED)
      luaH_quiescewriters(L, gco2t(object), 1);
    if (object != NULL && object->tt == LUA_VTABLE && luaC_tracing(L))
      luaC_prepareweak(L, gco2t(object));
  }
}

void luaA_begin (lua_State *L, GCObject *object, LuaObjectScope *scope) {
  beginaccess(L, objectlock(object), object, scope);
}

void luaA_beginpayload (lua_State *L, Udata *object, LuaObjectScope *scope) {
  beginaccess(L, &object->payload_access, obj2gco(object), scope);
}

void luaA_beginlock (lua_State *L, LuaObjectLock *lock, LuaObjectScope *scope) {
  beginaccess(L, lock, NULL, scope);
}

int luaA_trylock (lua_State *L, LuaObjectLock *lock) {
  return tryaccess(L, lock, NULL);
}

void luaA_endlock (lua_State *L, LuaObjectLock *lock, int reservation) {
  lua_assert(reservation != 0 && luaA_lockowner(lock) == luaX_token(L));
  if (reservation == ACCESS_OWNED) unlockwatched(L, lock);
}

static void releasescope (lua_State *L, LuaObjectScope *scope) {
  int reservation = scope->held;
  scope->held = 0;
  lua_assert(!reservation || luaA_lockowner(scope->lock) == luaX_token(L));
  if (reservation == ACCESS_OWNED) {
    GCObject *object = iscollectable(&scope->root.value)
      ? gcvalue(&scope->root.value) : NULL;
    version(object, scope->lock, VERSION_LEAVE);
    if (object != NULL && scope->lock == objectlock(object) && luaA_local(L, object))
      __atomic_store_n(&scope->lock->word, 0, __ATOMIC_RELEASE);
    else
      unlockwatched(L, scope->lock);
  }
}

void luaA_endrooted (lua_State *L, LuaObjectScope *scope) {
  lua_assert(L->object_scopes == scope);
  releasescope(L, scope);
  if (scope->saved_top >= 0)
    L->top.p = restorestack(L, scope->saved_top);
  L->object_scopes = scope->previous;
}

void luaA_end (lua_State *L, LuaObjectScope *scope) {
  luaA_endrooted(L, scope);
  luaR_poproot(L, &scope->root);
}

/* Called before longjmp, while abandoned scopes' native storage still lives.
** The protected boundary restores its saved root head immediately after jump. */
void luaA_unwind (lua_State *L, LuaObjectScope *target) {
  while (L->object_scopes != target) {
    LuaObjectScope *scope = L->object_scopes;
    lua_assert(scope != NULL);
    releasescope(L, scope);
    L->object_scopes = scope->previous;
  }
}

/* Error handlers may execute arbitrary Lua before the final throw. Retain the
** roots, but release object ownership; the failed operation never resumes. */
void luaA_release (lua_State *L) {
  LuaObjectScope *scope;
  for (scope = L->object_scopes; scope != NULL; scope = scope->previous)
    releasescope(L, scope);
}

static Table *objectmeta (const TValue *object) {
  return ttistable(object) ? hvalue(object)->metatable : uvalue(object)->metatable;
}

void luaA_getmetatable (lua_State *L, const TValue *object, LuaNativeRoot *result) {
  Table *mt;
  if (ttistable(object) || ttisfulluserdata(object)) {
    LuaObjectScope scope;
    luaA_begin(L, gcvalue(object), &scope);
    mt = objectmeta(object);
    if (mt) { sethvalue(L, &result->value, mt); }
    else setnilvalue(&result->value);
    luaA_end(L, &scope);
  }
  else {
    mt = __atomic_load_n(&G(L)->mt[ttype(object)], __ATOMIC_ACQUIRE);
    if (mt) {
      sethvalue(L, &result->value, mt);
      luaC_tracepointer(L, mt);
    }
    else setnilvalue(&result->value);
  }
}

/* The layout scope excludes all writers. A detached metatable may still
** carry the flag. Invalidating a newer binding is harmless if it changes
** after the identity check. */
void luaA_invalidatestringindex (lua_State *L, Table *mt) {
  global_State *g = G(L);
  if (__atomic_load_n(&g->mt[LUA_TSTRING], __ATOMIC_ACQUIRE) == mt)
    __atomic_store_n(&g->string_index, NULL, __ATOMIC_RELEASE);
  __atomic_fetch_and(&mt->flags, cast_byte(~LUA_TABLE_STRING_INDEX),
                     __ATOMIC_RELEASE);
}

void luaA_setmetatable (lua_State *L, const TValue *object, Table *mt) {
  LuaNativeRoot rooted_object, rooted_mt;
  luaR_pushroot(L, &rooted_object);
  luaR_setroot(L, &rooted_object, object);
  object = &rooted_object.value;
  luaR_pushroot(L, &rooted_mt);
  if (mt) sethvalue(L, &rooted_mt.value, mt);
  if (ttistable(object) || ttisfulluserdata(object)) {
    LuaObjectScope first, second;
    GCObject *owner = gcvalue(object);
    GCObject *other = mt ? obj2gco(mt) : owner;
    int pair = owner != other;
    int ownerfirst = cast(uintptr_t, owner) < cast(uintptr_t, other);
    luaA_begin(L, ownerfirst ? owner : other, &first);
    if (pair) luaA_begin(L, ownerfirst ? other : owner, &second);
    if (mt != NULL) luaA_publish(L, owner, &rooted_mt.value);
    luaC_prewriteptr(L, objectmeta(object), mt);
    if (ttistable(object)) {
      Table *table = hvalue(object);
      if (gettableflags(table) & LUA_TABLE_STRING_INDEX)
        luaA_invalidatestringindex(L, table);
      __atomic_store_n(&table->metatable, mt, __ATOMIC_RELEASE);
    }
    else __atomic_store_n(&uvalue(object)->metatable, mt, __ATOMIC_RELEASE);
    if (mt) {
      luaC_objbarrier(L, gcvalue(object), mt);
      luaC_checkfinalizer(L, gcvalue(object), mt);
    }
    if (pair) luaA_end(L, &second);
    luaA_end(L, &first);
  }
  else {
    int type = ttype(object);
    global_State *g = G(L);
    uint64_t sequence;
    Table *index = NULL;
    LuaObjectScope access, layout;
    int cache = type == LUA_TSTRING && mt != NULL && luaA_localtable(L, mt) &&
      mt->metatable == NULL &&
      !(__atomic_load_n(&mt->sequence, __ATOMIC_RELAXED) & 1);
    /* Start with a private, strong metatable. Keep its layout owned until
    ** both the binding and the derived index are visible to other states. */
    if (cache) luaA_begin(L, obj2gco(mt), &layout);
    luaA_beginlock(L, &g->metatable_access[type], &access);
    if (cache) {
      const TValue *field = luaH_Hgetshortstr(mt, g->tmname[TM_INDEX]);
      if (ttistable(field)) {
        index = hvalue(field);
        __atomic_fetch_or(&mt->flags, LUA_TABLE_STRING_INDEX, __ATOMIC_RELEASE);
      }
    }
    if (mt != NULL) luaA_share(L, obj2gco(mt));
    luaC_prewriteptr(L, __atomic_load_n(&g->mt[type], __ATOMIC_RELAXED), mt);
    sequence = __atomic_load_n(&g->metatable_sequence[type], __ATOMIC_RELAXED);
    __atomic_store_n(&g->metatable_sequence[type], sequence + 1, __ATOMIC_RELEASE);
    if (type == LUA_TSTRING)
      __atomic_store_n(&g->string_index, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&g->mt[type], mt, __ATOMIC_RELEASE);
    if (index != NULL)
      __atomic_store_n(&g->string_index, index, __ATOMIC_RELEASE);
    __atomic_store_n(&g->metatable_sequence[type], sequence + 2, __ATOMIC_RELEASE);
    luaA_end(L, &access);
    if (cache) luaA_end(L, &layout);
  }
  luaR_poproot(L, &rooted_mt);
  luaR_poproot(L, &rooted_object);
}

/* Discover under owner access and retain it when address order permits.
** Recheck after acquiring both objects; GC can run during an ownership wait. */
static int objectandmeta (lua_State *L, const TValue *object,
                          LuaObjectScope *first, LuaObjectScope *second,
                          LuaNativeRoot *metatable) {
  GCObject *owner = gcvalue(object);
  for (;;) {
    Table *mt;
    luaA_begin(L, owner, first);
    mt = objectmeta(object);
    if (mt) { sethvalue(L, &metatable->value, mt); }
    else setnilvalue(&metatable->value);
    if (mt == NULL || obj2gco(mt) == owner) {
      return 1;
    }
    else {
      GCObject *other = obj2gco(mt);
      int ownerfirst = cast(uintptr_t, owner) < cast(uintptr_t, other);
      if (ownerfirst)
        luaA_begin(L, other, second);
      else {
        luaA_end(L, first);
        luaA_begin(L, other, first);
        luaA_begin(L, owner, second);
      }
      if (objectmeta(object) == mt)
        return 2;
      luaA_end(L, second);
      luaA_end(L, first);
    }
  }
}

static void endpair (lua_State *L, LuaObjectScope *first,
                      LuaObjectScope *second, int count) {
  if (count == 2) luaA_end(L, second);
  luaA_end(L, first);
}

/* The returned value is copied into the caller's live, GC-visible snapshot. */
static void metafield (lua_State *L, Table *mt, TString *key,
                       LuaNativeRoot *result, int event) {
  const TValue *slot;
  setnilvalue(&result->value);
  if (mt == NULL)
    return;
  slot = luaH_Hgetshortstr(mt, key);
  if (ttisnil(slot)) {
    if (event >= 0 && event <= TM_EQ)
      __atomic_fetch_or(&mt->flags, cast_byte(1u << event), __ATOMIC_RELAXED);
  }
  else
    luaR_setroot(L, result, slot);
}

void luaA_tabletm (lua_State *L, Table *mt, TMS event, LuaNativeRoot *result) {
  LuaObjectScope scope;
  luaA_begin(L, obj2gco(mt), &scope);
  metafield(L, mt, G(L)->tmname[event], result, event);
  luaA_end(L, &scope);
}

/* Managed admission retains the metatable and its buffers through this
** nonparking lookup. Validate the type slot as well as the table snapshot:
** a detached metatable can be modified before it is installed again. */
int luaA_readtypefield (lua_State *L, int type, TString *key, TValue *result) {
  global_State *g = G(L);
  uint64_t sequence;
  TValue value;
  Table *mt;
  if (type == LUA_TSTRING && key == g->tmname[TM_INDEX]) {
    Table *index = __atomic_load_n(&g->string_index, __ATOMIC_ACQUIRE);
    if (index != NULL) {
      sethvalue(L, result, index);
      return 1;
    }
  }
  sequence = __atomic_load_n(&g->metatable_sequence[type], __ATOMIC_ACQUIRE);
  if (sequence & 1) return 0;
  mt = __atomic_load_n(&g->mt[type], __ATOMIC_ACQUIRE);
  if (mt == NULL) setnilvalue(&value);
  else if (!luaH_readshortstr(g, L, mt, key, &value)) return 0;
  if (__atomic_load_n(&g->metatable_sequence[type], __ATOMIC_ACQUIRE) != sequence)
    return 0;
  setobj(L, result, &value);
  return 1;
}

void luaA_metafield (lua_State *L, const TValue *object, TString *key,
                     LuaNativeRoot *result, int event) {
  LuaNativeRoot owner, metatable, rooted_key;
  LuaObjectScope first, second;
  int perobject = ttistable(object) || ttisfulluserdata(object);
  if (!perobject) {
    TValue value;
    if (luaA_readtypefield(L, ttype(object), key, &value)) {
      luaR_setroot(L, result, &value);
      return;
    }
  }
  luaR_pushroot(L, &owner);
  luaR_setroot(L, &owner, object);
  luaR_pushroot(L, &metatable);
  luaR_pushroot(L, &rooted_key);
  setsvalue(L, &rooted_key.value, key);
  if (perobject) {
    int count = objectandmeta(L, &owner.value, &first, &second, &metatable);
    Table *mt = ttistable(&metatable.value) ? hvalue(&metatable.value) : NULL;
    metafield(L, mt, key, result, event);
    endpair(L, &first, &second, count);
  }
  else {
    int type = ttype(&owner.value);
    for (;;) {
      Table *mt;
      luaA_getmetatable(L, &owner.value, &metatable);
      mt = ttistable(&metatable.value) ? hvalue(&metatable.value) : NULL;
      if (mt) luaA_begin(L, obj2gco(mt), &first);
      if (__atomic_load_n(&G(L)->mt[type], __ATOMIC_ACQUIRE) == mt) {
        metafield(L, mt, key, result, event);
        if (mt) luaA_end(L, &first);
        break;
      }
      if (mt) luaA_end(L, &first);
    }
  }
  luaR_poproot(L, &rooted_key);
  luaR_poproot(L, &metatable);
  luaR_poproot(L, &owner);
}

lu_byte luaA_index (lua_State *L, Table *table, const TValue *key,
                    TValue *result, LuaNativeRoot *method) {
  LuaNativeRoot owner, metatable;
  LuaObjectScope first, second;
  lu_byte tag;
  int count;
  /* A hit or a miss with no metatable completes indexing from one view. */
  if (luaH_tryget(L, table, key, result, &tag) &&
      tag != LUA_VABSTKEY) {
    setnilvalue(&method->value);
    return tag;
  }
  luaR_pushroot(L, &owner);
  sethvalue(L, &owner.value, table);
  luaR_pushroot(L, &metatable);
  count = objectandmeta(L, &owner.value, &first, &second, &metatable);
  tag = luaH_getowned(table, key, result);
  if (!tagisempty(tag)) luaC_tracevalue(L, result);
  setnilvalue(&method->value);
  if (tagisempty(tag)) {
    Table *mt = ttistable(&metatable.value) ? hvalue(&metatable.value) : NULL;
    metafield(L, mt, G(L)->tmname[TM_INDEX], method, TM_INDEX);
    if (ttisnil(&method->value)) {
      setnilvalue(result);
      tag = LUA_VNIL;
    }
  }
  endpair(L, &first, &second, count);
  luaR_poproot(L, &metatable);
  luaR_poproot(L, &owner);
  return tag;
}

int luaA_assign (lua_State *L, const TValue *object, const TValue *key,
                 TValue *value, LuaNativeRoot *method) {
  Table *table = hvalue(object);
  LuaNativeRoot metatable;
  LuaObjectScope first, second;
  int handled, count;
  setnilvalue(&method->value);
  if (__atomic_load_n(&table->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED) {
    luaA_sharevalue(L, key);
    luaA_sharevalue(L, value);
    if (luaH_setshared(L, table, key, value, 0)) return 1;
  }
  /* Only a missing key with a metatable needs paired ownership. Recheck
  ** after the ordered pair is acquired: either object may have changed. */
  luaR_pushroot(L, &metatable);
  count = objectandmeta(L, object, &first, &second, &metatable);
  handled = luaH_assignowned(L, table, key, value, 0);
  if (!handled) {
    Table *mt = hvalue(&metatable.value);
    metafield(L, mt, G(L)->tmname[TM_NEWINDEX], method, TM_NEWINDEX);
    handled = ttisnil(&method->value);
    if (handled) luaH_assignowned(L, table, key, value, 1);
  }
  endpair(L, &first, &second, count);
  luaR_poproot(L, &metatable);
  return handled;
}
