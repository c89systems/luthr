/* Private task execution transitions. See Copyright Notice in lua.h */
#ifndef lexec_h
#define lexec_h

#include "lua.h"
#include "llimits.h"
#include "lpolicy.h"
#include <pthread.h>
#include <stdint.h>

struct lua_longjmp;
struct LuaWaiter;
struct LuaTaskRoot;

/* An attached runtime C activation owns its state across API calls. The
** record lives on the logical execution's retained C stack. */
typedef struct LuaApiRegion {
  struct LuaApiRegion *previous;
  lua_State *state;
} LuaApiRegion;

typedef struct LuaWorldScope {
  unsigned collection;
  unsigned heap;
  LuaApiRegion *api_region;  /* detached activation, restored only on attach */
} LuaWorldScope;
#define LUA_ALLOC_BINS 8
#define LUA_ALLOC_POOLS (2 * LUA_ALLOC_BINS)  /* ordinary storage and call frames */
typedef struct LuaAllocationCache {
  struct LuaAllocationCache *next;  /* VM registry, collection/quiescence */
  struct global_State *global;
  lua_State *thread;  /* unmanaged cache: borrow its coroutine gate */
  void **permission;  /* managed cache: borrow its slot's ownership word */
  void (*returned)(void *);  /* notify an idle slot after a collector borrows it */
  void *context;
  struct LuaAllocPage *pages[LUA_ALLOC_POOLS];
  unsigned char growth[LUA_ALLOC_POOLS];  /* backing-size history survives handoffs */
  uint64_t growth_epoch;  /* a collection invalidates stale capacity demand */
} LuaAllocationCache;

typedef struct LuaCallScope {
  struct LuaCallScope *previous;
  struct LuaExecution *execution;
  unsigned native_depth;
  int managed;
  LuaApiRegion api;
  LuaWorldScope world;
} LuaCallScope;

typedef struct LuaExecution {
  void *context;
  struct LuaScheduler *scheduler;  /* production routing, NULL for host drivers */
  LuaPolicy *policy;
  void (*managed_enter)(struct LuaExecution *, lua_State *);
  /* Release only: this may run under a resource bookkeeping mutex. */
  void (*managed_leave)(struct LuaExecution *);
  void (*native_enter)(struct LuaExecution *, lua_State *);
  void (*native_leave)(struct LuaExecution *, lua_State *);
  void (*native_restore)(struct LuaExecution *, lua_State *, unsigned);
  void (*poll)(struct LuaExecution *, lua_State *);
  /* Owner-local request only: no allocation, Lua entry or stack switch. */
  void (*request_handoff)(struct LuaExecution *, lua_State *);
  /* wait releases the supplied mutex and returns unlocked after notification
  ** removes its registration. wake only arranges resumption; it cannot enter
  ** Lua or switch the calling stack. */
  void (*wait)(struct LuaExecution *, pthread_mutex_t *, struct LuaWaiter *);
  void (*wake)(struct LuaExecution *, struct LuaWaiter *);
  /* Reserve/release routing to the current carrier at the outermost pin.
  ** pin returns nonzero on success. Both hooks are nonblocking: no Lua entry,
  ** allocation, migration, or nonlocal exit. Carriers remain reusable while
  ** the pinned execution parks. Missing hooks reject managed pin regions. */
  int (*pin_carrier)(struct LuaExecution *);
  void (*unpin_carrier)(struct LuaExecution *);
  struct lua_longjmp *protected;
  struct LuaTaskRoot *task;  /* atomic binding, cleared at result collection */
  unsigned managed_depth;
  unsigned native_depth;
  unsigned pin_depth;
  pthread_t pinned_carrier;
  LuaAllocationCache *allocation_cache;  /* borrowed managed slot, not a task root */
  LuaCallScope *trusted;  /* runtime-owned C frames, including nested callbacks */
  struct LuaGCFrame *gc_frame;  /* execution-local collector reentrancy */
} LuaExecution;

enum { LUA_PIN_INVALID = -1, LUA_PIN_UNAVAILABLE = -2, LUA_PIN_OVERFLOW = -3 };
typedef struct LuaCarrierPin {
  LuaExecution *execution;
  pthread_t carrier;
  int active;
} LuaCarrierPin;

typedef struct LuaOwner {
  void *token;
} LuaOwner;

typedef struct LuaWaiter {
  struct LuaWaiter *next;
  struct LuaWaiter **link;  /* predicate mutex; NULL after notification */
  struct LuaWaitQueue *queue;  /* set for FIFO waits */
  LuaExecution *execution;
} LuaWaiter;

typedef struct LuaWaitQueue {
  LuaWaiter *head;
  LuaWaiter **tail;
} LuaWaitQueue;

typedef struct LuaStopWait {
  LuaWaiter waiter;
  pthread_mutex_t *mutex;
  pthread_cond_t *changed;
  unsigned observers;  /* owning task's world mutex */
} LuaStopWait;

LUAI_FUNC LuaExecution *luaQ_current (void);
/* Private binding-author regions. Initially zeroed storage and its execution
** outlive every use; the caller serializes access to each record. A region
** belongs to both its logical execution and physical carrier. Multiple regions
** may retire in any order. A binding must close its native resources on that
** owner before unpinning, including protected error/stop cleanup, and retire
** all regions before task completion is published or a foreign callback detaches.
** Cleanup must already be installed before creating the native resource.
** Pinning does not root a Lua value or
** adapt arbitrary finalizers/callbacks. An unmanaged owner is thread-local.
** Rejection preserves the record and routing. No allocation or native join. */
LUAI_FUNC int luaQ_pincarrier (LuaCarrierPin *pin);
LUAI_FUNC int luaQ_pinowner (const LuaCarrierPin *pin);
LUAI_FUNC int luaQ_unpincarrier (LuaCarrierPin *pin);
/* Trusted runtime code only: body and continuations cannot retain foreign TLS
** or perform opaque blocking work. Zero-capture implementations have canonical
** GC-rooted identities for the VM lifetime; captured closures remain distinct.
** Ordinary native functions remain foreign by default. */
LUAI_FUNC void luaQ_pushmanaged (lua_State *L, lua_CFunction function, int n);
LUAI_FUNC void luaQ_entermanaged (lua_State *L, LuaExecution *execution);
LUAI_FUNC void luaQ_leavemanaged (LuaExecution *execution);
/* Native admission can precede stack ownership. The caller supplies either
** its bound execution or the ambient driver; L may be NULL at that boundary. */
LUAI_FUNC void luaQ_beginnative (lua_State *L, LuaExecution *execution);
LUAI_FUNC void luaQ_endnative (lua_State *L, LuaExecution *execution);
LUAI_FUNC void luaQ_restorenative (LuaExecution *execution, lua_State *L,
                                  unsigned depth);
LUAI_FUNC void luaQ_poll (lua_State *L);
LUAI_FUNC void luaQ_requesthandoff (lua_State *L);
/* Bounded CPU work in trusted C helpers. Call outside object ownership, using
** an attached C activation or this helper's API entry for state ownership.
** All borrowed Lua values must remain rooted across this migration point.
** Initialize the caller-local counter to zero. Does not charge bytecode fuel. */
enum { LUA_NATIVE_WORK_SLICE = 1024 };
LUAI_FUNC void luaQ_workpoll (lua_State *L, unsigned *work, size_t amount);
/* Charge a completed bounded chunk; the operation itself must be bounded. */
l_sinline void luaQ_workn (lua_State *L, unsigned *work, size_t amount) {
  if (l_unlikely(amount >= LUA_NATIVE_WORK_SLICE - *work))
    luaQ_workpoll(L, work, amount);
  else *work += cast_uint(amount);
}

l_sinline void luaQ_work (lua_State *L, unsigned *work) {
  luaQ_workn(L, work, 1);
}
LUAI_FUNC void luaQ_setmanagedhook (lua_State *L, lua_Hook hook);
LUAI_FUNC int luaQ_ismanagedhook (lua_State *L, lua_Hook hook);
LUAI_FUNC void luaQ_setowner (LuaOwner *owner);
LUAI_FUNC int luaQ_isowner (const LuaOwner *owner);
/* Stable for the logical execution across carrier movement; unmanaged callers
** use a permanent thread-local address. The token outlives every held scope. */
LUAI_FUNC void *luaQ_ownertoken (void);
/* All waits/observers register under the predicate mutex. Ordinary OS-thread
** waits retire registration on return, including spurious/direct POSIX signals;
** scheduler waits retire on notification. Callers recheck their predicate.
** A wake with no registrations is a no-op. */
LUAI_FUNC void luaQ_wait (lua_State *L, pthread_mutex_t *mutex,
                          pthread_cond_t *changed, LuaWaiter **waiters);
/* Owned waits only. Returns with mutex held, after retiring registration and
** all request-side borrows. A true result requires the caller to retire its
** resource lease before delivery (or return STOPPED to a quiescent driver). */
LUAI_FUNC int luaQ_waitstop (lua_State *L, pthread_mutex_t *mutex,
                            pthread_cond_t *changed, LuaWaiter **waiters);
/* The resource transfers ownership to head before waking it. */
LUAI_FUNC int luaQ_waitstopfifo (lua_State *L, pthread_mutex_t *mutex,
                                LuaWaitQueue *queue);
/* Selection under the resource mutex transfers the only right to wake.
** The scheduler keeps the selected waiter alive until wake is called. */
LUAI_FUNC void luaQ_takewaiter (LuaWaiter *waiter);
LUAI_FUNC void luaQ_wakewaiter (LuaWaiter *waiter);
/* Called with a request-side borrow; no world mutex remains held. */
LUAI_FUNC void luaQ_interrupt (LuaStopWait *wait);
LUAI_FUNC void luaQ_wake (pthread_cond_t *changed, LuaWaiter **waiters);

#endif
