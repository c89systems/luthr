/* Coroutine operation ownership. See Copyright Notice in lua.h */
#define lcoroscope_c
#define LUA_CORE
#include "lprefix.h"

#include "lapi.h"
#include "lcoroscope.h"
#include "lgc.h"

int luaO_claim (lua_State *L, lua_State *co, LuaCoroutineScope *scope) {
  int available;
  lua_assert(G(L) == G(co));
  lua_assert(L->native_depth != 0);  /* caller's C activation */
  available = co->task_owner == NULL &&
              __atomic_load_n(&co->operation, __ATOMIC_ACQUIRE) == NULL &&
              co->resume_active == 0 && co->close_active == 0 &&
              co->callback_active == 0 &&
              !(co->status == LUA_OK && co->ci != &co->base_ci);
  if (available) {
    scope->context = luaE_getnativestackcontext();
    scope->previous = scope->context->coroutine_scopes;
    scope->context->coroutine_scopes = scope;
    scope->source = L;
    scope->target = co;
    scope->held = 1;
    /* Both stacks are enrolled and protected from collection here. A later
    ** trace sees these roots in the participation directory. A trace that
    ** already sampled the directory gets both through the producer barrier. */
    __atomic_fetch_add(&L->owned_operations, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&co->operation_roots, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&co->operation, scope, __ATOMIC_RELEASE);
    luaC_tracepointer(L, L);
    luaC_tracepointer(L, co);
  }
  return available;
}

int luaO_begin (lua_State *L, lua_State *co, LuaCoroutineScope *scope) {
  int available;
  luaX_lockpair(L, co);
  available = luaO_claim(L, co, scope);
  luaX_unlockpair(L, co);
  return available;
}
/* Recovery can yield the claim to an error handler while keeping the target
** rooted. Unwinding can retire scopes without borrowing either Lua stack. */
static void release (LuaCoroutineScope *scope) {
  if (scope->held) {
    lua_assert(__atomic_load_n(&scope->target->operation, __ATOMIC_RELAXED) == scope);
    __atomic_store_n(&scope->target->operation, NULL, __ATOMIC_RELEASE);
    scope->held = 0;
  }
}

void luaO_end (LuaCoroutineScope *scope) {
  NativeStackContext *context = luaE_getnativestackcontext();
  lua_assert(context == scope->context && context->coroutine_scopes == scope);
  release(scope);
  context->coroutine_scopes = scope->previous;
  lua_assert(__atomic_load_n(&scope->source->owned_operations, __ATOMIC_RELAXED) != 0);
  lua_assert(__atomic_load_n(&scope->target->operation_roots, __ATOMIC_RELAXED) != 0);
  __atomic_fetch_sub(&scope->source->owned_operations, 1, __ATOMIC_RELEASE);
  __atomic_fetch_sub(&scope->target->operation_roots, 1, __ATOMIC_RELEASE);
}

void luaO_release (LuaCoroutineScope *boundary) {
  LuaCoroutineScope *scope = luaE_getnativestackcontext()->coroutine_scopes;
  for (; scope != boundary; scope = scope->previous) {
    lua_assert(scope != NULL);
    release(scope);
  }
}

void luaO_unwind (LuaCoroutineScope *boundary) {
  NativeStackContext *context = luaE_getnativestackcontext();
  while (context->coroutine_scopes != boundary) {
    lua_assert(context->coroutine_scopes != NULL);
    luaO_end(context->coroutine_scopes);
  }
}

int luaO_status (lua_State *L, lua_State *co) {
  int status;
  if (L == co) return LUA_CO_RUNNING;
  lua_lock(co);
  if (co->task_owner != NULL ||
      __atomic_load_n(&co->operation, __ATOMIC_ACQUIRE) != NULL || co->resume_active ||
      co->close_active || co->callback_active)
    status = LUA_CO_NORMAL;
  else if (co->status == LUA_YIELD)
    status = LUA_CO_SUSPENDED;
  else if (co->status != LUA_OK)
    status = LUA_CO_DEAD;
  else if (co->ci != &co->base_ci)
    status = LUA_CO_NORMAL;
  else
    status = co->top.p == co->ci->func.p + 1 ? LUA_CO_DEAD : LUA_CO_SUSPENDED;
  lua_unlock(co);
  return status;
}
