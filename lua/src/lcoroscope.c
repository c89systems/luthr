/* Coroutine operation ownership. See Copyright Notice in lua.h */
#define lcoroscope_c
#define LUA_CORE
#include "lprefix.h"

#include "lapi.h"
#include "lcoroscope.h"

int luaO_begin (lua_State *L, lua_State *co, LuaCoroutineScope *scope) {
  LuaWorld *world = &G(L)->world;
  int available;
  lua_assert(G(L) == G(co));
  luaX_lockpair(L, co);
  lua_assert(L->native_depth != 0);  /* caller's C activation */
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  available = co->operation == NULL && co->resume_active == 0 &&
              co->close_active == 0 && co->callback_active == 0 &&
              !(co->status == LUA_OK && co->ci != &co->base_ci);
  if (available) {
    scope->context = luaE_getnativestackcontext();
    scope->previous = scope->context->coroutine_scopes;
    scope->context->coroutine_scopes = scope;
    scope->source = L;
    scope->target = co;
    scope->held = 1;
    scope->next = G(L)->coroutine_scopes;
    scope->link = &G(L)->coroutine_scopes;
    if (scope->next != NULL) scope->next->link = &scope->next;
    G(L)->coroutine_scopes = scope;
    co->operation = scope;
    L->owned_operations++;
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaX_unlockpair(L, co);
  return available;
}

/* The metadata mutex also protects the GC root list. Unwinding can retire
** scopes from several Lua states without entering their stack gates. */
static void release (LuaCoroutineScope *scope) {
  if (scope->held) {
    lua_assert(scope->target->operation == scope);
    scope->target->operation = NULL;
    scope->held = 0;
  }
}

void luaO_end (LuaCoroutineScope *scope) {
  LuaWorld *world = &G(scope->source)->world;
  NativeStackContext *context = luaE_getnativestackcontext();
  lua_assert(context == scope->context && context->coroutine_scopes == scope);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  release(scope);
  lua_assert(scope->source->owned_operations != 0);
  scope->source->owned_operations--;
  *scope->link = scope->next;
  if (scope->next != NULL) scope->next->link = scope->link;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  context->coroutine_scopes = scope->previous;
}

void luaO_release (LuaCoroutineScope *boundary) {
  LuaCoroutineScope *scope = luaE_getnativestackcontext()->coroutine_scopes;
  for (; scope != boundary; scope = scope->previous) {
    LuaWorld *world;
    lua_assert(scope != NULL);
    world = &G(scope->source)->world;
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    release(scope);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
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
  luaX_checksync(pthread_mutex_lock(&G(co)->world.mutex));
  if (co->operation != NULL || co->resume_active || co->close_active ||
      co->callback_active)
    status = LUA_CO_NORMAL;
  else if (co->status == LUA_YIELD)
    status = LUA_CO_SUSPENDED;
  else if (co->status != LUA_OK)
    status = LUA_CO_DEAD;
  else if (co->ci != &co->base_ci)
    status = LUA_CO_NORMAL;
  else
    status = co->top.p == co->ci->func.p + 1 ? LUA_CO_DEAD : LUA_CO_SUSPENDED;
  luaX_checksync(pthread_mutex_unlock(&G(co)->world.mutex));
  lua_unlock(co);
  return status;
}
