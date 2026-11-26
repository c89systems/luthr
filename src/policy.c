#include "policy.h"
#include <lapi.h>
#include <lmem.h>
#include <lstate.h>
#include <stdlib.h>
#include <string.h>

typedef struct Invocation {
  LuaPolicy base;
  luthr_lua_task_options options;
  unsigned long remaining;
  pthread_mutex_t observer;
} Invocation;

static __thread Invocation *observing;

static void check(int status) {
  if (status != 0)
    abort();
}

static void hook(lua_State *L, lua_Debug *debug) {
  Invocation *invocation = (Invocation *)L->policy;
  if (invocation == NULL ||
      !__atomic_load_n(&invocation->base.active, __ATOMIC_ACQUIRE))
    return;
  if (debug->event == LUA_HOOKCOUNT &&
      invocation->options.instruction_limit != 0) {
    unsigned long remaining =
        __atomic_load_n(&invocation->remaining, __ATOMIC_RELAXED);
    while (remaining != 0) {
      if (__atomic_compare_exchange_n(&invocation->remaining, &remaining,
                                      remaining - 1, 0, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED)) {
        if (remaining > 1)
          return;
        break;
      }
    }
    __atomic_store_n(&invocation->base.exhausted, 1, __ATOMIC_RELEASE);
    lua_lock(L);
    luaCP_throwlimit(L);
  }
  if (debug->event == LUA_HOOKLINE && debug->currentline > 0 &&
      !__atomic_load_n(&invocation->base.exhausted, __ATOMIC_ACQUIRE) &&
      __atomic_load_n(&invocation->options.trace, __ATOMIC_ACQUIRE) != NULL) {
    luthr_lua_runtime_trace_event event;
    luthr_lua_runtime_trace_fn trace;
    void *context;
    Invocation *previous;
    LuaExecution *execution = luaQ_current();
    LuaWorldScope world;
    luaQ_beginnative(L, execution);
    world = luaW_suspendapi();
    check(pthread_mutex_lock(&invocation->observer));
    trace = invocation->options.trace;
    context = invocation->options.trace_context;
    if (trace != NULL && lua_getinfo(L, "S", debug)) {
      event.source = debug->source;
      event.source_size = debug->srclen;
      event.line = debug->currentline;
      previous = observing;
      observing = invocation;
      trace(context, &event);
      observing = previous;
    }
    check(pthread_mutex_unlock(&invocation->observer));
    luaW_restoreapi(world);
    luaQ_endnative(L, execution);
  }
}

static int hookmask(const luthr_lua_task_options *options) {
  return (options->instruction_limit != 0 ? LUA_MASKCOUNT : 0) |
         (options->trace != NULL ? LUA_MASKLINE : 0);
}

static void release(lua_State *L, LuaPolicy *base) {
  Invocation *invocation = (Invocation *)base;
  check(pthread_mutex_destroy(&invocation->observer));
  luaM_free_(L, invocation, sizeof(*invocation));
}

LuaPolicy *luthr_policy_new(lua_State *L, const luthr_lua_task_options *options,
                            void *owner) {
  Invocation *invocation;
  luaQ_setmanagedhook(L, hook);
  lua_lock(L);
  invocation = luaM_alloccontrol(L, sizeof(*invocation));
  if (invocation != NULL) {
    memset(invocation, 0, sizeof(*invocation));
    if (pthread_mutex_init(&invocation->observer, NULL) != 0) {
      luaM_free_(L, invocation, sizeof(*invocation));
      invocation = NULL;
    } else {
      if (options != NULL)
        invocation->options = *options;
      invocation->remaining = invocation->options.instruction_limit;
      invocation->base.global = G(L);
      invocation->base.references = 1;
      invocation->base.active = 1;
      invocation->base.owner = owner;
      invocation->base.hook = hook;
      invocation->base.hookmask = hookmask(&invocation->options);
      invocation->base.release = release;
    }
  }
  lua_unlock(L);
  return invocation != NULL ? &invocation->base : NULL;
}

void luthr_policy_context(LuaPolicy *base, void *context) {
  if (base != NULL)
    __atomic_store_n(&((Invocation *)base)->options.context, context,
                     __ATOMIC_RELEASE);
}

void luthr_policy_reset(LuaPolicy *base,
                        const luthr_lua_task_options *options) {
  Invocation *invocation = (Invocation *)base;
  invocation->options = *options;
  invocation->remaining = options->instruction_limit;
  __atomic_store_n(&base->hookmask, hookmask(options), __ATOMIC_RELEASE);
  __atomic_store_n(&base->exhausted, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&base->active, 1, __ATOMIC_RELEASE);
}
void *luthr_policy_get_context(LuaPolicy *base) {
  return base != NULL ? __atomic_load_n(&((Invocation *)base)->options.context,
                                        __ATOMIC_ACQUIRE)
                      : NULL;
}
void luthr_policy_trace(LuaPolicy *base, luthr_lua_runtime_trace_fn trace,
                        void *context) {
  Invocation *invocation = (Invocation *)base;
  /* A trace callback already owns this invocation's observer mutex. */
  if (observing != invocation)
    check(pthread_mutex_lock(&invocation->observer));
  invocation->options.trace_context = trace != NULL ? context : NULL;
  __atomic_store_n(&invocation->options.trace, trace, __ATOMIC_RELEASE);
  __atomic_store_n(&base->hookmask, hookmask(&invocation->options),
                   __ATOMIC_RELEASE);
  if (observing != invocation)
    check(pthread_mutex_unlock(&invocation->observer));
}
int luthr_policy_tracing(void *owner) {
  return observing != NULL && observing->base.owner == owner;
}
LuaPolicy *luthr_policy_current(void *owner) {
  LuaPolicy *policy = luaCP_current();
  if (observing != NULL && observing->base.owner == owner)
    return &observing->base;
  return policy != NULL && policy->owner == owner ? policy : NULL;
}
LuaPolicy *luthr_policy_from_state(lua_State *L) { return L->policy; }
void luthr_policy_set_factory(lua_State *L, LuaPolicyFactory factory,
                              void *context) {
  lua_lock(L);
  G(L)->policy_factory = factory;
  G(L)->policy_context = context;
  lua_unlock(L);
}
