/* Native call activation. See lua.h for license. */
#ifndef lcall_h
#define lcall_h

#include "laccess.h"
#include "lstate.h"

/* Admission can park. In particular, reserve foreign execution capacity
** before building a frame or reading arguments that can move with the stack. */
l_sinline void luaQ_begincall (lua_State *L, int managed, LuaCallScope *scope) {
  LuaExecution *execution = luaX_execution(L);
  scope->execution = execution;
  scope->managed = managed;
  if (!managed)
    luaQ_beginnative(L, execution);
  else if (execution != NULL) {
    scope->previous = execution->trusted;
    scope->native_depth = execution->native_depth;
    execution->trusted = scope;
  }
}

l_sinline void luaQ_entercall (lua_State *L, LuaCallScope *scope) {
  NativeStackContext *context;
  if (!scope->managed) {
    scope->world = luaW_detach(L);
    return;
  }
#if defined(LUAI_ASSERT)
  {
    LuaObjectScope *object;
    for (object = L->object_scopes; object != NULL; object = object->previous)
      lua_assert(!object->held);
  }
#endif
  L->native_depth++;
  scope->world = luaW_ownsheap(L) ? luaW_suspend(L) : (LuaWorldScope){0};
  context = luaX_context(L);
  scope->api.previous = context->api_region;
  scope->api.state = L;
  context->api_region = &scope->api;
}

l_sinline void luaQ_leavecall (lua_State *L, LuaCallScope *scope) {
  if (!scope->managed) {
    luaW_attach(L, scope->world);
    luaQ_endnative(L, scope->execution);
  }
  else {
    NativeStackContext *context = luaX_context(L);
    LuaExecution *execution = scope->execution;
    lua_assert(context->api_region == &scope->api && L->native_depth != 0);
    context->api_region = scope->api.previous;
    if (scope->world.heap != 0 || scope->world.collection != 0 ||
        luaW_ownsheap(L))
      luaW_restore(L, scope->world, L->native_depth);
    L->native_depth--;
    if (execution != NULL) {
      lua_assert(execution == luaX_execution(L) && execution->trusted == scope);
      execution->trusted = scope->previous;
    }
  }
}

#endif
