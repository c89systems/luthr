#ifndef LUTHRUM_LUA_RUNTIME_HOOK_POLICY_H
#define LUTHRUM_LUA_RUNTIME_HOOK_POLICY_H

/* Private opt-in understood by the owned Lua collector. No public hook event
 * uses this bit; ordinary Lua hooks retain native finalizer suppression. */
#define LUTHRUM_LUA_RUNTIME_MASK_FINALIZERS 16

#endif
