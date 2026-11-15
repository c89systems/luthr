/*
** Managed participation and collector rendezvous.
** See Copyright Notice in lua.h
*/
#define lworld_c
#define LUA_CORE
#include "lprefix.h"

#include "lapi.h"
#include "lgate.h"
#include "lstate.h"
#include "lworld.h"

int luaW_init (LuaWorld *world) {
  int status;
  world->ready = world->stopped = 0;
  world->active = 0;
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
  lua_assert(world->ready && !world->stopped && world->active == 0);
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
    luaX_checksync(pthread_cond_wait(&world->changed, &world->mutex));
  world->active++;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_leave (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(world->active != 0);
  world->active--;
  if (world->active == 0)
    luaX_checksync(pthread_cond_broadcast(&world->changed));
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
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  while (world->stopped) {
    L->native_depth++;
    lua_assert(world->active != 0);
    world->active--;
    luaX_checksync(pthread_cond_broadcast(&world->changed));
    do {
      luaX_checksync(pthread_cond_wait(&world->changed, &world->mutex));
    } while (world->stopped);
    world->active++;
    L->native_depth--;
  }
  world->stopped = 1;
  world->collector = pthread_self();
  luaX_checksync(pthread_cond_broadcast(&world->changed));
  lua_assert(world->active != 0);
  world->active--;
  while (world->active != 0)
    luaX_checksync(pthread_cond_wait(&world->changed, &world->mutex));
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
             pthread_equal(world->collector, pthread_self()));
  world->active = 1;  /* the collector resumes as an ordinary mutator */
  world->stopped = 0;
  luaX_checksync(pthread_cond_broadcast(&world->changed));
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

unsigned luaW_suspend (lua_State *L) {
  unsigned depth = L->world_depth;
  if (depth != 0) {
    L->world_depth = 1;
    luaW_start(L);
  }
  return depth;
}

void luaW_restore (lua_State *L, unsigned depth, unsigned native) {
  if (L->world_depth != depth) {
    (void)luaW_suspend(L);
    if (depth != 0) {
      luaW_stop(L);
      L->world_depth = depth;
    }
  }
  L->native_depth = native;
}

/* These boundaries retain the coroutine while native code blocks or reenters.
** Protected unwinding restores the enclosing depth if a callback never returns. */
unsigned luaW_detach (lua_State *L) {
  unsigned depth;
  L->native_depth++;
  depth = luaW_suspend(L);
  lua_unlock(L);
  return depth;
}

void luaW_attach (lua_State *L, unsigned depth) {
  lua_lock(L);
  if (depth != 0) {
    luaW_stop(L);
    L->world_depth = depth;
  }
  lua_assert(L->native_depth != 0);
  L->native_depth--;
}
