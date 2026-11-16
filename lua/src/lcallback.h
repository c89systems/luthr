/* Private registered native callbacks. See Copyright Notice in lua.h */
#ifndef lcallback_h
#define lcallback_h

#include "lexec.h"
#include "lobject.h"

/* Negative results describe registration/admission; nonnegative results are
** ordinary Lua protected-call statuses. Storage outlives provider acknowledgment
** and all entered calls. Destroy is not acknowledgment from a native provider. */
enum {
  LUA_CB_REVOKED = -1,
  LUA_CB_CAPACITY = -2,
  LUA_CB_PENDING = -3,
  LUA_CB_INVALID = -4
};

typedef struct LuaCallback {
  struct LuaCallback *next;
  struct LuaCallback **previous;
  lua_State *thread;
  TValue function;
  TString *close_error;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  LuaOwner owner;
  pthread_t carrier;
  unsigned entries;
  unsigned depth;
  unsigned waiters;
  unsigned limit;
  int revoked;
  int ready;
} LuaCallback;

/* The function on top is retained without consumption. Zero selects one
** admitted outer entry. Same-carrier nested calls share their outer entry. */
LUAI_FUNC int luaCB_init (lua_State *L, LuaCallback *callback, unsigned limit);
/* invoke receives a copy of the registered function on top and may use ordinary
** Lua APIs. Its entire invocation, including marshalling and cleanup, is
** protected. It cannot yield through this synchronous native boundary.
** Closing its registered coroutine while active returns LUA_ERRRUN without
** abandoning the callback; revoke/destroy govern registration lifetime.
** An adapter must contain foreign exceptions and non-Lua nonlocal exits.
** A foreign caller supplies a quiescent execution with scheduler hooks; an
** already attached caller supplies NULL or its current execution. */
LUAI_FUNC int luaCB_call (LuaCallback *callback, LuaExecution *execution,
                         void (*invoke)(lua_State *, void *), void *context,
                         char *error, size_t error_size);
LUAI_FUNC void luaCB_revoke (LuaCallback *callback);
/* Call only after the provider has acknowledged that no future entry can start.
** An active/queued call returns PENDING without waiting or dropping roots. */
LUAI_FUNC int luaCB_destroy (lua_State *L, LuaCallback *callback);

#endif
