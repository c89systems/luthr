/* Tasks, execution slots, reusable carriers and bounded work exchange. */
#define LUA_CORE
#include "lprefix.h"
#include "lscheduler.h"
#include "lapi.h"
#include "ldo.h"
#include "lmem.h"
#include "lgc.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SC_READY, SC_RESOURCE, SC_DONE };

static void wakework (LuaScheduler *s, int all);
static void wakecollectors (LuaScheduler *s);

/* Remove a selected sleeper before signalling it, so later submissions choose
** another carrier even if this one has not yet reacquired the engine mutex. */
static void wakecarrier (LuaCarrier **link) {
  LuaCarrier *carrier = *link;
  lua_assert(carrier->sleeping);
  *link = carrier->idle_next;
  carrier->idle_next = NULL;
  carrier->sleeping = 0;
  luaX_checksync(pthread_cond_signal(&carrier->changed));
}

static void lock (LuaScheduler *s) {
  luaX_checksync(pthread_mutex_lock(&s->mutex));
}
static void unlock (LuaScheduler *s) {
  luaX_checksync(pthread_mutex_unlock(&s->mutex));
}
static void notify (LuaScheduler *s) {
  luaX_checksync(pthread_cond_broadcast(&s->changed));
  if (s->stopping)
    while (s->idle != NULL) wakecarrier(&s->idle);
}
static void changed (LuaScheduler *s) {
  luaX_checksync(pthread_cond_wait(&s->changed, &s->mutex));
}

static void append (LuaScheduledTask **head, LuaScheduledTask **tail,
                    LuaScheduledTask *task) {
  lua_assert(task->next == NULL);
  if (*tail != NULL) (*tail)->next = task;
  else *head = task;
  *tail = task;
}

static LuaScheduledTask *take (LuaScheduledTask **head,
                               LuaScheduledTask **tail) {
  LuaScheduledTask *task = *head;
  if (task != NULL) {
    *head = task->next;
    if (*head == NULL) *tail = NULL;
    task->next = NULL;
  }
  return task;
}

static int eligible (LuaScheduledTask *task, LuaCarrier *carrier) {
  LuaCarrier *affinity = __atomic_load_n(&task->affinity, __ATOMIC_ACQUIRE);
  LuaScheduledTask *reserved;
  if (affinity != NULL && affinity != carrier) return 0;
  reserved = __atomic_load_n(&carrier->reserved, __ATOMIC_ACQUIRE);
  return !task->native_pending || reserved == NULL || reserved == task;
}

/* The engine and queue mutexes keep the queued task's routing fields stable.
** A pinned/native-pending task must wake an eligible carrier, not an arbitrary
** sleeper which might leave its required carrier asleep indefinitely. */
static void wakequeued (LuaScheduler *s, LuaScheduledTask *task) {
  LuaCarrier **link;
  if (__atomic_load_n(&s->reentry_waiting, __ATOMIC_ACQUIRE) != 0) return;
  for (link = &s->idle; *link != NULL; link = &(*link)->idle_next) {
    if (eligible(task, *link)) {
      wakecarrier(link);
      return;
    }
  }
}

static LuaScheduledTask *takefor (LuaExecutionSlot *slot, LuaCarrier *carrier) {
  LuaScheduledTask **link = &slot->head, *previous = NULL;
  while (*link != NULL) {
    LuaScheduledTask *task = *link;
    if (eligible(task, carrier)) {
      *link = task->next;
      if (slot->tail == task) slot->tail = previous;
      task->next = NULL;
      slot->count--;
      return task;
    }
    previous = task;
    link = &task->next;
  }
  return NULL;
}

/* Caller retains scheduler.mutex on host/wake/publication paths. Queue
** ownership is independent; successful dispatch needs no engine mutex. */
static void enqueue (LuaScheduler *s, LuaScheduledTask *task) {
  LuaExecutionSlot *slot = &s->slots[task->home];
  lua_assert(task->slot == NULL);
  luaX_checksync(pthread_mutex_lock(&slot->mutex));
  append(&slot->head, &slot->tail, task);
  slot->count++;
  __atomic_fetch_add(&s->runnable, 1, __ATOMIC_SEQ_CST);
  wakequeued(s, task);
  luaX_checksync(pthread_mutex_unlock(&slot->mutex));
  notify(s);
}

/* Exchange at most eight queued tasks with a peer, rotating the first peer.
** Never hold two local-queue mutexes together. Detached batches remain rooted
** by the engine's admitted task registry. */
static LuaScheduledTask *stealwork (LuaScheduler *s, unsigned index,
                                    LuaCarrier *carrier) {
  LuaExecutionSlot *local = &s->slots[index];
  LuaScheduledTask *task;
  unsigned i;
  for (i = 0; i < s->config.slots; i++) {
    unsigned peer_index = (carrier->steal_cursor + i) % s->config.slots;
    LuaExecutionSlot *peer = &s->slots[peer_index];
    LuaScheduledTask *head = NULL, *tail = NULL;
    unsigned count, moved = 0;
    if (peer_index == index) continue;
    luaX_checksync(pthread_mutex_lock(&peer->mutex));
    count = peer->count / 2 + peer->count % 2;
    if (count > 8) count = 8;
    while (moved < count) {
      task = takefor(peer, carrier);
      if (task == NULL) break;
      append(&head, &tail, task);
      moved++;
    }
    luaX_checksync(pthread_mutex_unlock(&peer->mutex));
    task = take(&head, &tail);
    if (task != NULL) {
      luaX_checksync(pthread_mutex_lock(&local->mutex));
      if (head != NULL) {
        if (local->tail != NULL) local->tail->next = head;
        else local->head = head;
        local->tail = tail;
        local->count += moved - 1;
      }
      luaX_checksync(pthread_mutex_unlock(&local->mutex));
      carrier->steal_cursor = (peer_index + 1) % s->config.slots;
      return task;
    }
  }
  return NULL;
}

/* Periodically prefer peer work even when the local queue stays nonempty.
** A yielding local workload must not monopolize the available carriers while
** eligible work waits on a busy peer. The counter and cursor are carrier-local.
** Like Go's periodic global-queue check, this bounds a local scheduling burst
** without adding an engine-wide transaction to ordinary dispatch. */
static LuaScheduledTask *localwork (LuaScheduler *s, unsigned index,
                                    LuaCarrier *carrier) {
  LuaExecutionSlot *local = &s->slots[index];
  LuaScheduledTask *task;
  enum { LOCAL_DISPATCH_BURST = 61 };
  if (s->config.slots > 1 &&
      ++carrier->local_dispatches == LOCAL_DISPATCH_BURST) {
    carrier->local_dispatches = 0;
    task = stealwork(s, index, carrier);
    if (task != NULL) return task;
  }
  luaX_checksync(pthread_mutex_lock(&local->mutex));
  task = takefor(local, carrier);
  luaX_checksync(pthread_mutex_unlock(&local->mutex));
  return task != NULL ? task : stealwork(s, index, carrier);
}

static void bindslot (LuaScheduledTask *task, LuaExecutionSlot *slot) {
  task->slot = slot;
  task->fiber.execution.allocation_cache = &slot->allocation;
}

static void cachereturned (void *context) {
  LuaScheduler *s = context;
  lock(s);
  notify(s);
  wakework(s, 1);
  unlock(s);
}

static int slotdemand (LuaScheduler *s) {
  return __atomic_load_n(&s->runnable, __ATOMIC_SEQ_CST) != 0 ||
         __atomic_load_n(&s->reentry_waiting, __ATOMIC_SEQ_CST) != 0;
}

static void releaseslot (LuaScheduledTask *task) {
  LuaExecutionSlot *slot = task->slot;
  if (slot == NULL) return;
  task->fiber.execution.allocation_cache = NULL;
  task->slot = NULL;
  lua_assert(__atomic_load_n(&slot->owner, __ATOMIC_RELAXED) == task);
  /* Demand publication precedes a waiter's final slot scan. Either that scan
  ** observes this return, or this observer check sees demand and signals under
  ** the scheduler mutex. A return with no demand needs no global transaction. */
  __atomic_store_n(&slot->owner, NULL, __ATOMIC_SEQ_CST);
  if (slotdemand(task->scheduler) ||
      (task->scheduler->config.slots > 1 && luaC_workerready(task->scheduler->global))) {
    lock(task->scheduler);
    notify(task->scheduler);
    wakework(task->scheduler, 0);
    unlock(task->scheduler);
  }
}

static void flushrouting (LuaScheduler *s) {
  if (__atomic_load_n(&s->routing_request, __ATOMIC_ACQUIRE) != 0 &&
      __atomic_exchange_n(&s->routing_request, 0, __ATOMIC_ACQ_REL)) {
    lock(s);
    notify(s);
    wakework(s, 1);
    unlock(s);
  }
}

/* Reservation hooks cannot acquire a bookkeeping mutex. The execution that
** releases a route arranges the scheduler notification at its next boundary. */
static int pincarrier (LuaExecution *execution) {
  LuaScheduledTask *task = execution->context;
  LuaScheduledTask *expected = NULL;
  LuaCarrier *carrier = task->carrier;
  if (task->external) return 1;  /* the provider already owns this thread */
  if (!__atomic_compare_exchange_n(&carrier->reserved, &expected, task, 0,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && expected != task) return 0;
  __atomic_store_n(&task->affinity, carrier, __ATOMIC_RELEASE);
  return 1;
}

static void unpincarrier (LuaExecution *execution) {
  LuaScheduledTask *task = execution->context;
  if (!task->external && !task->native) {
    LuaCarrier *carrier = __atomic_load_n(&task->affinity, __ATOMIC_ACQUIRE);
    lua_assert(carrier == task->carrier);
    __atomic_store_n(&task->affinity, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&carrier->reserved, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&task->scheduler->routing_request, 1, __ATOMIC_RELEASE);
  }
}

static void pause (LuaScheduledTask *task, int reason) {
  LuaExecution *execution = &task->fiber.execution;
  LuaCarrier *affinity = __atomic_load_n(&task->affinity, __ATOMIC_ACQUIRE);
  lua_assert(!task->external && execution->managed_depth == 0);
  lua_assert(execution->native_depth == 0 || affinity != NULL);
  UNUSED(affinity);
  releaseslot(task);
  task->reason = reason;
  luaCT_pause(&task->fiber);
  if (execution->native_depth == 0) luaD_rebindexecution(execution);
  else lua_assert(task->carrier == affinity);  /* opaque frames never migrate */
}

static int tryslot (LuaScheduledTask *task) {
  LuaScheduler *s = task->scheduler;
  unsigned i;
  for (i = 0; i < s->config.slots; i++) {
    void *expected = NULL;
    LuaExecutionSlot *slot = &s->slots[(task->home + i) % s->config.slots];
    if (__atomic_compare_exchange_n(&slot->owner, &expected, task, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      bindslot(task, slot);
      return 1;
    }
  }
  return 0;
}

static void managedenter (LuaExecution *execution, lua_State *L) {
  LuaScheduledTask *task = execution->context;
  LuaScheduler *s = task->scheduler;
  UNUSED(L);
  if (task->slot != NULL) return;
  if (execution->native_depth == 0) {
    pause(task, SC_READY);
    lua_assert(task->slot != NULL);
  }
  else {
    /* Reacquire local permission directly when nobody needs a handoff. Opaque
    ** native execution still releases its slot between API operations. */
    if (!slotdemand(s) && tryslot(task)) return;
    /* A synchronous reentry cannot move an opaque native activation. There
    ** are slots + native_limit carriers, so a waiting callback cannot occupy
    ** the last carrier able to dispatch queued managed work. */
    lock(s);
    __atomic_fetch_add(&s->reentry_waiting, 1, __ATOMIC_SEQ_CST);
    while (!tryslot(task)) changed(s);
    __atomic_fetch_sub(&s->reentry_waiting, 1, __ATOMIC_SEQ_CST);
    notify(s);
    wakework(s, 1);
    unlock(s);
  }
}

static void managedleave (LuaExecution *execution) {
  /* Retain a slot through owner-local host/API sequences. Parking and native
  ** withdrawal release it. Ordinary API calls don't transact on a VM permit. */
  if (execution->native_depth != 0 &&
      (execution->trusted == NULL ||
       execution->native_depth > execution->trusted->native_depth))
    releaseslot(execution->context);
  flushrouting(execution->scheduler);
}

static void nativeenter (LuaExecution *execution, lua_State *L) {
  LuaScheduledTask *task = execution->context;
  LuaScheduler *s = task->scheduler;
  LuaWorldScope depth = {0};
  int detached = 0, managed = execution->managed_depth != 0;
  if (execution->native_depth != 0) return;
  task->native_pending = 1;
  if (!eligible(task, task->carrier)) {
    lua_assert(execution->pin_depth == 0);
    if (managed) depth = luaW_detach(L);
    pause(task, SC_READY);
    /* Reattachment itself can park for collection. Keep the routing constraint
    ** through that boundary and every later native-admission wait. */
    if (managed) luaW_attach(L, depth);
  }
  luaX_checksync(pthread_mutex_lock(&s->native_mutex));
  if (s->native == s->config.native_limit) {
    luaX_checksync(pthread_mutex_unlock(&s->native_mutex));
    if (managed) {
      depth = luaW_detach(L);  /* no foreign activation exists yet */
      detached = 1;
    }
    luaX_checksync(pthread_mutex_lock(&s->native_mutex));
  }
  while (s->native == s->config.native_limit) {
    if (luaQ_waitstop(L, &s->native_mutex, &s->native_changed,
                      &s->native_waiters)) {
      luaX_checksync(pthread_mutex_unlock(&s->native_mutex));
      if (managed) luaW_attach(L, depth);
      else lua_lock(L);
      luaTK_checkstop(L);  /* the admission wait has retired every borrow */
      abort();
    }
  }
  task->native = 1;
  if (!pincarrier(execution)) abort();
  task->native_pending = 0;
  s->native++;
  if (s->native > s->native_peak) s->native_peak = s->native;
  luaX_checksync(pthread_mutex_unlock(&s->native_mutex));
  if (detached) luaW_attach(L, depth);
  if (!managed) releaseslot(task); /* trusted hook/adapter now enters opaque C */
}

static void nativeleave (LuaExecution *execution, lua_State *L) {
  LuaScheduledTask *task = execution->context;
  LuaScheduler *s = task->scheduler;
  UNUSED(L);
  if (execution->native_depth != 0 || !task->native) return;
  luaX_checksync(pthread_mutex_lock(&s->native_mutex));
  task->native = 0;
  lua_assert(s->native != 0);
  s->native--;
  luaQ_wake(&s->native_changed, &s->native_waiters);
  luaX_checksync(pthread_mutex_unlock(&s->native_mutex));
  if (execution->pin_depth == 0) unpincarrier(execution);
  flushrouting(s);
}

static void nativerestore (LuaExecution *execution, lua_State *L, unsigned old) {
  LuaScheduledTask *task = execution->context;
  UNUSED(old);
  if (execution->native_depth == 0) task->native_pending = 0;
  nativeleave(execution, L);
}

static int haswork (LuaScheduler *s, LuaCarrier *carrier) {
  unsigned i, j;
  for (i = 0; i < s->config.slots; i++) {
    LuaExecutionSlot *slot = &s->slots[i];
    LuaScheduledTask *task;
    int found = 0;
    luaX_checksync(pthread_mutex_lock(&slot->mutex));
    for (task = slot->head; task != NULL && !found; task = task->next) {
      if (carrier != NULL) found = eligible(task, carrier);
      else for (j = 0; j < s->config.carriers; j++)
        if (eligible(task, &s->carriers[j])) { found = 1; break; }
    }
    luaX_checksync(pthread_mutex_unlock(&slot->mutex));
    if (found) return 1;
  }
  return 0;
}

/* Capacity or routing changed without a newly queued task. Normal release
** wakes one eligible sleeper; unpin/reentry can enable several routes. */
static void wakework (LuaScheduler *s, int all) {
  LuaCarrier **link;
  if (__atomic_load_n(&s->runnable, __ATOMIC_ACQUIRE) == 0 ||
      __atomic_load_n(&s->reentry_waiting, __ATOMIC_ACQUIRE) != 0) {
    wakecollectors(s);
    return;
  }
  for (link = &s->idle; *link != NULL;) {
    if (haswork(s, *link)) {
      wakecarrier(link);
      if (!all) break;
    }
    else link = &(*link)->idle_next;
  }
  wakecollectors(s);
}

static void wakecollectors (LuaScheduler *s) {
  unsigned i, available = 0;
  if (s->config.slots == 1 || s->idle == NULL || s->stopping ||
      !luaC_workerready(s->global) ||
      __atomic_load_n(&s->global->world.stopped, __ATOMIC_SEQ_CST) ||
      __atomic_load_n(&s->reentry_waiting, __ATOMIC_ACQUIRE) != 0) return;
  for (i = 0; i < s->config.slots; i++)
    available += __atomic_load_n(&s->slots[i].owner, __ATOMIC_ACQUIRE) == NULL;
  if (available > s->config.slots - 1 - s->gc_active)
    available = s->config.slots - 1 - s->gc_active;
  while (available-- != 0 && s->idle != NULL) wakecarrier(&s->idle);
}

void luaSC_notifygc (global_State *g) {
  LuaScheduler *s = __atomic_load_n(&g->scheduler, __ATOMIC_ACQUIRE);
  if (s == NULL || !__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE) ||
      s->config.slots == 1 || !luaC_workerready(g) ||
      __atomic_load_n(&g->world.stopped, __ATOMIC_SEQ_CST)) return;
  lock(s);
  wakecollectors(s);
  unlock(s);
}

static void requesthandoff (LuaExecution *execution, lua_State *L) {
  LuaScheduledTask *task = execution->context;
  LuaScheduler *s = task->scheduler;
  unsigned i;
  if (task->slot != NULL) {
    /* A free slot can run the recipient without preempting useful work.
    ** Only shorten this owner's quantum when every managed slot is busy. */
    for (i = 0; i < s->config.slots; i++)
      if (__atomic_load_n(&s->slots[i].owner, __ATOMIC_RELAXED) == NULL)
        return;
    task->polls = 1;
    L->poll_remaining = 1;  /* defer until initialized values are published */
  }
}

static void poll (LuaExecution *execution, lua_State *L) {
  LuaScheduledTask *task = execution->context;
  LuaScheduler *s = task->scheduler;
  LuaWorldScope depth;
  unsigned publishing;
  flushrouting(s);
  if (--task->polls != 0) return;
  task->polls = s->config.quantum_polls;
  /* Read publication before the runnable predicate: observing its completion
  ** also publishes the recipient's queue entry to the following load. */
  publishing = __atomic_load_n(&s->pending_ready, __ATOMIC_ACQUIRE);
  if (__atomic_load_n(&s->runnable, __ATOMIC_ACQUIRE) == 0 &&
      __atomic_load_n(&s->reentry_waiting, __ATOMIC_ACQUIRE) == 0 &&
      __atomic_load_n(&s->external_yields, __ATOMIC_ACQUIRE) == 0) {
    /* A notified waiter cannot enter the runnable queue until its outgoing
    ** context is saved. Keep the handoff prompt across that publication gap. */
    if (publishing != 0)
      task->polls = 1;
    return;
  }
  depth = luaW_detach(L);
  releaseslot(task);  /* a requested handoff retires trusted helper capacity too */
  task->handoffs++;
  if (task->external) {
    unsigned epoch;
    lock(s);
    epoch = s->handoff_epoch;
    __atomic_fetch_add(&s->external_yields, 1, __ATOMIC_RELEASE);
    notify(s);
    wakework(s, 1);
    while (epoch == s->handoff_epoch &&
           __atomic_load_n(&s->runnable, __ATOMIC_ACQUIRE) != 0 &&
           haswork(s, NULL)) changed(s);
    __atomic_fetch_sub(&s->external_yields, 1, __ATOMIC_RELEASE);
    unlock(s);
  }
  else pause(task, SC_READY);
  luaW_attach(L, depth);
}

static void resourcewait (LuaExecution *execution, pthread_mutex_t *mutex,
                          LuaWaiter *waiter) {
  LuaScheduledTask *task = execution->context;
  LuaScheduler *s = task->scheduler;
  releaseslot(task);
  lock(s);
  lua_assert(task->waiter == NULL);
  task->waiter = waiter;
  task->notified = task->published = 0;
  task->parks++;
  /* Registration is visible before the predicate is released. A notifier may
  ** run before context publication; only the outgoing carrier can then queue
  ** the task, after saving its complete context. */
  luaX_checksync(pthread_mutex_unlock(mutex));
  if (task->external) {
    while (!task->notified) changed(s);
  }
  else {
    unlock(s);
    pause(task, SC_RESOURCE);
    lock(s);
    lua_assert(task->notified);
  }
  task->waiter = NULL;
  unlock(s);
}

static void resourcewake (LuaExecution *execution, LuaWaiter *waiter) {
  (void)waiter;  /* also checked by the debug ownership assertion below */
  LuaScheduledTask *task = execution->context;
  LuaScheduler *s = task->scheduler;
  lock(s);
  lua_assert(task->waiter == waiter && !task->notified);
  task->notified = 1;
  if (!task->external) {
    if (task->published) {
      task->published = 0;
      enqueue(s, task);
    }
    else {
      task->early_wakes++;
      __atomic_fetch_add(&s->pending_ready, 1, __ATOMIC_RELEASE);
    }
  }
  notify(s);
  unlock(s);
}

static void taskentry (void *argument) {
  LuaScheduledTask *task = argument;
  int status, results, nargs = task->nargs;
  do {
    status = luaTK_resume(&task->root, nargs, &results);
    nargs = 0;
    if (status == LUA_YIELD) {
      /* Scheduler tasks treat a Lua yield as a voluntary scheduling boundary.
      ** Yielded values are discarded; final outcomes remain rooted. */
      lua_settop(task->root.thread, 0);
      pause(task, SC_READY);
    }
  } while (status == LUA_YIELD);
  if (luaTK_finish(&task->root, &status, &results) != LUA_OK) abort();
  if (task->policy != NULL) __atomic_store_n(&task->policy->active, 0, __ATOMIC_RELEASE);
  lua_assert(task->fiber.execution.pin_depth == 0 && !task->native &&
             task->fiber.execution.trusted == NULL && task->affinity == NULL);
  pause(task, SC_DONE);
  abort();
}

static LuaScheduledTask *dispatch (LuaCarrier *carrier, int locked) {
  LuaScheduler *s = carrier->scheduler;
  unsigned i;
  if (__atomic_load_n(&s->runnable, __ATOMIC_ACQUIRE) == 0) return NULL;
  if (__atomic_load_n(&s->reentry_waiting, __ATOMIC_ACQUIRE) != 0) return NULL;
  /* An ineligible carrier sleeps without repeatedly reserving/releasing a
  ** slot and waking its peers solely because pinned work exists elsewhere. */
  if (!haswork(s, carrier)) return NULL;
  for (i = 0; i < s->config.slots; i++) {
    unsigned index = (carrier->cursor + i) % s->config.slots;
    LuaExecutionSlot *slot = &s->slots[index];
    void *expected = NULL;
    LuaScheduledTask *task;
    /* The carrier address is a temporary reservation, never dereferenced as
    ** a task. No managed operation starts until bindslot replaces it. */
    if (!__atomic_compare_exchange_n(&slot->owner, &expected,
          cast(LuaScheduledTask *, carrier), 0,
          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
    task = localwork(s, index, carrier);
    if (task == NULL) {
      __atomic_store_n(&slot->owner, NULL, __ATOMIC_RELEASE);
      if (!locked) lock(s);
      notify(s);
      wakework(s, 0);
      if (!locked) unlock(s);
      continue;
    }
    __atomic_fetch_sub(&s->runnable, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&slot->owner, task, __ATOMIC_RELEASE);
    bindslot(task, slot);
    task->home = index;
    carrier->cursor = (index + 1) % s->config.slots;
    return task;
  }
  return NULL;
}

/* Only an otherwise idle carrier borrows managed capacity. These payload
** batches cannot allocate, park, invoke foreign code or run finalizers. The
** normal driver retains phase completion and destruction, including its
** native callback and full-collection completion contracts. */
static LuaExecutionSlot *dispatchcollector (LuaCarrier *carrier) {
  LuaScheduler *s = carrier->scheduler;
  unsigned i;
  if (s->gc_active == s->config.slots - 1 || !luaC_workerready(s->global) ||
      __atomic_load_n(&s->global->world.stopped, __ATOMIC_SEQ_CST) ||
      __atomic_load_n(&s->reentry_waiting, __ATOMIC_ACQUIRE) != 0) return NULL;
  for (i = 0; i < s->config.slots; i++) {
    LuaExecutionSlot *slot = &s->slots[(carrier->cursor + i) % s->config.slots];
    void *expected = NULL;
    if (__atomic_compare_exchange_n(&slot->owner, &expected,
          &slot->collector_execution, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      s->gc_active++;
      return slot;
    }
  }
  return NULL;
}

static void *carrierentry (void *argument) {
  LuaCarrier *carrier = argument;
  LuaScheduler *s = carrier->scheduler;
  for (;;) {
    LuaScheduledTask *task = dispatch(carrier, 0);
    LuaExecutionSlot *collector = NULL;
    if (task == NULL) {
      lock(s);
      while (!s->stopping && (task = dispatch(carrier, 1)) == NULL &&
             (collector = dispatchcollector(carrier)) == NULL) {
        lua_assert(!carrier->sleeping && carrier->idle_next == NULL);
        carrier->idle_next = s->idle;
        s->idle = carrier;
        carrier->sleeping = 1;
        do {
          luaX_checksync(pthread_cond_wait(&carrier->changed, &s->mutex));
        } while (carrier->sleeping);
      }
      if (s->stopping) { unlock(s); return NULL; }
      unlock(s);
    }
    if (collector != NULL) {
      luaSC_collectidle(carrier, collector);
      __atomic_store_n(&collector->owner, NULL, __ATOMIC_SEQ_CST);
      lock(s);
      lua_assert(s->gc_active != 0);
      s->gc_active--;
      notify(s);
      wakework(s, 1);
      unlock(s);
      continue;
    }
    if (task->carrier != NULL && task->carrier != carrier) task->migrations++;
    task->carrier = carrier;
    task->dispatches++;
    luaCT_resume(&task->fiber, &carrier->context);
    lua_assert(luaQ_current() == NULL);
    lock(s);
    s->handoff_epoch++;
    if (task->reason == SC_DONE) {
      task->complete = 1;
      lua_assert(s->outstanding != 0);
      s->outstanding--;
      if (s->completed != NULL) s->completed(s->completion_context);
    }
    else if (task->reason == SC_RESOURCE) {
      if (task->notified) {
        enqueue(s, task);
        __atomic_fetch_sub(&s->pending_ready, 1, __ATOMIC_RELEASE);
      }
      else task->published = 1;
    }
    else enqueue(s, task);
    notify(s);
    unlock(s);
  }
}

static void initcollectors (lua_State *L, void *argument) {
  LuaScheduler *s = argument;
  unsigned i;
  if (s->config.slots == 1) return;  /* payload helpers need a concurrent driver */
  luaD_checkstack(L, 1);
  for (i = 0; i < s->config.slots; i++) {
    lua_State *worker = luaE_newthread(L, 1);
    s->slots[i].collector = worker;
    L->top.p--;
  }
}

static void freecollectors (LuaScheduler *s) {
  unsigned i;
  for (i = 0; i < s->config.slots; i++) {
    lua_State *worker = s->slots[i].collector;
    if (worker != NULL) {
      lua_assert(worker->native_depth == 1);
      worker->native_depth--;
      s->slots[i].collector = NULL;
    }
  }
}

static void freeengine (lua_State *L, LuaScheduler *s) {
  unsigned i;
  lua_lock(L);
  luaW_stop(L);
  __atomic_store_n(&s->ready, 0, __ATOMIC_RELEASE);
  freecollectors(s);
  for (i = 0; i < s->config.slots; i++) {
    LuaAllocationCache **link = &G(L)->slot_caches;
    luaM_releasecache(L, &s->slots[i].allocation);
    while (*link != &s->slots[i].allocation) link = &(*link)->next;
    *link = (*link)->next;
    luaX_checksync(pthread_mutex_destroy(&s->slots[i].mutex));
  }
  luaM_free_(L, s->slots, sizeof(*s->slots) * s->config.slots);
  for (i = 0; i < s->config.carriers; i++)
    luaX_checksync(pthread_cond_destroy(&s->carriers[i].changed));
  luaM_free_(L, s->carriers, sizeof(*s->carriers) * s->config.carriers);
  s->slots = NULL;
  s->carriers = NULL;
  __atomic_store_n(&G(L)->scheduler, NULL, __ATOMIC_RELEASE);
  luaW_start(L);
  lua_unlock(L);
  luaX_checksync(pthread_cond_destroy(&s->changed));
  luaX_checksync(pthread_mutex_destroy(&s->mutex));
  luaX_checksync(pthread_cond_destroy(&s->native_changed));
  luaX_checksync(pthread_mutex_destroy(&s->native_mutex));
}

int luaSC_init (lua_State *L, LuaScheduler *s, const LuaSchedulerConfig *config) {
  unsigned i, initialized = 0, initialized_carriers = 0;
  int status;
  pthread_condattr_t attributes;
  if (s->ready || luaQ_current() != NULL || config == NULL ||
      config->slots == 0 || config->native_limit == 0 ||
      config->quantum_polls == 0 || config->carriers < config->slots ||
      config->native_limit > config->carriers - config->slots ||
      luaM_testsize(config->slots, sizeof(*s->slots)) ||
      luaM_testsize(config->carriers, sizeof(*s->carriers))) return EINVAL;
  memset(s, 0, sizeof(*s));
  s->config = *config;
  s->global = G(L);
  status = pthread_mutex_init(&s->mutex, NULL);
  if (status != 0) return status;
  status = pthread_condattr_init(&attributes);
  if (status == 0) {
    status = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (status == 0) status = pthread_cond_init(&s->changed, &attributes);
    luaX_checksync(pthread_condattr_destroy(&attributes));
  }
  if (status != 0) {
    luaX_checksync(pthread_mutex_destroy(&s->mutex));
    return status;
  }
  status = pthread_mutex_init(&s->native_mutex, NULL);
  if (status == 0) {
    status = pthread_cond_init(&s->native_changed, NULL);
    if (status != 0) luaX_checksync(pthread_mutex_destroy(&s->native_mutex));
  }
  if (status != 0) {
    luaX_checksync(pthread_cond_destroy(&s->changed));
    luaX_checksync(pthread_mutex_destroy(&s->mutex));
    return status;
  }
  lua_lock(L);
  {
    LuaScheduler *expected = NULL;
    if (!__atomic_compare_exchange_n(&G(L)->scheduler, &expected, s, 0,
          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      lua_unlock(L);
      luaX_checksync(pthread_cond_destroy(&s->changed));
      luaX_checksync(pthread_mutex_destroy(&s->mutex));
      luaX_checksync(pthread_cond_destroy(&s->native_changed));
      luaX_checksync(pthread_mutex_destroy(&s->native_mutex));
      return EBUSY;
    }
  }
  s->slots = luaM_realloc_(L, NULL, 0, config->slots * sizeof(*s->slots));
  s->carriers = luaM_realloc_(L, NULL, 0, config->carriers * sizeof(*s->carriers));
  if (s->slots == NULL || s->carriers == NULL) status = ENOMEM;
  else {
    memset(s->slots, 0, config->slots * sizeof(*s->slots));
    memset(s->carriers, 0, config->carriers * sizeof(*s->carriers));
    for (i = 0; i < config->slots; i++) {
      status = pthread_mutex_init(&s->slots[i].mutex, NULL);
      if (status != 0) break;
      initialized++;
    }
    if (status == 0) {
      for (i = 0; i < config->carriers; i++) {
        status = pthread_cond_init(&s->carriers[i].changed, NULL);
        if (status != 0) break;
        initialized_carriers++;
      }
    }
  }
  if (status == 0) {
    ptrdiff_t top = savestack(L, L->top.p);
    if (luaD_rawrunprotected(L, initcollectors, s) != LUA_OK) status = ENOMEM;
    L->top.p = restorestack(L, top);
  }
  if (status != 0) {
    if (initialized != 0) freecollectors(s);
    while (initialized_carriers != 0)
      luaX_checksync(pthread_cond_destroy(&s->carriers[--initialized_carriers].changed));
    while (initialized != 0)
      luaX_checksync(pthread_mutex_destroy(&s->slots[--initialized].mutex));
    luaM_free_(L, s->slots, s->slots ? config->slots * sizeof(*s->slots) : 0);
    luaM_free_(L, s->carriers,
               s->carriers ? config->carriers * sizeof(*s->carriers) : 0);
    s->slots = NULL;
    s->carriers = NULL;
    __atomic_store_n(&G(L)->scheduler, NULL, __ATOMIC_RELEASE);
    lua_unlock(L);
    luaX_checksync(pthread_cond_destroy(&s->changed));
    luaX_checksync(pthread_mutex_destroy(&s->mutex));
    luaX_checksync(pthread_cond_destroy(&s->native_changed));
    luaX_checksync(pthread_mutex_destroy(&s->native_mutex));
    return status;
  }
  luaX_checksync(pthread_mutex_lock(&G(L)->world.mutex));
  for (i = 0; i < config->slots; i++) {
    s->slots[i].allocation.global = G(L);
    s->slots[i].allocation.permission = &s->slots[i].owner;
    s->slots[i].allocation.returned = cachereturned;
    s->slots[i].allocation.context = s;
    s->slots[i].allocation.next = G(L)->slot_caches;
    G(L)->slot_caches = &s->slots[i].allocation;
  }
  luaX_checksync(pthread_mutex_unlock(&G(L)->world.mutex));
  lua_unlock(L);
  for (i = 0; i < config->carriers; i++) {
    LuaCarrier *carrier = &s->carriers[i];
    carrier->scheduler = s;
    carrier->cursor = i % config->slots;
    status = pthread_create(&carrier->thread, NULL, carrierentry, carrier);
    if (status != 0) break;
    s->created++;
  }
  if (status != 0) {
    lock(s);
    s->stopping = 1;
    notify(s);
    unlock(s);
    while (s->created != 0)
      luaX_checksync(pthread_join(s->carriers[--s->created].thread, NULL));
    freeengine(L, s);
    return status;
  }
  __atomic_store_n(&s->ready, 1, __ATOMIC_RELEASE);
  luaSC_notifygc(G(L));
  return 0;
}

static void configureexecution (LuaScheduler *s, LuaScheduledTask *task) {
  LuaExecution *execution = &task->fiber.execution;
  execution->context = task;
  execution->scheduler = s;
  execution->policy = task->policy;
  execution->managed_enter = managedenter;
  execution->managed_leave = managedleave;
  execution->native_enter = nativeenter;
  execution->native_leave = nativeleave;
  execution->native_restore = nativerestore;
  execution->poll = poll;
  execution->request_handoff = requesthandoff;
  execution->wait = resourcewait;
  execution->wake = resourcewake;
  execution->pin_carrier = pincarrier;
  execution->unpin_carrier = unpincarrier;
  task->polls = s->config.quantum_polls;
}

int luaSC_submit (LuaScheduler *s, LuaScheduledTask *task, lua_State *L,
                   int nargs) {
  LuaExecution *execution;
  int status;
  if (!s->ready || task->scheduler != NULL || s->global != G(L) ||
      luaQ_current() != NULL || nargs < 0) return EINVAL;
  status = luaCT_create(&task->fiber, taskentry, task);
  if (status != 0) return status;
  configureexecution(s, task);
  execution = &task->fiber.execution;
  lock(s);
  if (s->closed) status = ESHUTDOWN;
  else {
    /* No worker can see the record until admission and queue publication
    ** complete. Admission may wait for GC; don't retain the engine mutex. */
    unlock(s);
    status = luaTK_admit(L, &task->root, execution);
    lock(s);
    if (status == LUA_OK) {
      task->scheduler = s;
      task->nargs = nargs;
      task->polls = s->config.quantum_polls;
      task->home = s->next_home++ % s->config.slots;
      task->registered_next = s->tasks;
      task->registered_link = &s->tasks;
      if (s->tasks != NULL) s->tasks->registered_link = &task->registered_next;
      s->tasks = task;
      s->submitted++;
      s->outstanding++;
      enqueue(s, task);
    }
  }
  unlock(s);
  if (status != LUA_OK) luaCT_destroy(&task->fiber);
  return status;
}

void luaSC_wait (LuaScheduledTask *task) {
  LuaScheduler *s = task->scheduler;
  lua_assert(s != NULL && luaQ_current() == NULL);
  lock(s);
  while (!task->complete) changed(s);
  unlock(s);
}

int luaSC_collect (lua_State *L, LuaScheduledTask *task,
                    int *status, int *results) {
  LuaScheduler *s = task->scheduler;
  int result;
  if (s == NULL || luaQ_current() != NULL) return LUA_TK_INVALID;
  lock(s);
  result = task->complete ? LUA_OK : LUA_TK_PENDING;
  unlock(s);
  if (result != LUA_OK) return result;
  result = luaTK_collect(L, &task->root, status, results);
  if (result == LUA_OK) {
    lock(s);
    *task->registered_link = task->registered_next;
    if (task->registered_next != NULL)
      task->registered_next->registered_link = task->registered_link;
    task->registered_link = NULL;
    unlock(s);
    luaCT_destroy(&task->fiber);
  }
  return result;
}

int luaSC_shutdown (LuaScheduler *s) {
  int status;
  if (!s->ready) return EINVAL;
  lock(s);
  s->closed = 1;
  unlock(s);
  status = luaTK_shutdown(s->global);
  lock(s);
  if (s->callbacks_active != 0) status = LUA_TK_PENDING;
  unlock(s);
  return status;
}

typedef struct ExternalInvocation {
  LuaScheduledTask task;
  void (*invoke)(lua_State *, void *);
  void *context;
} ExternalInvocation;

static void externalinvoke (lua_State *L, void *context) {
  ExternalInvocation *call = context;
  lua_lock(L);
  luaTK_begincallback(L, &call->task.root, &call->task.fiber.execution);
  luaTK_checkstop(L);
  lua_unlock(L);
  call->invoke(L, call->context);
}

static void callbackoutcome (LuaCallbackOutcome *out, int status,
                              LuaExecution *execution) {
  if (out != NULL && status >= 0) {
    LuaTaskRoot *root = execution != NULL
      ? __atomic_load_n(&execution->task, __ATOMIC_ACQUIRE) : NULL;
    out->lua_status = status;
    out->exhausted = execution != NULL && execution->policy != NULL &&
      __atomic_load_n(&execution->policy->exhausted, __ATOMIC_ACQUIRE);
    out->stopped = root != NULL &&
      __atomic_load_n(&root->stop_delivered, __ATOMIC_ACQUIRE);
  }
}

int luaSC_callback (LuaScheduler *s, LuaCallback *callback,
                    void (*invoke)(lua_State *, void *), void *context,
                    char *error, size_t error_size, LuaCallbackOutcome *out) {
  LuaExecution *current = luaQ_current();
  ExternalInvocation call = {0};
  int status;
  if (!s->ready || callback == NULL || callback->thread == NULL ||
      G(callback->thread) != s->global || invoke == NULL ||
      (error_size != 0 && error == NULL)) return LUA_CB_INVALID;
  if (error_size != 0) error[0] = '\0';
  if (current != NULL && current->scheduler != s) return LUA_CB_INVALID;
  lock(s);
  if (s->closed) status = LUA_CB_REVOKED;
  else if (current == NULL && s->callbacks_active == s->config.callback_limit)
    status = LUA_CB_CAPACITY;
  else {
    status = LUA_OK;
    if (current == NULL) {
      s->callbacks_active++;
      if (s->callbacks_active > s->callbacks_peak)
        s->callbacks_peak = s->callbacks_active;
    }
  }
  unlock(s);
  if (status != LUA_OK) return status;
  if (current != NULL) {
    status = luaCB_call(callback, NULL, invoke, context, error, error_size);
    callbackoutcome(out, status, current);
    return status;
  }
  configureexecution(s, &call.task);
  call.task.scheduler = s;
  call.task.external = 1;
  if (s->global->policy_factory != NULL) {
    call.task.policy = s->global->policy_factory(mainthread(s->global), callback->policy_settings,
                                                s->global->policy_context);
    if (call.task.policy == NULL) {
      lock(s);
      s->callbacks_active--;
      if (s->completed != NULL) s->completed(s->completion_context);
      notify(s);
      unlock(s);
      callbackoutcome(out, LUA_ERRMEM, NULL);
      return LUA_ERRMEM;
    }
    call.task.fiber.execution.policy = call.task.policy;
  }
  call.invoke = invoke;
  call.context = context;
  status = luaCB_call(callback, &call.task.fiber.execution,
                     externalinvoke, &call, error, error_size);
  callbackoutcome(out, status, &call.task.fiber.execution);
  luaTK_endcallback(&call.task.root);
  if (call.task.policy != NULL) {
    lua_State *L = mainthread(s->global);
    __atomic_store_n(&call.task.policy->active, 0, __ATOMIC_RELEASE);
    lua_lock(L);
    luaCP_release(L, call.task.policy);
    lua_unlock(L);
  }
  lua_assert(call.task.slot == NULL && call.task.waiter == NULL);
  lock(s);
  lua_assert(s->callbacks_active != 0);
  s->callbacks_active--;
  if (s->completed != NULL) s->completed(s->completion_context);
  notify(s);
  unlock(s);
  return status;
}

int luaSC_destroy (lua_State *L, LuaScheduler *s) {
  unsigned i;
  if (!s->ready || G(L) != s->global || luaQ_current() != NULL) return EINVAL;
  lock(s);
  if (s->tasks != NULL || s->outstanding != 0 || s->callbacks_active != 0) {
    unlock(s);
    return EBUSY;
  }
  s->closed = s->stopping = 1;
  notify(s);
  unlock(s);
  for (i = 0; i < s->created; i++)
    luaX_checksync(pthread_join(s->carriers[i].thread, NULL));
  freeengine(L, s);
  return 0;
}
