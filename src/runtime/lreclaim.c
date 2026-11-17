/*
** Private garbage retirement and concurrent destruction.
** See Copyright Notice in lua.h
*/
#define lreclaim_c
#define LUA_CORE
#include "lprefix.h"

#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"

static void registerreclaimer (lua_State *L, LuaWorld *world);
static void unregisterreclaimer (lua_State *L, LuaWorld *world);

static void freeupval (lua_State *L, UpVal *uv) {
  lua_assert(!upisopen(uv));  /* retirement has detached all stack links */
  if (uv->mutex_ready)
    luaX_checksync(pthread_mutex_destroy(&uv->mutex));
  luaM_free(L, uv);
}


static void freeobj (lua_State *L, GCObject *o) {
  switch (o->tt) {
    case LUA_VPROTO:
      luaF_freeproto(L, gco2p(o));
      break;
    case LUA_VUPVAL:
      freeupval(L, gco2upv(o));
      break;
    case LUA_VLCL: {
      LClosure *cl = gco2lcl(o);
      lua_assert(cl->access.depth == 0);
      luaM_freemem(L, cl, sizeLclosure(cl->nupvalues));
      break;
    }
    case LUA_VCCL: {
      CClosure *cl = gco2ccl(o);
      lua_assert(cl->access.depth == 0);
      luaM_freemem(L, cl, sizeCclosure(cl->nupvalues));
      break;
    }
    case LUA_VTABLE:
      luaH_free(L, gco2t(o));
      break;
    case LUA_VTHREAD:
      luaE_freethread(L, gco2th(o));
      break;
    case LUA_VUSERDATA: {
      Udata *u = gco2u(o);
      luaM_freemem(L, o, sizeudata(u->nuvalue, u->len));
      break;
    }
    case LUA_VSHRSTR: {
      TString *ts = gco2ts(o);
      luaM_freemem(L, ts, sizestrshr(cast_uint(ts->shrlen)));
      break;
    }
    case LUA_VLNGSTR: {
      TString *ts = gco2ts(o);
      if (ts->shrlen == LSTRMEM) {  /* external allocators stay serialized */
        luaW_heapenter(L);
        (*ts->falloc)(ts->ud, ts->contents, ts->u.lnglen + 1, 0);
        luaW_heapleave(L);
      }
      luaM_freemem(L, ts, luaS_sizelngstr(ts->u.lnglen, ts->shrlen));
      break;
    }
    default: lua_assert(0);
  }
}

/* Retire only after tracing and weak processing establish unreachability.
** Unlink shared indexes and stack bindings while participants are quiescent;
** destruction afterwards touches only privately owned storage. In particular,
** a later collection must never scan a retired coroutine's half-freed stack. */
void luaC_retireobject (lua_State *L, GCObject *o) {
  if (o->tt == LUA_VSHRSTR)
    luaS_remove(L, gco2ts(o));
  else if (o->tt == LUA_VUPVAL) {
    UpVal *uv = gco2upv(o);
    if (upisopen(uv)) {
      luaF_unlinkupval(uv);
      uv->owner = NULL;
    }
  }
  else if (o->tt == LUA_VTHREAD) {
    lua_State *thread = gco2th(o);
    lua_assert(thread != L && thread->native_depth == 0);
    luaF_closeupval(thread, thread->stack.p);
    luaW_unregister(thread);
    if (thread->reclaim_pages) L->reclaim_pages = 1;
    /* An abandoned protected collection may leave private garbage behind. */
    if (luaC_retired(thread) != NULL) {
      thread->retired_tail->next = luaC_retired(L);
      if (luaC_retired(L) == NULL) L->retired_tail = thread->retired_tail;
      __atomic_store_n(&L->retired, luaC_retired(thread), __ATOMIC_RELEASE);
      __atomic_store_n(&thread->retired, NULL, __ATOMIC_RELEASE);
      thread->retired_tail = NULL;
    }
    if (thread->reclaim_registered) {
      luaX_checksync(pthread_mutex_lock(&G(L)->world.mutex));
      lua_assert(thread->reclaim_workers == 0);
      unregisterreclaimer(thread, &G(L)->world);
      luaX_checksync(pthread_mutex_unlock(&G(L)->world.mutex));
    }
  }
  o->next = NULL;
  if (L->retired_tail != NULL) L->retired_tail->next = o;
  else __atomic_store_n(&L->retired, o, __ATOMIC_RELEASE);
  L->retired_tail = o;
  /* Publish retirement while still quiescent. Helpers must not wait for the
  ** retiring execution to leave its API/VM frame before seeing this work. */
  if (!(G(L)->gcstp & GCSTPCLS) && !L->reclaim_registered) {
    luaX_checksync(pthread_mutex_lock(&G(L)->world.mutex));
    registerreclaimer(L, &G(L)->world);
    luaX_checksync(pthread_mutex_unlock(&G(L)->world.mutex));
  }
}

/* Claim a bounded private chain while the work owner remains registered.
** The owner's worker count retains its state until every claimant returns. */
static GCObject *takework (lua_State *owner) {
  GCObject *head = luaC_retired(owner);
  GCObject *last = head;
  unsigned count = 1;
  if (head == NULL) return NULL;
  while (count++ < 64 && last->next != NULL) last = last->next;
  __atomic_store_n(&owner->retired, last->next, __ATOMIC_RELEASE);
  last->next = NULL;
  if (luaC_retired(owner) == NULL) owner->retired_tail = NULL;
  owner->reclaim_workers++;
  return head;
}

static int canpark (lua_State *L) {
  return L->world_depth == 0 && L->object_scopes == NULL &&
         luaW_scope(L).heap == 0;
}

static void pollreclaimer (lua_State *L) {
  if (canpark(L)) {
    luaX_pollrequests(L);
    luaQ_poll(L);
  }
}

static lu_mem destroywork (lua_State *L, GCObject *head) {
  lu_mem bytes = 0;
  while (head != NULL) {
    GCObject *o = head;
    lu_mem size = cast(lu_mem, luaC_objsize(o));
    head = o->next;
    bytes = size > cast(lu_mem, MAX_LMEM) - bytes
      ? cast(lu_mem, MAX_LMEM) : bytes + size;
    freeobj(L, o);
  }
  luaM_flushdebt(L);
  return bytes;
}

static int completework (lua_State *L, lua_State *owner) {
  LuaWorld *world = &G(L)->world;
  int finished;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(owner->reclaim_workers != 0);
  owner->reclaim_workers--;
  finished = luaC_retired(owner) == NULL && owner->reclaim_workers == 0;
  if (finished && owner->reclaim_registered)
    unregisterreclaimer(owner, world);
  luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  return finished;
}

static void registerreclaimer (lua_State *L, LuaWorld *world) {
  lua_State *head;
  lua_assert(!L->reclaim_registered);
  head = __atomic_load_n(&world->reclaimers, __ATOMIC_RELAXED);
  if (head == NULL && !luaC_tracing(L))
    __atomic_fetch_add(&world->work_epoch, 1, __ATOMIC_RELEASE);
  L->reclaim_next = head;
  L->reclaim_registered = 1;
  __atomic_store_n(&world->reclaimers, L, __ATOMIC_RELEASE);
}

/* Caller owns world.mutex and has no outstanding claimed chunks. */
static void unregisterreclaimer (lua_State *L, LuaWorld *world) {
  lua_State *head = __atomic_load_n(&world->reclaimers, __ATOMIC_RELAXED);
  lua_assert(luaC_retired(L) == NULL && L->reclaim_workers == 0);
  if (head == L)
    __atomic_store_n(&world->reclaimers, L->reclaim_next, __ATOMIC_RELEASE);
  else {
    lua_State *previous = head;
    while (previous != NULL && previous->reclaim_next != L)
      previous = previous->reclaim_next;
    lua_assert(previous != NULL);
    previous->reclaim_next = L->reclaim_next;
  }
  L->reclaim_next = NULL;
  L->reclaim_registered = 0;
  luaQ_wake(&world->changed, &world->waiters);
}

void luaC_reclaim (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  if (L->reclaiming) return;  /* another entry borrowed a parked stack */
  L->reclaiming = 1;
  L->native_depth++;
  if (canpark(L)) {
    for (;;) {
      GCObject *work;
      luaX_checksync(pthread_mutex_lock(&world->mutex));
      if (luaC_retired(L) != NULL && !L->reclaim_registered)
        registerreclaimer(L, world);
      work = takework(L);
      if (work == NULL) {
        if (L->reclaim_workers == 0) {
          if (L->reclaim_registered) unregisterreclaimer(L, world);
          luaX_checksync(pthread_mutex_unlock(&world->mutex));
          break;
        }
        luaW_waitcollector(L);
        luaX_checksync(pthread_mutex_unlock(&world->mutex));
        continue;
      }
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
      destroywork(L, work);
      if (completework(L, L)) luaC_finishreclaim(L);
      pollreclaimer(L);  /* no claimed work remains on this C stack */
    }
  }
  else {
    /* Emergency retries and teardown keep their existing exclusive scope. */
    for (;;) {
      GCObject *work;
      luaX_checksync(pthread_mutex_lock(&world->mutex));
      work = takework(L);
      if (work == NULL) {
        if (L->reclaim_registered && L->reclaim_workers == 0)
          unregisterreclaimer(L, world);
        luaX_checksync(pthread_mutex_unlock(&world->mutex));
        break;
      }
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
      destroywork(L, work);
      completework(L, L);
    }
  }
  luaM_flushdebt(L);
  if (L->reclaim_pages) {
    L->reclaim_pages = 0;
    if (!(G(L)->gcstp & GCSTPCLS))
      luaC_finishreclaim(L);
  }
  L->native_depth--;
  L->reclaiming = 0;
}

void luaC_assistreclaim (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  lu_mem budget = L->assist_debt;
  L->assist_debt = 0;
  if (budget < LUA_GC_ASSIST_BYTES || L->reclaiming || !canpark(L) ||
      L->assist_epoch !=
        __atomic_load_n(&world->work_epoch, __ATOMIC_ACQUIRE))
    return;
  L->reclaiming = 1;
  L->native_depth++;
  while (budget != 0) {
    lua_State *owner;
    GCObject *work = NULL;
    lu_mem done;
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    for (owner = __atomic_load_n(&world->reclaimers, __ATOMIC_RELAXED);
         owner != NULL; owner = owner->reclaim_next) {
      work = takework(owner);
      if (work != NULL) break;
    }
    if (work == NULL) {
      /* Outstanding claims are bounded private batches. No shared work is
      ** available to pay this allocation's debt; waiting could depend on a
      ** callback which needs this very mutator to make progress. */
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
      break;
    }
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    done = destroywork(L, work);
    if (completework(L, owner)) luaC_finishreclaim(L);
    budget = done >= budget ? 0 : budget - done;
    pollreclaimer(L);
  }
  L->native_depth--;
  L->reclaiming = 0;
  if (luaC_retired(L) != NULL || L->reclaim_pages)
    luaC_reclaim(L);
}
