/* Published barrier work, independent of coroutine-directory size. See lua.h. */
#define LUA_CORE
#include "lprefix.h"
#include "lgc.h"
#include "lscheduler.h"

static GCObject **barrierlink (GCObject *o) {
  if (novariant(o->tt) == LUA_TSTRING) return &o->next;
  return luaC_graylink(o);
}

GCObject *luaC_barriernext (GCObject *o) {
  return *barrierlink(o);
}

void luaC_publishbarrier (lua_State *L, GCObject *o, int back) {
  global_State *g = G(L);
  uintptr_t token = cast(uintptr_t, luaX_token(L));
  unsigned shard = cast_uint((token >> 4) ^ (token >> 12) ^ (token >> 20)) &
                   (LUA_BARRIER_SHARDS - 1);
  LuaBarrierQueue *producer = &g->barriers[shard];
  GCObject **queue = back ? &producer->back : &producer->forward;
  GCObject **link = barrierlink(o);
  GCObject *head = __atomic_load_n(queue, __ATOMIC_RELAXED);
  lua_assert(o != obj2gco(mainthread(g)));
  do {
    *link = head;
  } while (!__atomic_compare_exchange_n(queue, &head, o, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  /* Only the publisher which changed an empty head advertises the shard.
  ** A delayed advertisement can cause an empty visit, never lost work. No
  ** scheduler, ownership wait, allocator or safepoint belongs to this path. */
  if (head == NULL)
    __atomic_fetch_or(&g->barrier_ready, (1u << shard) | LUA_BARRIER_NOTICE,
                      __ATOMIC_RELEASE);
}

void luaC_notifybarriers (global_State *g) {
  /* A safe boundary claims only the notice. Published heads remain ready
  ** until drained. A later empty-to-nonempty publication can ask again.
  ** Slot return, trace arming and world resume have their own wakeups. */
  if (__atomic_fetch_and(&g->barrier_ready, ~cast_uint(LUA_BARRIER_NOTICE),
                         __ATOMIC_ACQ_REL) & LUA_BARRIER_NOTICE)
    luaSC_notifygc(g);
}

void luaC_drainbarriers (lua_State *L) {
  global_State *g = G(L);
  unsigned ready, shard;
  if (__atomic_load_n(&g->barrier_ready, __ATOMIC_ACQUIRE) == 0) return;
  /* Clear BEFORE consuming heads: either a consumer sees a producer's link,
  ** or that producer's later directory publication remains for the next pass.
  ** The fixed queues outlive producers; no coroutine walk or borrow is needed. */
  ready = __atomic_exchange_n(&g->barrier_ready, 0, __ATOMIC_ACQ_REL) &
          ~cast_uint(LUA_BARRIER_NOTICE);
  for (shard = 0; ready != 0; shard++, ready >>= 1) {
    LuaBarrierQueue *producer;
    GCObject *object;
    if (!(ready & 1u)) continue;
    producer = &g->barriers[shard];
    object = __atomic_exchange_n(&producer->back, NULL, __ATOMIC_ACQUIRE);
    while (object != NULL) {
      GCObject *next = luaC_barriernext(object);
      lua_assert(isgray(object));
      if (luaC_tracing(L)) luaC_linktrace(g, object);
      else {
        *luaC_graylink(object) = g->grayagain;
        g->grayagain = object;
      }
      object = next;
    }
    object = __atomic_exchange_n(&producer->forward, NULL, __ATOMIC_ACQUIRE);
    while (object != NULL) {
      GCObject *next = luaC_barriernext(object);
      lua_assert(isgray(object));
      luaC_linktrace(g, object);
      object = next;
    }
  }
}
