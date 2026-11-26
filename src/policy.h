#ifndef LUTHR_PRIVATE_POLICY_H
#define LUTHR_PRIVATE_POLICY_H
#include <lpolicy.h>
#include <luthr/tasks.h>

LuaPolicy *luthr_policy_new(lua_State *, const luthr_lua_task_options *,
                            void *);
void luthr_policy_reset(LuaPolicy *, const luthr_lua_task_options *);
void luthr_policy_context(LuaPolicy *, void *);
void *luthr_policy_get_context(LuaPolicy *);
void luthr_policy_trace(LuaPolicy *, luthr_lua_runtime_trace_fn, void *);
int luthr_policy_tracing(void *owner);
LuaPolicy *luthr_policy_current(void *owner);
LuaPolicy *luthr_policy_from_state(lua_State *);
void luthr_policy_set_factory(lua_State *, LuaPolicyFactory, void *);
#endif
