/* Backing callbacks share an admission domain, not the collector's permit. */
#define lallocator_c
#define LUA_CORE
#include "lprefix.h"

#include "lallocator.h"
#include "lgc.h"
#include "lstate.h"

int luaAL_init (LuaAllocator *allocator, lua_Alloc function, void *context) {
  int status;
  allocator->function = function;
  allocator->context = context;
  allocator->owner = NULL;
  allocator->depth = allocator->waiting = 0;
  allocator->waiters = NULL;
  status = pthread_mutex_init(&allocator->mutex, NULL);
  if (status != 0) return status;
  status = pthread_cond_init(&allocator->changed, NULL);
  if (status != 0) luaX_checksync(pthread_mutex_destroy(&allocator->mutex));
  return status;
}

void luaAL_destroy (LuaAllocator *allocator) {
  lua_assert(allocator->owner == NULL && allocator->waiting == 0 &&
             allocator->waiters == NULL && allocator->depth == 0);
  luaX_checksync(pthread_cond_destroy(&allocator->changed));
  luaX_checksync(pthread_mutex_destroy(&allocator->mutex));
}

void luaAL_enter (lua_State *L) {
  LuaAllocator *allocator = &G(L)->allocator;
  LuaWorld *world = &G(L)->world;
  void *token = luaX_token(L), *owner;
  owner = __atomic_load_n(&allocator->owner, __ATOMIC_RELAXED);
  if (owner == token) {
    lua_assert(allocator->depth != 0);
    allocator->depth++;
    return;
  }
  for (;;) {
    owner = NULL;
    if (__atomic_compare_exchange_n(&allocator->owner, &owner, token, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      allocator->depth = 1;
      return;
    }
    /* Callback waiters have their own notification domain. Returning a block
    ** must not wake every worker awaiting collector metadata or quiescence. */
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    __atomic_fetch_add(&allocator->waiting, 1, __ATOMIC_SEQ_CST);
    lua_assert(L->world_depth == 0);
    L->native_depth++;
    luaW_deactivate(L);
    if (luaW_control(world) & LUA_WORLD_STORAGE_WAIT)
      luaQ_wake(&world->changed, &world->waiters);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    luaX_checksync(pthread_mutex_lock(&allocator->mutex));
    while (__atomic_load_n(&allocator->owner, __ATOMIC_SEQ_CST) != NULL)
      luaQ_wait(L, &allocator->mutex, &allocator->changed, &allocator->waiters);
    luaX_checksync(pthread_mutex_unlock(&allocator->mutex));
    /* Never own callback admission while inactive. Join any intervening stop
    ** and publish stable roots before retrying the fast ownership claim. */
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    luaW_waitresume(L);
    luaW_resume(L);
    /* A stack resize retains metadata ownership while pointers are offsets.
    ** Its entry already published roots; do not scan that partial frame. */
    if (luaW_scope(L).heap == 0) luaC_checksnapshot(L);
    L->native_depth--;
    __atomic_fetch_sub(&allocator->waiting, 1, __ATOMIC_SEQ_CST);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
}

void luaAL_leave (lua_State *L) {
  LuaAllocator *allocator = &G(L)->allocator;
  lua_assert(__atomic_load_n(&allocator->owner, __ATOMIC_RELAXED) ==
               luaX_token(L) && allocator->depth != 0);
  if (--allocator->depth == 0) {
    __atomic_store_n(&allocator->owner, NULL, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&allocator->waiting, __ATOMIC_SEQ_CST) != 0) {
      luaX_checksync(pthread_mutex_lock(&allocator->mutex));
      luaQ_wake(&allocator->changed, &allocator->waiters);
      luaX_checksync(pthread_mutex_unlock(&allocator->mutex));
    }
  }
}

void *luaAL_realloc (lua_State *L, void *block, size_t old, size_t size) {
  LuaAllocator *allocator = &G(L)->allocator;
  void *replacement;
  luaAL_enter(L);
  replacement = allocator->function(allocator->context, block, old, size);
  luaAL_leave(L);
  return replacement;
}
