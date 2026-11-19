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

l_sinline void version (GCObject *object, LuaObjectLock *lock) {
  if (object != NULL && lock == objectlock(object)) {
    uint64_t *sequence = luaA_sequence(object);
    if (sequence != NULL)
      __atomic_store_n(sequence,
        __atomic_load_n(sequence, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
  }
}

static int tryaccess (lua_State *, LuaObjectLock *, GCObject *);

static int mutatoraccess (lua_State *L, LuaObjectLock *lock, GCObject *object) {
  if (object != NULL && lock == objectlock(object) && luaA_local(L, object)) {
    void *token = luaX_token(L);
    void *owner = __atomic_load_n(&lock->owner, __ATOMIC_RELAXED);
    lua_assert(owner == NULL || owner == token);
    if (owner == token) lock->depth++;
    else {
      __atomic_store_n(&lock->owner, token, __ATOMIC_RELEASE);
      lock->depth = 1;
      version(object, lock);
    }
    return 1;
  }
  return tryaccess(L, lock, object);
}

static int tryaccess (lua_State *L, LuaObjectLock *lock, GCObject *object) {
  void *token = luaX_token(L);
  void *owner = __atomic_load_n(&lock->owner, __ATOMIC_SEQ_CST);
  if (owner == token) {
    lua_assert(lock->depth != 0);
    lock->depth++;
    return 1;
  }
  if (owner == NULL && __atomic_compare_exchange_n(&lock->owner, &owner,
          token, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    lua_assert(lock->depth == 0);
    lock->depth = 1;
    version(object, lock);
    return 1;
  }
  return 0;
}

int luaA_tryscan (lua_State *L, GCObject *object) {
  return tryaccess(L, objectlock(object), object);
}

void luaA_endscan (lua_State *L, GCObject *object) {
  LuaObjectLock *lock = objectlock(object);
  lua_assert(__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) ==
             luaX_token(L) && lock->depth != 0);
  if (--lock->depth == 0) {
    version(object, lock);
    __atomic_store_n(&lock->owner, NULL, __ATOMIC_SEQ_CST);
    /* A scanner never parks with ownership. Existing mutator waiters need
    ** the same notification handshake as an ordinary scoped release. */
    if (__atomic_load_n(&lock->waiting, __ATOMIC_SEQ_CST) != 0) {
      LuaWorld *world = &G(L)->world;
      luaX_checksync(pthread_mutex_lock(&world->mutex));
      luaQ_wake(&world->changed, &world->waiters);
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
    }
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
    if (!mutatoraccess(L, lock, object)) {
      luaX_checksync(pthread_mutex_lock(&world->mutex));
      __atomic_fetch_add(&lock->waiting, 1, __ATOMIC_SEQ_CST);
      if (world->stopped || !tryaccess(L, lock, object)) {
        /* Never acquire an unrelated object behind metadata ownership. */
        lua_assert(world->heap_depth == 0 ||
                   !luaX_isowner(L, &world->heap_owner));
        lua_assert(__atomic_load_n(&L->admission->active, __ATOMIC_SEQ_CST) != 0);
        L->native_depth++;
        L->object_waiting++;
        luaW_deactivate(L);
        luaQ_wake(&world->changed, &world->waiters);
        do {
          luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
        } while (world->stopped || !tryaccess(L, lock, object));
        luaW_resume(L);
        luaC_checksnapshot(L);
        L->object_waiting--;
        L->native_depth--;
      }
      __atomic_fetch_sub(&lock->waiting, 1, __ATOMIC_SEQ_CST);
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
    }
    scope->held = 1;
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

void luaA_endlock (lua_State *L, LuaObjectLock *lock) {
  lua_assert(__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) ==
             luaX_token(L) && lock->depth != 0);
  if (--lock->depth == 0) {
    __atomic_store_n(&lock->owner, NULL, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&lock->waiting, __ATOMIC_SEQ_CST) != 0) {
      LuaWorld *world = &G(L)->world;
      luaX_checksync(pthread_mutex_lock(&world->mutex));
      luaQ_wake(&world->changed, &world->waiters);
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
    }
  }
}

static void releasescope (lua_State *L, LuaObjectScope *scope) {
  if (scope->held) {
    LuaWorld *world = &G(L)->world;
    lua_assert(scope->lock->depth != 0 &&
      __atomic_load_n(&scope->lock->owner, __ATOMIC_RELAXED) == luaX_token(L));
    scope->held = 0;
    if (--scope->lock->depth == 0) {
      GCObject *object = iscollectable(&scope->root.value)
        ? gcvalue(&scope->root.value) : NULL;
      version(object, scope->lock);
      if (object != NULL && scope->lock == objectlock(object) &&
          luaA_local(L, object)) {
        __atomic_store_n(&scope->lock->owner, NULL, __ATOMIC_RELEASE);
        return;
      }
      /* Order release/observer checks against the waiter's observer increment
      ** and predicate recheck: either it sees unowned or this side wakes it. */
      __atomic_store_n(&scope->lock->owner, NULL, __ATOMIC_SEQ_CST);
      if (__atomic_load_n(&scope->lock->waiting, __ATOMIC_SEQ_CST) != 0) {
        luaX_checksync(pthread_mutex_lock(&world->mutex));
        luaQ_wake(&world->changed, &world->waiters);
        luaX_checksync(pthread_mutex_unlock(&world->mutex));
      }
    }
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
    int type = ttype(object);
    LuaObjectScope access;
    luaA_beginlock(L, &G(L)->metatable_access[type], &access);
    mt = G(L)->mt[type];
    if (mt) { sethvalue(L, &result->value, mt); }
    else setnilvalue(&result->value);
    luaA_end(L, &access);
  }
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
    if (ttistable(object))
      __atomic_store_n(&hvalue(object)->metatable, mt, __ATOMIC_RELEASE);
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
    LuaObjectScope access;
    luaA_beginlock(L, &G(L)->metatable_access[type], &access);
    if (mt != NULL) luaA_share(L, obj2gco(mt));
    luaC_prewriteptr(L, G(L)->mt[type], mt);
    G(L)->mt[type] = mt;
    luaA_end(L, &access);
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

void luaA_metafield (lua_State *L, const TValue *object, TString *key,
                     LuaNativeRoot *result, int event) {
  LuaNativeRoot owner, metatable, rooted_key;
  LuaObjectScope first, second;
  luaR_pushroot(L, &owner);
  luaR_setroot(L, &owner, object);
  luaR_pushroot(L, &metatable);
  luaR_pushroot(L, &rooted_key);
  setsvalue(L, &rooted_key.value, key);
  if (ttistable(&owner.value) || ttisfulluserdata(&owner.value)) {
    int count = objectandmeta(L, &owner.value, &first, &second, &metatable);
    Table *mt = ttistable(&metatable.value) ? hvalue(&metatable.value) : NULL;
    metafield(L, mt, key, result, event);
    endpair(L, &first, &second, count);
  }
  else {
    int type = ttype(&owner.value);
    for (;;) {
      Table *mt;
      LuaObjectScope access;
      luaA_getmetatable(L, &owner.value, &metatable);
      mt = ttistable(&metatable.value) ? hvalue(&metatable.value) : NULL;
      if (mt) luaA_begin(L, obj2gco(mt), &first);
      luaA_beginlock(L, &G(L)->metatable_access[type], &access);
      if (G(L)->mt[type] == mt) {
        metafield(L, mt, key, result, event);
        luaA_end(L, &access);
        if (mt) luaA_end(L, &first);
        break;
      }
      luaA_end(L, &access);
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
