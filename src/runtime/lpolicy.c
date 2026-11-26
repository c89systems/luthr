#define LUA_CORE
#include "lprefix.h"
#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lpolicy.h"
#include "lstate.h"
#include "luthr_hook_policy.h"

LuaPolicy *luaCP_current (void) {
  LuaThreadContext *thread = luaE_threadcontext();
  LuaExecution *execution = luaE_resolvestack(thread)->execution;
  return execution != NULL ? execution->policy : thread->policy;
}

void luaCP_retain (LuaPolicy *policy) {
  if (policy != NULL) __atomic_fetch_add(&policy->references, 1, __ATOMIC_RELAXED);
}

void luaCP_release (lua_State *L, LuaPolicy *policy) {
  if (policy != NULL &&
      __atomic_fetch_sub(&policy->references, 1, __ATOMIC_ACQ_REL) == 1)
    policy->release(L, policy);
}

void luaCP_bind (lua_State *L, LuaPolicy *policy) {
  if (L->policy != policy) {
    LuaPolicy *previous = L->policy;
    luaCP_retain(policy);
    L->policy = policy;
    luaCP_release(L, previous);
  }
}

void luaCP_clear (lua_State *L) {
  lua_assert(L->policy != NULL);
  if (gethook(L) == L->policy->hook) lua_sethook(L, NULL, 0, 0);
  luaCP_bind(L, NULL);
}

void luaCP_apply (lua_State *L, LuaPolicy *policy) {
  if (policy != L->policy && policy != NULL && policy->global == G(L))
    luaCP_bind(L, policy);
  policy = L->policy;
  if (policy != NULL) {
    int mask = __atomic_load_n(&policy->hookmask, __ATOMIC_ACQUIRE);
    lua_Hook hook = gethook(L);
    if (mask != 0 && __atomic_load_n(&policy->active, __ATOMIC_ACQUIRE)) {
      int count = (mask & LUA_MASKCOUNT) != 0;
      mask |= LUTHR_LUA_RUNTIME_MASK_FINALIZERS;
      if (hook != policy->hook || gethookmask(L) != mask ||
          getbasehookcount(L) != count)
        lua_sethook(L, policy->hook, mask, count);
    }
    else if (hook == policy->hook) lua_sethook(L, NULL, 0, 0);
  }
}

void luaCP_begin (LuaPolicy *policy, LuaPolicyScope *scope) {
  scope->execution = luaQ_current();
  if (scope->execution != NULL) {
    scope->previous = scope->execution->policy;
    scope->execution->policy = policy;
  }
  else {
    LuaThreadContext *thread = luaE_threadcontext();
    scope->previous = thread->policy;
    thread->policy = policy;
  }
}

void luaCP_end (LuaPolicyScope *scope) {
  if (scope->execution != NULL) scope->execution->policy = scope->previous;
  else luaE_threadcontext()->policy = scope->previous;
}

int luaCP_exhausted (lua_State *L) {
  LuaPolicy *policy = L->policy;
  return policy != NULL && __atomic_load_n(&policy->active, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&policy->exhausted, __ATOMIC_ACQUIRE);
}

void luaCP_throwlimit (lua_State *L) {
  lua_assert(L->top.p < L->stack_last.p + EXTRA_STACK);
  setsvalue2s(L, L->top.p++, G(L)->limiterrmsg);
  luaD_throw(L, LUA_ERRRUN);
}

void luaCP_checklimit (lua_State *L) {
  if (luaCP_exhausted(L)) luaCP_throwlimit(L);
}
