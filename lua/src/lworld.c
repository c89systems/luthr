/*
** Managed participation and collector rendezvous.
** See Copyright Notice in lua.h
*/
#define lworld_c
#define LUA_CORE
#include "lprefix.h"

#include "lapi.h"
#include "laccess.h"
#include "lgate.h"
#include "lstate.h"
#include "lworld.h"

int luaW_init (LuaWorld *world) {
  int status;
  world->ready = world->stopped = 0;
  world->active = world->heap_depth = 0;
  world->waiters = NULL;
  status = pthread_mutex_init(&world->mutex, NULL);
  if (status != 0)
    return status;
  status = pthread_cond_init(&world->changed, NULL);
  if (status != 0) {
    luaX_checksync(pthread_mutex_destroy(&world->mutex));
    return status;
  }
  world->ready = 1;
  return 0;
}

void luaW_destroy (LuaWorld *world) {
  lua_assert(world->ready && !world->stopped && world->active == 0 &&
             world->heap_depth == 0 && world->waiters == NULL);
  luaX_checksync(pthread_cond_destroy(&world->changed));
  luaX_checksync(pthread_mutex_destroy(&world->mutex));
  world->ready = 0;
}

/* Stack ownership may already be reserved, but no managed data is accessed
** until this entry completes. A waiting gate does not count as a mutator. */
void luaW_enter (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  while (world->stopped)
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  world->active++;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_leave (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(world->active != 0);
  world->active--;
  if (world->active == 0)
    luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

/* The requesting mutator retains its stack gate. Other mutators publish their
** roots before leaving managed execution, or finish their current API entry.
** Contending collectors park as pinned, inactive mutators before retrying. */
void luaW_stop (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  if (L->world_depth != 0) {
    L->world_depth++;
    return;
  }
  luaW_heapenter(L);  /* collect only after pending metadata scopes finish */
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(!world->stopped);  /* metadata ownership orders collection requests */
  world->stopped = 1;
  luaQ_setowner(&world->collector);
  luaQ_wake(&world->changed, &world->waiters);
  lua_assert(world->active != 0);
  world->active--;
  while (world->active != 0)
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  L->world_depth = 1;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_start (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  lua_assert(L->world_depth != 0);
  if (--L->world_depth != 0)
    return;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(world->stopped && world->active == 0 &&
             luaQ_isowner(&world->collector));
  world->active = 1;  /* the collector resumes as an ordinary mutator */
  world->stopped = 0;
  luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaW_heapleave(L);
}

/* Reentrant within one execution (or unmanaged carrier), including operations
** on another coroutine's stack. A metadata waiter is pinned but not a mutator.
** The outermost acquisition must be a GC-safe boundary, before mutation. */
void luaW_heapenter (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (world->heap_depth != 0 &&
      luaQ_isowner(&world->heap_owner)) {
    world->heap_depth++;
  }
  else {
    if (world->heap_depth != 0 || world->stopped) {
      lua_assert(L->world_depth == 0 && world->active != 0);
      L->native_depth++;
      L->heap_waiting++;
      world->active--;
      luaQ_wake(&world->changed, &world->waiters);
      do {
        luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
      } while (world->heap_depth != 0 || world->stopped);
      world->active++;
      L->heap_waiting--;
      L->native_depth--;
    }
    luaQ_setowner(&world->heap_owner);
    world->heap_depth = 1;
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_heapleave (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(world->heap_depth != 0 &&
             luaQ_isowner(&world->heap_owner));
  if (--world->heap_depth == 0)
    luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

static unsigned heapdepth (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  unsigned depth;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  depth = (world->heap_depth != 0 &&
           luaQ_isowner(&world->heap_owner))
          ? world->heap_depth : 0;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  return depth;
}

static void restoreheap (lua_State *L, unsigned depth) {
  unsigned current = heapdepth(L);
  while (current > depth) {
    luaW_heapleave(L);
    current--;
  }
  while (current < depth) {
    luaW_heapenter(L);
    current++;
  }
}

LuaWorldScope luaW_scope (lua_State *L) {
  LuaWorldScope scope;
  scope.collection = L->world_depth;
  scope.heap = heapdepth(L);
  if (scope.collection != 0) {
    lua_assert(scope.heap != 0);
    scope.heap--;  /* exclude the collector's implicit metadata ownership */
  }
  return scope;
}

LuaWorldScope luaW_suspend (lua_State *L) {
  LuaWorldScope scope = luaW_scope(L);
  if (scope.collection != 0) {
    L->world_depth = 1;
    luaW_start(L);
  }
  restoreheap(L, 0);
  return scope;
}

void luaW_restore (lua_State *L, LuaWorldScope scope, unsigned native) {
  if (L->world_depth != scope.collection) {
    if (L->world_depth != 0) {
      L->world_depth = 1;
      luaW_start(L);
    }
    restoreheap(L, scope.heap);
    if (scope.collection != 0) {
      luaW_stop(L);
      L->world_depth = scope.collection;
    }
  }
  else
    restoreheap(L, scope.heap + (scope.collection != 0));
  L->native_depth = native;
}

/* These boundaries retain the coroutine while native code blocks or reenters.
** Protected unwinding restores the enclosing depth if a callback never returns. */
LuaWorldScope luaW_detach (lua_State *L) {
  LuaWorldScope scope;
#if defined(LUAI_ASSERT)
  LuaObjectScope *object;
  for (object = L->object_scopes; object != NULL; object = object->previous)
    lua_assert(!object->held);  /* callbacks never inherit object ownership */
#endif
  L->native_depth++;
  scope = luaW_suspend(L);
  lua_unlock(L);
  return scope;
}

void luaW_attach (lua_State *L, LuaWorldScope scope) {
  lua_lock(L);
  luaW_restore(L, scope, L->native_depth);
  lua_assert(L->native_depth != 0);
  L->native_depth--;
}
