/* Bounded, task-aware chunk transport. See Copyright Notice in lua.h */
#define lpipe_c
#define LUA_CORE
#include "lprefix.h"

#include <string.h>
#include "lapi.h"
#include "laccess.h"
#include "lauxlib.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lpipe.h"
#include "ltask.h"

static const char identity;
typedef struct Pipe {
  const void *identity;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  LuaWaiter *waiters;
  unsigned capacity, head, count, calls, sending, receiving;
  size_t bytes, peak;
  lua_Unsigned sent, received;
  int ready, closed, finalizing;
} Pipe;

typedef struct Access {
  Udata *owner;
  Pipe *pipe;
  LuaObjectScope object;
} Access;

/* Object and metadata ownership precede the physical transport mutex. No Lua
** API, allocation or scheduler park runs while that mutex remains acquired. */
static Pipe *enter (lua_State *L, Access *access, Udata *owner) {
  lua_lock(L);
  if (owner == NULL) {
    if (L->top.p <= L->ci->func.p + 1 ||
        !ttisfulluserdata(s2v(L->ci->func.p + 1))) {
      lua_unlock(L);
      luaL_argerror(L, 1, "chunk pipe expected");
    }
    owner = uvalue(s2v(L->ci->func.p + 1));
  }
  luaA_begin(L, obj2gco(owner), &access->object);
  if (owner->metatable != __atomic_load_n(&G(L)->pipe_meta, __ATOMIC_ACQUIRE) ||
      owner->len != sizeof(Pipe) ||
      owner->nuvalue == 0 || owner->nuvalue > LUA_PIPE_SLOTS_MAX) {
    luaA_end(L, &access->object);
    lua_unlock(L);
    luaL_argerror(L, 1, "chunk pipe expected");
  }
  access->owner = owner;
  access->pipe = cast(Pipe *, getudatamem(owner));
  if (access->pipe->identity != &identity ||
      access->pipe->capacity != owner->nuvalue) {
    luaA_end(L, &access->object);
    lua_unlock(L);
    luaL_argerror(L, 1, "chunk pipe expected");
  }
  if (access->pipe->ready)
    luaX_checksync(pthread_mutex_lock(&access->pipe->mutex));
  return access->pipe;
}

static void leave (lua_State *L, Access *access) {
  if (access->pipe->ready)
    luaX_checksync(pthread_mutex_unlock(&access->pipe->mutex));
  luaA_end(L, &access->object);
  lua_unlock(L);
}

static void discard (lua_State *L, Access *access) {
  unsigned i;
  for (i = 0; i < access->pipe->capacity; ++i) {
    luaC_tracevalue(L, &access->owner->uv[i].uv);
    setnilvalue(&access->owner->uv[i].uv);
  }
  access->pipe->head = access->pipe->count = 0;
  access->pipe->bytes = 0;
}

/* The object lock prevents new admission between the last lease and mutex
** destruction, including when __gc is explicitly called while tasks wait. */
static void retire (lua_State *L, Access *access) {
  Pipe *pipe = access->pipe;
  lua_assert(pipe->ready && pipe->calls != 0);
  pipe->calls--;
  if (pipe->calls == 0 && pipe->finalizing) {
    lua_assert(pipe->waiters == NULL && pipe->sending == 0 && pipe->receiving == 0);
    pipe->ready = 0;
    luaX_checksync(pthread_mutex_unlock(&pipe->mutex));
    luaX_checksync(pthread_cond_destroy(&pipe->changed));
    luaX_checksync(pthread_mutex_destroy(&pipe->mutex));
  }
  leave(L, access);
}

static int waitforchange (lua_State *L, Access *access, unsigned *waiting) {
  Pipe *pipe = access->pipe;
  LuaWorldScope scope;
  int stopped;
  (*waiting)++;
  /* Stable userdata arguments and the detached C activation retain the owner.
  ** Only native queue bookkeeping remains under this mutex after withdrawal. */
  luaA_end(L, &access->object);
  scope = luaW_suspendapi();
  if (scope.api_region == NULL) lua_unlock(L);
  stopped = luaQ_waitstop(L, &pipe->mutex, &pipe->changed, &pipe->waiters);
  (*waiting)--;
  luaX_checksync(pthread_mutex_unlock(&pipe->mutex));
  luaW_restoreapi(scope);
  return stopped;
}

static int sendchunk (lua_State *L) {
  size_t size;
  Access access;
  Udata *owner;
  Pipe *pipe;
  luaL_checklstring(L, 2, &size);
  luaL_argcheck(L, size <= LUA_PIPE_CHUNK_MAX, 2, "chunk exceeds 1024 bytes");
  pipe = enter(L, &access, NULL);
  owner = access.owner;
  if (!pipe->ready) {
    leave(L, &access);
    return luaL_error(L, "pipe is closed");
  }
  pipe->calls++;
  for (;;) {
    if (pipe->closed) {
      retire(L, &access);
      return luaL_error(L, "pipe is closed");
    }
    if (pipe->count < pipe->capacity) {
      const TValue *chunk = s2v(L->ci->func.p + 2);
      unsigned slot = (pipe->head + pipe->count) % pipe->capacity;
      lua_assert(ttisstring(chunk) && tsslen(tsvalue(chunk)) == size);
      luaC_prewrite(L, &owner->uv[slot].uv, chunk);
      setobj(L, &owner->uv[slot].uv, chunk);
      luaC_barrier(L, owner, chunk);
      pipe->count++;
      pipe->bytes += size;
      if (pipe->peak < pipe->bytes) pipe->peak = pipe->bytes;
      pipe->sent += size;
      luaQ_wake(&pipe->changed, &pipe->waiters);
      retire(L, &access);
      lua_pushinteger(L, cast(lua_Integer, size));
      return 1;
    }
    int stopped = waitforchange(L, &access, &pipe->sending);
    pipe = enter(L, &access, owner);
    lua_assert(pipe->ready); /* This operation still owns its admission. */
    if (stopped) {
      retire(L, &access);
      lua_lock(L);
      luaTK_throwstop(L);
    }
  }
}

static int receivechunk (lua_State *L) {
  Access access;
  Pipe *pipe = enter(L, &access, NULL);
  Udata *owner = access.owner;
  if (!pipe->ready) {
    leave(L, &access);
    lua_pushnil(L);
    return 1;
  }
  pipe->calls++;
  for (;;) {
    if (pipe->count != 0) {
      TValue *chunk = &owner->uv[pipe->head].uv;
      size_t size;
      if (!ttisstring(chunk) || (size = tsslen(tsvalue(chunk))) > LUA_PIPE_CHUNK_MAX ||
          size > pipe->bytes) {
        discard(L, &access);
        pipe->closed = 1;
        luaQ_wake(&pipe->changed, &pipe->waiters);
        retire(L, &access);
        return luaL_error(L, "pipe storage was modified outside its operations");
      }
      /* Existing immutable chunk, transferred without allocating or copying the
      ** body. Preserve its root before retiring the slot. */
      setobj2s(L, L->top.p, chunk);
      api_incr_top(L);
      luaC_tracevalue(L, chunk);
      setnilvalue(chunk);
      pipe->head = (pipe->head + 1) % pipe->capacity;
      pipe->count--;
      pipe->bytes -= size;
      pipe->received += size;
      luaQ_wake(&pipe->changed, &pipe->waiters);
      retire(L, &access);
      return 1;
    }
    if (pipe->closed) {
      retire(L, &access);
      lua_pushnil(L);
      return 1;
    }
    int stopped = waitforchange(L, &access, &pipe->receiving);
    pipe = enter(L, &access, owner);
    lua_assert(pipe->ready);
    if (stopped) {
      retire(L, &access);
      lua_lock(L);
      luaTK_throwstop(L);
    }
  }
}

static int closepipe (lua_State *L, int finalizing) {
  Access access;
  Pipe *pipe = enter(L, &access, NULL);
  if (pipe->ready) {
    pipe->closed = 1;
    if (finalizing) {
      pipe->finalizing = 1;
      discard(L, &access);
    }
    luaQ_wake(&pipe->changed, &pipe->waiters);
    /* Reuse retirement so explicit finalization and real GC obey one path. */
    pipe->calls++;
    retire(L, &access);
  }
  else leave(L, &access);
  lua_pushboolean(L, 1);
  return 1;
}
static int closemethod (lua_State *L) { return closepipe(L, 0); }
static int finalize (lua_State *L) { return closepipe(L, 1); }

static int stats (lua_State *L) {
  Access access;
  Pipe *pipe = enter(L, &access, NULL);
  unsigned capacity = pipe->capacity, sending = pipe->sending, receiving = pipe->receiving;
  size_t bytes = pipe->bytes, peak = pipe->peak;
  lua_Unsigned sent = pipe->sent, received = pipe->received;
  int closed = pipe->closed, ready = pipe->ready;
  leave(L, &access);
  lua_createtable(L, 0, 9);
#define FIELD(name, value) \
  lua_pushinteger(L, cast(lua_Integer, (value))); lua_setfield(L, -2, (name))
  FIELD("capacity", capacity);
  FIELD("buffered_bytes", bytes);
  FIELD("peak_bytes", peak);
  FIELD("sent_bytes", sent);
  FIELD("received_bytes", received);
  FIELD("waiting_senders", sending);
  FIELD("waiting_receivers", receiving);
#undef FIELD
  lua_pushboolean(L, closed); lua_setfield(L, -2, "closed");
  lua_pushboolean(L, ready); lua_setfield(L, -2, "ready");
  return 1;
}

static void metatable (lua_State *L) {
  Table *shared;
  lua_lock(L);
  shared = __atomic_load_n(&G(L)->pipe_meta, __ATOMIC_ACQUIRE);
  if (shared != NULL) {
    sethvalue2s(L, L->top.p, shared);
    api_incr_top(L);
    lua_unlock(L);
    return;
  }
  lua_unlock(L);
  lua_newtable(L);
  lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
  luaQ_pushmanaged(L, sendchunk, 0); lua_setfield(L, -2, "send");
  luaQ_pushmanaged(L, receivechunk, 0); lua_setfield(L, -2, "recv");
  luaQ_pushmanaged(L, closemethod, 0); lua_setfield(L, -2, "close");
  luaQ_pushmanaged(L, stats, 0); lua_setfield(L, -2, "stats");
  luaQ_pushmanaged(L, finalize, 0); lua_setfield(L, -2, "__gc");
  lua_pushliteral(L, "luthr.chunk_pipe"); lua_setfield(L, -2, "__name");
  lua_pushboolean(L, 0); lua_setfield(L, -2, "__metatable");
  lua_lock(L);
  shared = NULL;
  if (!__atomic_compare_exchange_n(&G(L)->pipe_meta, &shared,
        hvalue(s2v(L->top.p - 1)), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    sethvalue2s(L, L->top.p - 1, shared);
  lua_unlock(L);
}

void luaP_newpipe (lua_State *L, unsigned slots) {
  Pipe *pipe;
  Udata *owner;
  Table *mt;
  TValue value;
  if (slots == 0) slots = 1;
  if (slots > LUA_PIPE_SLOTS_MAX)
    luaL_error(L, "pipe capacity exceeds 64 chunks");
  if (!lua_checkstack(L, 8)) {
    lua_lock(L);
    luaM_error(L);
  }
  metatable(L);
  mt = cast(Table *, lua_topointer(L, -1));
  pipe = cast(Pipe *, lua_newuserdatauv(L, sizeof(Pipe), slots));
  memset(pipe, 0, sizeof(*pipe));
  pipe->identity = &identity;
  pipe->capacity = slots;
  if (pthread_mutex_init(&pipe->mutex, NULL) != 0) {
    lua_lock(L); luaM_error(L);
  }
  if (pthread_cond_init(&pipe->changed, NULL) != 0) {
    luaX_checksync(pthread_mutex_destroy(&pipe->mutex));
    lua_lock(L); luaM_error(L);
  }
  pipe->ready = 1;
  owner = cast(Udata *, cast_charp(pipe) - udatamemoffset(slots));
  lua_lock(L);
  setuvalue(L, &value, owner);
  luaA_setmetatable(L, &value, mt); /* All following operations are allocation-free. */
  setobjs2s(L, L->top.p - 2, L->top.p - 1);
  L->top.p--;
  lua_unlock(L);
}
