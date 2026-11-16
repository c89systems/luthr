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
  if (object->tt == LUA_VTABLE)
    return &gco2t(object)->access;
  if (object->tt == LUA_VLCL)
    return &gco2lcl(object)->access;
  if (object->tt == LUA_VCCL)
    return &gco2ccl(object)->access;
  lua_assert(object->tt == LUA_VUSERDATA);
  return &gco2u(object)->access;
}

void luaA_begin (lua_State *L, GCObject *object, LuaObjectScope *scope) {
  LuaWorld *world = &G(L)->world;
  LuaObjectLock *lock = objectlock(object);
  scope->lock = lock;
  scope->held = 0;
  scope->saved_top = -1;
  luaR_pushroot(L, &scope->root);
  setgcovalue(L, &scope->root.value, object);
  scope->previous = L->object_scopes;
  L->object_scopes = scope;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  /* A collector reads/mutates objects under quiescence, including ones whose
  ** owner is pinned at an allocation boundary. It must not wait on that owner. */
  if (!(world->stopped && luaQ_isowner(&world->collector))) {
    if (isLua(L->ci) && L->top.p < L->ci->top.p) {
      scope->saved_top = savestack(L, L->top.p);
      L->top.p = L->ci->top.p;  /* publish all registers before a possible wait */
    }
    if (lock->depth != 0 && luaQ_isowner(&lock->owner))
      lock->depth++;
    else {
      if (lock->depth != 0 || world->stopped) {
        /* Never acquire an unrelated object behind metadata ownership. */
        lua_assert(world->heap_depth == 0 ||
                   !luaQ_isowner(&world->heap_owner));
        lua_assert(world->active != 0);
        L->native_depth++;
        L->object_waiting++;
        world->active--;
        luaQ_wake(&world->changed, &world->waiters);
        do {
          luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
        } while (lock->depth != 0 || world->stopped);
        world->active++;
        L->object_waiting--;
        L->native_depth--;
      }
      luaQ_setowner(&lock->owner);
      lock->depth = 1;
    }
    scope->held = 1;
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

static void releasescope (lua_State *L, LuaObjectScope *scope) {
  if (scope->held) {
    LuaWorld *world = &G(L)->world;
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    lua_assert(scope->lock->depth != 0 &&
               luaQ_isowner(&scope->lock->owner));
    scope->held = 0;
    if (--scope->lock->depth == 0)
      luaQ_wake(&world->changed, &world->waiters);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
}

void luaA_end (lua_State *L, LuaObjectScope *scope) {
  lua_assert(L->object_scopes == scope);
  releasescope(L, scope);
  if (scope->saved_top >= 0)
    L->top.p = restorestack(L, scope->saved_top);
  L->object_scopes = scope->previous;
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
    luaW_heapenter(L);
    mt = G(L)->mt[type];
    if (mt) { sethvalue(L, &result->value, mt); }
    else setnilvalue(&result->value);
    luaW_heapleave(L);
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
    luaW_heapenter(L);  /* userdata metatables are visited by mutator barriers */
    if (ttistable(object)) hvalue(object)->metatable = mt;
    else uvalue(object)->metatable = mt;
    if (mt) {
      luaC_objbarrier(L, gcvalue(object), mt);
      luaC_checkfinalizer(L, gcvalue(object), mt);
    }
    luaW_heapleave(L);
    if (pair) luaA_end(L, &second);
    luaA_end(L, &first);
  }
  else {
    int type = ttype(object);
    luaW_heapenter(L);
    G(L)->mt[type] = mt;
    luaW_heapleave(L);
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
      luaA_getmetatable(L, &owner.value, &metatable);
      mt = ttistable(&metatable.value) ? hvalue(&metatable.value) : NULL;
      if (mt) luaA_begin(L, obj2gco(mt), &first);
      luaW_heapenter(L);
      if (G(L)->mt[type] == mt) {
        metafield(L, mt, key, result, event);
        luaW_heapleave(L);
        if (mt) luaA_end(L, &first);
        break;
      }
      luaW_heapleave(L);
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

int luaA_assign (lua_State *L, Table *table, const TValue *key,
                 TValue *value, LuaNativeRoot *method) {
  LuaNativeRoot owner, metatable;
  LuaObjectScope first, second;
  TValue old;
  int handled, count;
  luaR_pushroot(L, &owner);
  sethvalue(L, &owner.value, table);
  luaR_pushroot(L, &metatable);
  count = objectandmeta(L, &owner.value, &first, &second, &metatable);
  setnilvalue(&method->value);
  handled = !tagisempty(luaH_getowned(table, key, &old));
  if (!handled) {
    Table *mt = ttistable(&metatable.value) ? hvalue(&metatable.value) : NULL;
    metafield(L, mt, G(L)->tmname[TM_NEWINDEX], method, TM_NEWINDEX);
    handled = ttisnil(&method->value);
  }
  if (handled)
    luaH_setowned(L, table, key, value);
  endpair(L, &first, &second, count);
  luaR_poproot(L, &metatable);
  luaR_poproot(L, &owner);
  return handled;
}
