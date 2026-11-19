/* Registered callback entry and roots. See Copyright Notice in lua.h */
#define lcallback_c
#define LUA_CORE
#include "lprefix.h"

#include <string.h>
#include "lapi.h"
#include "laccess.h"
#include "lcallback.h"
#include "ldo.h"
#include "lstate.h"
#include "lstring.h"
#include "ltask.h"

static void createthread (lua_State *L, void *context) {
  LuaCallback *callback = cast(LuaCallback *, context);
  LuaWorldScope scope;
  luaD_checkstack(L, 2);
  scope = luaW_detach(L);
  callback->thread = lua_newthread(L);
  luaW_attach(L, scope);
  callback->close_error =
    luaS_newliteral(L, "cannot close an active registered callback");
  setsvalue2s(L, L->top.p++, callback->close_error);
}

int luaCB_init (lua_State *L, LuaCallback *callback, unsigned limit) {
  int status;
  ptrdiff_t top;
  memset(callback, 0, sizeof(*callback));
  setnilvalue(&callback->function);
  lua_lock(L);
  if (L->top.p <= L->ci->func.p + 1 ||
      !ttisfunction(s2v(L->top.p - 1))) {
    lua_unlock(L);
    return LUA_CB_INVALID;
  }
  if (pthread_mutex_init(&callback->mutex, NULL) != 0) {
    lua_unlock(L);
    return LUA_ERRMEM;
  }
  if (pthread_cond_init(&callback->changed, NULL) != 0) {
    luaX_checksync(pthread_mutex_destroy(&callback->mutex));
    lua_unlock(L);
    return LUA_ERRMEM;
  }
  callback->limit = limit == 0 ? 1 : limit;
  luaA_sharevalue(L, s2v(L->top.p - 1));
  setobj(L, &callback->function, s2v(L->top.p - 1));
  top = savestack(L, L->top.p);
  status = luaD_pcall(L, createthread, callback, top, 0);
  if (status == LUA_OK) {
    LuaObjectScope access;
    luaA_beginlock(L, &G(L)->callbacks_access, &access);
    callback->next = G(L)->callbacks;
    callback->previous = &G(L)->callbacks;
    if (callback->next != NULL)
      callback->next->previous = &callback->next;
    G(L)->callbacks = callback;
    callback->thread->callback_owner = callback;
    callback->ready = 1;
    luaA_end(L, &access);
  }
  else {
    callback->thread = NULL;
    callback->close_error = NULL;
    setnilvalue(&callback->function);
    luaX_checksync(pthread_cond_destroy(&callback->changed));
    luaX_checksync(pthread_mutex_destroy(&callback->mutex));
  }
  L->top.p = restorestack(L, top);
  lua_unlock(L);
  return status;
}

struct Invocation {
  LuaCallback *callback;
  void (*invoke)(lua_State *, void *);
  void *context;
};

static int invoke (lua_State *L) {
  struct Invocation *call = cast(struct Invocation *, lua_touserdata(L, 1));
  lua_pushvalue(L, 2);
  call->invoke(L, call->context);
  /* Provider marshalling can itself be opaque work. A stop requested while
  ** it blocks is delivered only after that activation returns on its owner. */
  lua_lock(L);
  luaTK_checkstop(L);
  lua_unlock(L);
  return 0;
}

static void protectedinvoke (lua_State *L, void *context) {
  struct Invocation *call = cast(struct Invocation *, context);
  StkId function;
  luaD_checkstack(L, 3);
  function = L->top.p;
  setfvalue(s2v(L->top.p++), invoke);
  setpvalue(s2v(L->top.p++), call);
  setobj2s(L, L->top.p++, &call->callback->function);
  luaD_callnoyield(L, function, 0);
}

static void copyerror (lua_State *L, char *error, size_t size) {
  const char *message = "Lua callback failed with a non-string error";
  size_t length = strlen(message);
  if (size == 0)
    return;
  if (ttisstring(s2v(L->top.p - 1))) {
    TString *string = tsvalue(s2v(L->top.p - 1));
    message = getstr(string);
    length = tsslen(string);
  }
  if (length >= size)
    length = size - 1;
  memcpy(error, message, length);
  error[length] = '\0';
}

int luaCB_call (LuaCallback *callback, LuaExecution *execution,
                 void (*function)(lua_State *, void *), void *context,
                 char *error, size_t error_size) {
  LuaExecution *current = luaQ_current();
  NativeStackContext native = {0};
  NativeStackContext *previous = NULL;
  struct Invocation call;
  lua_State *L;
  ptrdiff_t top;
  int attached = 0, nested, status;
  if (error_size != 0) {
    if (error == NULL)
      return LUA_CB_INVALID;
    error[0] = '\0';
  }
  if (function == NULL)
    return LUA_CB_INVALID;
  if (current == NULL) {
    NativeStackContext *source = luaE_getnativestackcontext();
    if (execution == NULL || execution->managed_depth != 0 ||
        execution->native_depth != 0 || execution->pin_depth != 0 ||
        execution->trusted != NULL ||
        execution->protected != NULL ||
        __atomic_load_n(&execution->task, __ATOMIC_ACQUIRE) != NULL)
      return LUA_CB_INVALID;
    execution->native_depth = 1;  /* already on a provider-owned native stack */
    native.limit = source->limit;
    native.overflow = source->overflow;
    native.execution = execution;
    previous = luaE_setnativestackcontext(&native);
    attached = 1;
  }
  else if ((execution != NULL && execution != current) ||
           current->managed_depth != 0 || current->native_depth == 0)
    return LUA_CB_INVALID;
  luaX_checksync(pthread_mutex_lock(&callback->mutex));
  nested = callback->depth != 0 && luaQ_isowner(&callback->owner) &&
           pthread_equal(callback->carrier, pthread_self());
  if (callback->revoked) status = LUA_CB_REVOKED;
  else if (!nested && callback->entries == callback->limit)
    status = LUA_CB_CAPACITY;
  else {
    if (!nested) {
      callback->entries++;
      callback->waiters++;
      luaX_checksync(pthread_cond_broadcast(&callback->changed));
      while (callback->depth != 0 && !callback->revoked)
        luaX_checksync(pthread_cond_wait(&callback->changed, &callback->mutex));
      callback->waiters--;
    }
    if (callback->revoked) {
      callback->entries--;
      status = LUA_CB_REVOKED;
    }
    else {
      callback->depth++;
      luaQ_setowner(&callback->owner);
      callback->carrier = pthread_self();
      luaX_checksync(pthread_mutex_unlock(&callback->mutex));
      L = callback->thread;
      call.callback = callback;
      call.invoke = function;
      call.context = context;
      lua_lock(L);
      L->callback_active++;
      top = savestack(L, L->top.p);
      status = luaD_pcall(L, protectedinvoke, &call, top, 0);
      if (status != LUA_OK)
        copyerror(L, error, error_size);
      L->top.p = restorestack(L, top);
      L->callback_active--;
      lua_unlock(L);
      luaX_checksync(pthread_mutex_lock(&callback->mutex));
      callback->depth--;
      if (!nested) callback->entries--;
    }
  }
  luaX_checksync(pthread_cond_broadcast(&callback->changed));
  luaX_checksync(pthread_mutex_unlock(&callback->mutex));
  if (attached) {
    lua_assert(execution->managed_depth == 0 && execution->native_depth == 1 &&
               execution->protected == NULL && execution->pin_depth == 0 &&
               execution->trusted == NULL &&
               native.coroutine_scopes == NULL);
    execution->native_depth = 0;
    luaE_setnativestackcontext(previous);
  }
  return status;
}

void luaCB_revoke (LuaCallback *callback) {
  luaX_checksync(pthread_mutex_lock(&callback->mutex));
  callback->revoked = 1;
  luaX_checksync(pthread_cond_broadcast(&callback->changed));
  luaX_checksync(pthread_mutex_unlock(&callback->mutex));
}

int luaCB_destroy (lua_State *L, LuaCallback *callback) {
  lua_State *thread;
  LuaObjectScope access;
  luaX_checksync(pthread_mutex_lock(&callback->mutex));
  if (!callback->revoked || !callback->ready || G(L) != G(callback->thread)) {
    luaX_checksync(pthread_mutex_unlock(&callback->mutex));
    return LUA_CB_INVALID;
  }
  if (callback->entries != 0 || callback->depth != 0) {
    luaX_checksync(pthread_mutex_unlock(&callback->mutex));
    return LUA_CB_PENDING;
  }
  callback->ready = 0;
  thread = callback->thread;
  luaX_checksync(pthread_mutex_unlock(&callback->mutex));
  luaX_lockpair(L, thread);
  luaA_beginlock(L, &G(L)->callbacks_access, &access);
  *callback->previous = callback->next;
  if (callback->next != NULL)
    callback->next->previous = callback->previous;
  callback->previous = NULL;
  callback->next = NULL;
  callback->thread = NULL;
  thread->callback_owner = NULL;
  callback->close_error = NULL;
  setnilvalue(&callback->function);
  luaA_end(L, &access);
  luaX_unlockpair(L, thread);
  luaX_checksync(pthread_cond_destroy(&callback->changed));
  luaX_checksync(pthread_mutex_destroy(&callback->mutex));
  return LUA_OK;
}
