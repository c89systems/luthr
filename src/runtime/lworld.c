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
#include "lgc.h"
#include "lstate.h"
#include "lworld.h"

typedef struct LuaHeapWaiter {
  struct LuaHeapWaiter *next;
  void *owner;
  LuaWaiter *waiters;
  pthread_cond_t changed;
} LuaHeapWaiter;

int luaW_init (LuaWorld *world) {
  int status;
  world->ready = world->stopped = 0;
  world->heap_depth = 0;
  world->heap_owner.token = world->collector.token = NULL;
  world->participants = NULL;
  world->reclaimers = NULL;
  world->reclaim_epoch = 0;
  world->waiters = NULL;
  world->heap_head = NULL;
  world->heap_tail = &world->heap_head;
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
  lua_assert(world->ready && !world->stopped && world->participants == NULL &&
             world->heap_depth == 0 && world->waiters == NULL &&
             world->heap_head == NULL && world->reclaimers == NULL);
  luaX_checksync(pthread_cond_destroy(&world->changed));
  luaX_checksync(pthread_mutex_destroy(&world->mutex));
  world->ready = 0;
}

void luaW_register (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaParticipation *participant = &L->participation;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(participant->previous == NULL && participant->active == 0);
  participant->next = world->participants;
  participant->previous = &world->participants;
  if (participant->next != NULL)
    participant->next->previous = &participant->next;
  world->participants = participant;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_unregister (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaParticipation *participant = &L->participation;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(__atomic_load_n(&participant->active, __ATOMIC_SEQ_CST) == 0);
  if (participant->previous != NULL) {
    *participant->previous = participant->next;
    if (participant->next != NULL)
      participant->next->previous = participant->previous;
    participant->previous = NULL;
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

unsigned luaW_activecount (LuaWorld *world) {
  LuaParticipation *participant;
  unsigned active = 0;
  for (participant = world->participants; participant != NULL;
       participant = participant->next)
    active += __atomic_load_n(&participant->active, __ATOMIC_SEQ_CST);
  return active;
}

void luaW_activate (lua_State *L) {
  __atomic_fetch_add(&L->admission->active, 1, __ATOMIC_SEQ_CST);
}

void luaW_deactivate (lua_State *L) {
  unsigned previous = __atomic_fetch_sub(&L->admission->active, 1,
                                        __ATOMIC_SEQ_CST);
  lua_assert(previous != 0);
  UNUSED(previous);
}

/* Stack ownership may already be reserved, but no managed data is accessed
** until this entry completes. A waiting gate does not count as a mutator. */
void luaW_enter (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  /* Store/load ordering pairs with the collector's request/scan ordering.
  ** Either the scan observes this entry, or it observes the request before
  ** accessing managed data. No shared ownership word is changed here. */
  luaW_activate(L);
  if (!__atomic_load_n(&world->stopped, __ATOMIC_SEQ_CST))
    return;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  /* Only the atomic participation record can change before admission. In
  ** particular, a late entrant cannot change roots while a collector scans
  ** this state. Its embedding caller retains the target's ordinary Lua root. */
  luaW_deactivate(L);
  luaQ_wake(&world->changed, &world->waiters);
  while (world->stopped)
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  luaW_activate(L);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_leave (LuaWorld *world, LuaParticipation *participant) {
  unsigned previous = __atomic_fetch_sub(&participant->active, 1,
                                        __ATOMIC_SEQ_CST);
  lua_assert(previous != 0);
  UNUSED(previous);
  /* No participant accesses after withdrawing: it may already be reclaimed. */
  if (__atomic_load_n(&world->stopped, __ATOMIC_SEQ_CST)) {
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    luaQ_wake(&world->changed, &world->waiters);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
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
  luaQ_setowner(&world->collector);
  __atomic_store_n(&world->stopped, 1, __ATOMIC_SEQ_CST);
  luaQ_wake(&world->changed, &world->waiters);
  luaW_deactivate(L);
  while (luaW_activecount(world) != 0)
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  L->world_depth = 1;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaC_publish(L);
  luaC_drainbarriers(L);
}

void luaW_start (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  lua_assert(L->world_depth != 0);
  if (--L->world_depth != 0)
    return;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(world->stopped && luaQ_isowner(&world->collector));
  /* Late entrants may have published a tentative reference after the stop
  ** scan. They cannot access managed data until stopped becomes false. */
  luaW_activate(L);  /* the collector resumes as an ordinary mutator */
  __atomic_store_n(&world->stopped, 0, __ATOMIC_SEQ_CST);
  luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaW_heapleave(L);
}

int luaW_iscollector (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  if (L->world_depth != 0) {
    lua_assert(__atomic_load_n(&world->stopped, __ATOMIC_ACQUIRE) &&
               luaQ_isowner(&world->collector));
    return 1;
  }
  return __atomic_load_n(&world->stopped, __ATOMIC_ACQUIRE) &&
         luaQ_isowner(&world->collector);
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
      LuaHeapWaiter wait = {NULL, NULL, NULL, PTHREAD_COND_INITIALIZER};
      lua_assert(L->world_depth == 0 &&
        __atomic_load_n(&L->admission->active, __ATOMIC_SEQ_CST) != 0);
      L->native_depth++;
      L->heap_waiting++;
      wait.owner = luaQ_ownertoken();
      *world->heap_tail = &wait;
      world->heap_tail = &wait.next;
      luaW_deactivate(L);
      luaQ_wake(&world->changed, &world->waiters);
      while (!luaQ_isowner(&world->heap_owner))
        luaQ_wait(L, &world->mutex, &wait.changed, &wait.waiters);
      /* Ownership is reserved directly for this recipient. Fresh arrivals
      ** cannot steal it while its saved stack waits for execution capacity. */
      lua_assert(world->heap_depth == 1 && !world->stopped);
      luaX_checksync(pthread_cond_destroy(&wait.changed));
      luaW_activate(L);
      L->heap_waiting--;
      L->native_depth--;
    }
    else {
      world->heap_depth = 1;
      luaQ_setowner(&world->heap_owner);
    }
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_heapleave (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(world->heap_depth != 0 &&
             luaQ_isowner(&world->heap_owner));
  if (--world->heap_depth == 0) {
    LuaHeapWaiter *next = world->heap_head;
    if (next == NULL)
      __atomic_store_n(&world->heap_owner.token, NULL, __ATOMIC_RELEASE);
    else {
      world->heap_head = next->next;
      if (world->heap_head == NULL) world->heap_tail = &world->heap_head;
      world->heap_depth = 1;
      __atomic_store_n(&world->heap_owner.token, next->owner, __ATOMIC_RELEASE);
      if (next->waiters != NULL && next->waiters->execution != NULL)
        luaQ_requesthandoff(L);
      luaQ_wake(&next->changed, &next->waiters);
    }
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

static unsigned heapdepth (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  return luaQ_isowner(&world->heap_owner) ? world->heap_depth : 0;
}

void luaW_waitreclaim (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  lua_assert(L->native_depth != 0);
  luaW_deactivate(L);
  luaQ_wake(&world->changed, &world->waiters);
  do {
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  } while (world->stopped);
  luaW_activate(L);
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
