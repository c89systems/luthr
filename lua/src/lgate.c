/*
** Per-coroutine stack ownership and VM safepoints.
** See Copyright Notice in lua.h
*/
#define lgate_c
#define LUA_CORE
#include "lprefix.h"

#include <stdio.h>
#include <stdlib.h>
#include "lgate.h"
#include "lstate.h"

void luaX_checksync (int status) {
  if (status != 0) {
    fprintf(stderr, "Luthrum internal synchronization failure: %d\n", status);
    abort();
  }
}

int luaX_initgate (LuaStackGate *gate) {
  int status;
  gate->ready = gate->owned = 0;
  gate->next_ticket = gate->serving = 0;
  status = pthread_mutex_init(&gate->mutex, NULL);
  if (status != 0)
    return status;
  status = pthread_cond_init(&gate->changed, NULL);
  if (status != 0) {
    luaX_checksync(pthread_mutex_destroy(&gate->mutex));
    return status;
  }
  gate->ready = 1;
  return 0;
}

void luaX_destroygate (LuaStackGate *gate) {
  if (!gate->ready)
    return;
  lua_assert(!gate->owned && gate->next_ticket == gate->serving);
  luaX_checksync(pthread_cond_destroy(&gate->changed));
  luaX_checksync(pthread_mutex_destroy(&gate->mutex));
  gate->ready = 0;
}

void luaX_lockstate (lua_State *L) {
  LuaStackGate *gate = &L->stack_gate;
  unsigned long ticket;
  luaX_checksync(pthread_mutex_lock(&gate->mutex));
  /* Recursive entry is an internal boundary error, not a wait for ourselves. */
  if (gate->owned && pthread_equal(gate->owner, pthread_self())) {
    fputs("Luthrum recursive coroutine stack gate entry\n", stderr);
    abort();
  }
  ticket = gate->next_ticket++;
  while (ticket != gate->serving)
    luaX_checksync(pthread_cond_wait(&gate->changed, &gate->mutex));
  lua_assert(!gate->owned);
  gate->owner = pthread_self();
  gate->owned = 1;
  luaX_checksync(pthread_mutex_unlock(&gate->mutex));
  luaW_enter(L);
}

void luaX_unlockstate (lua_State *L) {
  LuaStackGate *gate = &L->stack_gate;
  lua_assert(L->world_depth == 0);
  luaX_checksync(pthread_mutex_lock(&gate->mutex));
  if (!gate->owned || !pthread_equal(gate->owner, pthread_self())) {
    fputs("Luthrum coroutine stack gate released by a non-owner\n", stderr);
    abort();
  }
  gate->owned = 0;
  gate->serving++;
  luaX_checksync(pthread_cond_broadcast(&gate->changed));
  luaX_checksync(pthread_mutex_unlock(&gate->mutex));
  /* Keep participation until all physical gate accesses have finished. A
  ** collector may reclaim this state after the final leave; do not touch it. */
  luaW_leave(L);
}

void luaX_poll (lua_State *L) {
  unsigned depth;
  /* Internal poll handlers execute as managed VM code, with stack ownership.
  ** They are not ordinary C bindings and do not detach automatically. */
  if (L->state_poll != NULL)
    L->state_poll(L, L->state_poll_context);
  depth = luaW_detach(L);
  luaW_attach(L, depth);
}
