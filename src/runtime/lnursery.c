/* Private nurseries. See Copyright Notice in lua.h. */
#define LUA_CORE
#include "lprefix.h"

#include <string.h>

#include "ldebug.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lmempage.h"
#include "lpacer.h"
#include "lstring.h"
#include "ltable.h"
#include "ltablestorage.h"
#include "ltask.h"

#define TASK_NURSERY_BYTES (2u * 1024 * 1024)
#define TASK_NURSERY_START (128u * 1024)
/* Bound the work needed to publish a single task's outgoing roots. */
#define TASK_NURSERY_ROOT_MAX 2048u
#define TASK_ROOT_LIVE cast(uintptr_t, 1u)

/* The hash stores dense indices, so root snapshots visit only actual roots.
** During a local trace the low pointer bit records which roots remain live. */
l_sinline uint16_t *rootslot (lua_State *L, uintptr_t root) {
  unsigned mask = L->task_root_capacity * 2 - 1;
  unsigned slot = cast_uint((root >> 3) ^ (root >> 17)) & mask;
  uint16_t *index = L->task_root_index;
  while (index[slot] != 0 &&
         (L->task_roots[index[slot] - 1] & ~TASK_ROOT_LIVE) != root)
    slot = (slot + 1) & mask;
  return &index[slot];
}

static void indexroots (lua_State *L) {
  unsigned i;
  memset(L->task_root_index, 0,
         L->task_root_capacity * 2 * sizeof(*L->task_root_index));
  for (i = 0; i < L->task_root_count; i++)
    *rootslot(L, L->task_roots[i]) = cast(uint16_t, i + 1);
}

static void clearroots (lua_State *L) {
  L->task_nursery_pending = 0;
  if (L->task_root_count != 0) {
    L->task_root_count = 0;
    indexroots(L);
  }
}

static int growroots (lua_State *L) {
  unsigned capacity = L->task_root_capacity * 2;
  size_t oldsize;
  uintptr_t *old, *roots;
  if (capacity > TASK_NURSERY_ROOT_MAX) return 0;
  roots = cast(uintptr_t *, luaM_tryalloc(L, luaC_taskrootsize(capacity)));
  if (roots == NULL) return 0;
  if (L->task_objects == NULL) {  /* a collection may have sealed it while allocating */
    luaM_free_(L, roots, luaC_taskrootsize(capacity));
    return 0;
  }
  old = L->task_roots;
  oldsize = luaC_taskrootbytes(L);
  memcpy(roots, old, L->task_root_count * sizeof(*roots));
  L->task_roots = roots;
  L->task_root_index = cast(uint16_t *, roots + capacity);
  L->task_root_capacity = capacity;
  indexroots(L);
  if (oldsize != 0) luaM_free_(L, old, oldsize);
  return 1;
}

void luaC_freetaskroots (lua_State *L) {
  size_t size = luaC_taskrootbytes(L);
  lua_assert(L->task_root_count == 0);
  if (size != 0) {
    uintptr_t *roots = L->task_roots;
    L->task_roots = L->task_root_inline;
    L->task_root_index = L->task_root_inline_index;
    L->task_root_capacity = LUA_TASK_INLINE_ROOTS;
    indexroots(L);
    luaM_free_(L, roots, size);
  }
}

void luaC_taskresize (lua_State *L, l_mem change) {
  size_t before = L->task_live_bytes - L->task_paced_bytes;
  size_t after;
  lua_assert(change >= 0 || cast(size_t, -change) <= L->task_live_bytes);
  L->task_live_bytes += change;
  if (L->task_paced_bytes > L->task_live_bytes)
    L->task_paced_bytes = L->task_live_bytes;
  after = L->task_live_bytes - L->task_paced_bytes;
  /* Limit the storage a growing task can add to the shared budget at once.
  ** A small local working set still collects without shared allocation debt. */
  if (after > TASK_NURSERY_START) {
    L->task_paced_bytes = L->task_live_bytes - TASK_NURSERY_START;
    after = TASK_NURSERY_START;
  }
  luaM_privatebytes(L, cast(l_mem, after) - cast(l_mem, before));
}

int luaC_taskallocate (lua_State *L, GCObject *o, size_t bytes) {
  int table = o->tt == LUA_VTABLE;
  if (L->task_nursery_closed || gethookmask(L) || !gcrunning(G(L)) ||
      (table && (!isLua(L->ci) || L->native_depth != 0))) return 0;
  if (bytes > TASK_NURSERY_BYTES - L->task_nursery_bytes) {
    luaC_sealtask(L);
    return 0;
  }
  if (L->task_objects == NULL) L->task_nursery_limit = TASK_NURSERY_START;
  if (table) gco2t(o)->gcstate = NURSERY_TAG | NURSERY_TASK;
  else gco2ts(o)->extra = LSTR_TASK;
  luaC_allocnext(o) = L->task_objects;
  L->task_objects = o;
  L->task_nursery_bytes += bytes;
  if (!table && L->task_nursery_bytes >= L->task_nursery_limit) {
    L->task_nursery_pending = 1;
    L->poll_remaining = 1;
  }
  luaC_taskresize(L, cast(l_mem, bytes));
  return 1;
}

/* Escape transfers the entire bounded cohort without moving any object.
** The existing root/write barriers cover an edge added by the triggering
** operation. Publish the outgoing roots before making the cohort black. */
static void sealtask (lua_State *L, lua_State *thread) {
  global_State *g = G(L);
  uintptr_t flags = LUA_BLOCK_TABLE;
  uint64_t epoch;
  GCObject *o, *next, *strings = NULL, *tail = NULL;
  lu_byte mark;
  unsigned i;
  if (thread->task_objects == NULL) return;
  luaC_noteyoung(g);
  luaM_privatebytes(L, -cast(l_mem, thread->task_live_bytes -
                                   thread->task_paced_bytes));
  thread->task_live_bytes = thread->task_paced_bytes = 0;
  /* Arming can rotate the stamp during the transfer. A single stamp makes
  ** the whole cohort black in this trace or visible from its roots in the
  ** next one. A black parent must never hide a child with the old stamp. */
  mark = luaC_newmark(g);
  epoch = g->sweep_plan.epoch;
  if (luaC_sweeping(L)) flags |= luaMP_birth(epoch);
  for (i = 0; i < thread->task_root_count; i++)
    luaC_traceobject(L, cast(GCObject *, thread->task_roots[i]));
  for (o = thread->task_objects; o != NULL; o = next) {
    next = luaC_allocnext(o);
    setmarked(o, mark);
    if (o->tt == LUA_VTABLE) {
      LuaAllocBlock *block = cast(LuaAllocBlock *, o) - 1;
      LuaAllocPage *page = luaMP_page(block);
      luaC_settablestate(gco2t(o), 0);
      __atomic_store_n(&block->block.page, cast(uintptr_t, page) | flags,
                       __ATOMIC_RELEASE);
      luaMP_young(page, epoch + 1);
    }
    else {
      TString *s = gco2ts(o);
      __atomic_fetch_and(&s->extra, cast_byte(~LSTR_TASK), __ATOMIC_RELEASE);
      luaC_allocnext(o) = strings;
      strings = o;
      if (tail == NULL) tail = o;
    }
  }
  if (strings != NULL) luaC_publishobjects(L, strings, tail);
  thread->task_objects = NULL;
  clearroots(thread);
  thread->task_nursery_bytes = 0;
  thread->task_nursery_closed = 1;
}

void luaC_sealtask (lua_State *L) {
  sealtask(L, L);
}

void luaC_sealtasks (lua_State *L) {
  global_State *g = G(L);
  LuaTaskRoot *task;
  if (!completestate(g)) return;
  lua_assert(luaW_iscollector(L));
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  for (task = g->tasks; task != NULL; task = task->next)
    sealtask(L, task->thread);
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
}

int luaC_taskedge (lua_State *L, GCObject *owner, GCObject *value) {
  lua_assert(L->task_objects != NULL);
  if (owner->tt == LUA_VTABLE && luaC_tasktable(gco2t(owner))) {
    uintptr_t root;
    uint16_t *slot;
    if (luaC_taskobject(value)) return 1;
    root = cast(uintptr_t, value);
    lua_assert(!(root & TASK_ROOT_LIVE));
    slot = rootslot(L, root);
    if (*slot != 0) return 1;
    if (L->task_root_count < L->task_root_capacity) {
      L->task_roots[L->task_root_count++] = root;
      *slot = cast(uint16_t, L->task_root_count);
      /* Grow or prune at the next instruction, including loops that only
      ** update tables. The barrier itself cannot park or allocate. */
      if (L->task_root_count == L->task_root_capacity - 1) {
        L->task_nursery_pending = 1;
        L->poll_remaining = 1;
      }
      luaC_traceobject(L, value);
      return 1;
    }
    luaC_sealtask(L);
  }
  else if (luaC_taskobject(value)) luaC_sealtask(L);
  return 0;
}

void luaC_taskbacking (lua_State *L, Table *table, size_t bytes) {
  if (luaC_tasktable(table)) {
    lua_assert(L->task_objects != NULL);
    if (bytes > TASK_NURSERY_BYTES - L->task_nursery_bytes) luaC_sealtask(L);
    else L->task_nursery_bytes += bytes;
  }
}

static void pacenursery (lua_State *L, size_t live) {
  global_State *g = G(L);
  size_t dead = L->task_live_bytes - live;
  /* Charge surviving storage before it can escape in bulk. Allocation since
  ** this checkpoint belongs to the local budget until the next scan. */
  luaM_privatebytes(L, -cast(l_mem, L->task_live_bytes - L->task_paced_bytes));
  L->task_live_bytes = L->task_paced_bytes = live;
  /* Private churn still drives weak references and finalizers on the shared
  ** heap. Charge a fraction once per local collection, even at task exit. */
  if (dead != 0 && gcrunning(g) && !luaC_tracing(L) && !luaC_sweeping(L))
    luaP_spend(g, cast(l_mem, dead / 16 + 1));
}

/* Task-private headers never enter shared sweep. Return contiguous
** runs from the same page together after disposing of their backing parts. */
l_sinline void freeset (lua_State *L, GCObject *o, int allocation_links) {
  while (o != NULL) {
    LuaAllocBlock *last = cast(LuaAllocBlock *, o) - 1, *first = NULL;
    LuaAllocPage *page = luaMP_page(last);
    unsigned count = 0;
    l_mem bytes = 0;
    for (;;) {
      LuaAllocBlock *block = cast(LuaAllocBlock *, o) - 1;
      GCObject *next = allocation_links ? luaC_allocnext(o) : o->next;
      lua_assert(luaMP_header(block) == cast(uintptr_t, page));
      if (o->tt == LUA_VTABLE) {
        Table *table = gco2t(o);
        lua_assert(luaC_tasktable(table) && luaA_lockowner(&table->access) == NULL);
        bytes += sizeof(*table) + table->inline_bytes;
        luaH_freeparts(L, table);
      }
      else {
        lua_assert(luaC_taskstring(o));
        bytes += sizestrreg(gco2ts(o)->u.lnglen);
      }
      o = next;
      count++;
      block->block.link.next = first;
      first = block;
      if (o == NULL || luaMP_page(cast(LuaAllocBlock *, o) - 1) != page) break;
    }
    luaM_freeobjectbatch(L, page, first, last, count, bytes);
  }
}

void luaC_endtask (lua_State *L) {
  StkId slot;
  GCObject *o;
  unsigned i;
  lua_assert(L->task_owner != NULL && L->task_objects != NULL);
  if (!gcrunning(G(L)) || L->openupval != NULL || L->native_roots != NULL) {
    luaC_sealtask(L);
    return;
  }
  for (slot = L->stack.p; slot < L->top.p; slot++) {
    if (iscollectable(s2v(slot)) && luaC_taskobject(gcvalue(s2v(slot)))) {
      luaC_sealtask(L);
      return;
    }
  }
  for (; slot < L->stack_last.p + EXTRA_STACK; slot++) setnilvalue2s(slot);
  for (i = 0; i < L->task_root_count; i++)
    luaC_traceobject(L, cast(GCObject *, L->task_roots[i]));
  o = L->task_objects;
  L->task_objects = NULL;
  clearroots(L);
  L->task_nursery_bytes = 0;
  L->task_nursery_closed = 1;
  pacenursery(L, 0);
  freeset(L, o, 1);
  luaM_flushdebt(L);
}

l_sinline int nurserytable (lua_State *L, Table *table, int task) {
  return task ? luaC_tasktable(table) : luaC_nurserytable(L, table);
}

l_sinline int marknursery (lua_State *L, GCObject *object, GCObject **gray,
                            int task) {
  if (object->tt == LUA_VTABLE) {
    Table *t = gco2t(object);
    uint64_t state;
    if (!nurserytable(L, t, task)) return 0;
    state = luaC_tablestate(t);
    if (!(state & NURSERY_LIVE)) {
      luaC_settablestate(t, state | NURSERY_LIVE);
      t->gclist = *gray;
      *gray = object;
    }
  }
  else {
    TString *s;
    lu_byte flags;
    if (!task || object->tt != LUA_VLNGSTR) return 0;
    s = gco2ts(object);
    flags = __atomic_load_n(&s->extra, __ATOMIC_RELAXED);
    if (!(flags & LSTR_TASK)) return 0;
    if (!(flags & LSTR_TASKLIVE))
      __atomic_store_n(&s->extra, flags | LSTR_TASKLIVE, __ATOMIC_RELAXED);
  }
  return 1;
}

l_sinline int nurseryedge (lu_byte tag, int task) {
  return task ? tag & BIT_ISCOLLECTABLE : tag == ctb(LUA_VTABLE);
}

l_sinline void visitnursery (lua_State *L, GCObject *object, GCObject **gray,
                              int task) {
  if (!marknursery(L, object, gray, task) && task) {
    uint16_t index = *rootslot(L, cast(uintptr_t, object));
    lua_assert(index != 0 && index <= L->task_root_count);
    if (index != 0) L->task_roots[index - 1] |= TASK_ROOT_LIVE;
  }
}

/* Compact the roots from live private tables. Outgoing roots that disappear
** still belong to any shared snapshot already in progress. */
static void prunetaskroots (lua_State *L) {
  unsigned i, count = 0;
  for (i = 0; i < L->task_root_count; i++) {
    uintptr_t root = L->task_roots[i];
    if (root & TASK_ROOT_LIVE) L->task_roots[count++] = root & ~TASK_ROOT_LIVE;
    else luaC_traceobject(L, cast(GCObject *, root));
  }
  if (count != L->task_root_count) {
    L->task_root_count = count;
    indexroots(L);
  }
}

static void tracenursery (lua_State *L, GCObject *gray, int task) {
  StkId slot;
  for (slot = L->stack.p; slot < L->top.p; slot++)
    if (nurseryedge(rawtt(s2v(slot)), task))
      marknursery(L, gcvalue(s2v(slot)), &gray, task);
  while (gray != NULL) {
    Table *t = gco2t(gray);
    unsigned i;
    gray = t->gclist;
    if (t->metatable != NULL)
      visitnursery(L, obj2gco(t->metatable), &gray, task);
    for (i = 0; i < t->asize; i++)
      if (nurseryedge(*getArrTag(t, i), task))
        visitnursery(L, getArrVal(t, i)->gc, &gray, task);
    for (i = 0; i < allocsizenode(t); i++) {
      Node *node = gnode(t, i);
      if (isempty(gval(node))) continue;
      if (nurseryedge(keytt(node), task))
        visitnursery(L, gckey(node), &gray, task);
      if (nurseryedge(rawtt(gval(node)), task))
        visitnursery(L, gcvalue(gval(node)), &gray, task);
    }
  }
  if (task) prunetaskroots(L);
}

static size_t sweepnursery (lua_State *L, GCObject **link, int task) {
  GCObject *dead = NULL, *o;
  size_t bytes = 0;
  StkId slot;
  while ((o = *link) != NULL) {
    if (o->tt == LUA_VTABLE) {
      Table *t = gco2t(o);
      if (nurserytable(L, t, task)) {
        uint64_t state = luaC_tablestate(t);
        if (!(state & NURSERY_LIVE)) goto detach;
        luaC_settablestate(t, state & ~cast(uint64_t, NURSERY_LIVE));
        if (task) bytes += luaH_size(t, t);
      }
    }
    else if (task) {
      TString *s = gco2ts(o);
      lu_byte flags = __atomic_load_n(&s->extra, __ATOMIC_RELAXED);
      lua_assert(flags & LSTR_TASK);
      if (!(flags & LSTR_TASKLIVE)) goto detach;
      __atomic_store_n(&s->extra, flags & ~LSTR_TASKLIVE, __ATOMIC_RELAXED);
      bytes += sizestrreg(s->u.lnglen);
    }
    link = luaC_alloclink(o);
    continue;
  detach:
    *link = luaC_allocnext(o);
    if (!task) {
      LuaAllocBlock *block = cast(LuaAllocBlock *, o) - 1;
      /* Detach from the sweep directory before a backing free can wait. */
      __atomic_store_n(&block->block.page, cast(uintptr_t, luaMP_page(block)),
                       __ATOMIC_RELAXED);
      L->nursery_tables--;
    }
    o->next = dead;
    dead = o;
  }
  if (task) pacenursery(L, bytes);
  if (dead == NULL) return bytes;
  /* A later safepoint can widen top. Discard stale registers before freeing
  ** any of their referents, including frees that release admission. */
  for (slot = L->top.p; slot < L->stack_last.p + EXTRA_STACK; slot++)
    setnilvalue2s(slot);
  if (task) freeset(L, dead, 0);
  else {
    do {
      o = dead;
      dead = o->next;
      luaH_free(L, gco2t(o));
    } while (dead != NULL);
  }
  luaM_flushdebt(L);
  return bytes;
}

void luaC_taskstep (lua_State *L) {
  global_State *g = G(L);
  size_t live;
  L->task_nursery_pending = 0;
  if (!gcrunning(g) || L->native_roots != NULL ||
      L->native_depth != 0 || L->stack_peer != NULL || L->reclaiming ||
      !g->gcparams[LUA_GCPSTEPSIZE] || !g->gcparams[LUA_GCPSTEPMUL]) {
    L->task_nursery_limit = TASK_NURSERY_BYTES + 1;
    return;
  }
  if (luaC_taskrootpressure(L) && L->task_nursery_bytes < L->task_nursery_limit &&
      growroots(L)) return;
  if (L->task_objects == NULL) return;
#if defined(LUAI_ASSERT)
  {
    UpVal *uv;
    /* Open private bindings still live in the scanned part of the stack.
    ** Materializing a binding uses the ordinary heap write barrier. */
    for (uv = L->openupval; uv != NULL; uv = uv->open.next)
      lua_assert(!luaF_localupval(L, uv) || uplevel(uv) < L->top.p);
  }
#endif
  tracenursery(L, NULL, 1);
  live = sweepnursery(L, &L->task_objects, 1);
  if (L->task_objects != NULL) {
    lua_assert(L->task_live_bytes == live);
    L->task_nursery_bytes = live;
    L->task_nursery_limit = live * 2 + TASK_NURSERY_START;
    if (luaC_taskrootpressure(L)) growroots(L);
  }
}

/* The admitted stack owns these nursery links. Quiescence detaches them
** before the shared collector can reuse a table's allocation prefix. */
void luaC_collectnursery (lua_State *L) {
  enum { NURSERY_MIN_TABLES = 32, NURSERY_SCAN_LIMIT = 16384,
         NURSERY_SCANS_PER_CYCLE = 16 };
  global_State *g = G(L);
  GCObject *gray = NULL, *o;
  size_t remaining, slots, unpinned = 0;
  if (L->nursery_tables < NURSERY_MIN_TABLES ||
      L->openupval != NULL || L->native_roots != NULL ||
      L->native_depth != 0 || L->stack_peer != NULL || L->reclaiming ||
      g->tobefnz != NULL || luaW_stopped(&g->world) ||
      g->gcparams[LUA_GCPSTEPSIZE] == 0 || g->gcparams[LUA_GCPSTEPMUL] == 0 ||
      __atomic_load_n(&g->pacer.nursery_scans, __ATOMIC_RELAXED) >=
        NURSERY_SCANS_PER_CYCLE)
    return;
  /* Bound both directory walks, the stack cleanup, and every possible edge
  ** visit before changing any local mark state. Large cohorts use shared GC. */
  slots = cast_sizet(L->stack_last.p - L->stack.p) + EXTRA_STACK;
  if (slots >= NURSERY_SCAN_LIMIT) return;
  remaining = NURSERY_SCAN_LIMIT - slots;
  for (o = L->new_tables; o != NULL; o = luaC_allocnext(o)) {
    if (--remaining == 0) return;
    if (o->tt == LUA_VTABLE && luaC_nurserytable(L, gco2t(o))) {
      Table *t = gco2t(o);
      unsigned nodes = allocsizenode(t);
      if (t->asize >= remaining || nodes >= remaining - t->asize) return;
      remaining -= t->asize + nodes;
      unpinned += (luaC_tablestate(t) & NURSERY_ROOT) == 0;
    }
  }
  if (unpinned < NURSERY_MIN_TABLES) return;
  /* Even if private churn repays its own debt, automatic shared tracing
  ** must still discover dead public objects, weak edges and finalizers. */
  if (__atomic_fetch_add(&g->pacer.nursery_scans, 1, __ATOMIC_RELAXED) >=
        NURSERY_SCANS_PER_CYCLE)
    return;
  for (o = L->new_tables; o != NULL; o = luaC_allocnext(o))
    if (o->tt == LUA_VTABLE && luaC_nurserytable(L, gco2t(o)) &&
        (luaC_tablestate(gco2t(o)) & NURSERY_ROOT))
      marknursery(L, o, &gray, 0);
  tracenursery(L, gray, 0);
  sweepnursery(L, &L->new_tables, 0);
}
