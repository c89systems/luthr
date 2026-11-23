/* Private table nurseries. See Copyright Notice in lua.h. */
#define LUA_CORE
#include "lprefix.h"

#include "ldebug.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lmempage.h"
#include "lpacer.h"
#include "ltable.h"
#include "ltablestorage.h"
#include "ltask.h"

#define TASK_NURSERY_BYTES (2u * 1024 * 1024)
#define TASK_NURSERY_START (128u * 1024)

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
  if (L->task_nursery_closed || !isLua(L->ci) ||
      gethookmask(L) || L->native_depth != 0 || !gcrunning(G(L))) return 0;
  if (bytes > TASK_NURSERY_BYTES - L->task_table_bytes) {
    luaC_sealtask(L);
    return 0;
  }
  if (L->task_tables == NULL) L->task_table_limit = TASK_NURSERY_START;
  gco2t(o)->gcstate = NURSERY_TAG | NURSERY_TASK;
  luaC_allocnext(o) = L->task_tables;
  L->task_tables = o;
  L->task_table_bytes += bytes;
  luaC_taskresize(L, cast(l_mem, bytes));
  return 1;
}

/* Escape transfers the entire bounded cohort without moving any object.
** The existing root/write barriers cover an edge added by the triggering
** operation. The only previous outgoing edges outside this cohort are
** the interned strings recorded below. */
static void sealtask (lua_State *L, lua_State *thread) {
  global_State *g = G(L);
  uintptr_t flags = LUA_BLOCK_TABLE;
  uint64_t epoch;
  GCObject *o;
  lu_byte mark;
  unsigned i;
  if (thread->task_tables == NULL) return;
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
  for (i = 0; i < thread->task_string_count; i++)
    luaC_traceobject(L, obj2gco(thread->task_string_roots[i]));
  for (o = thread->task_tables; o != NULL; o = luaC_allocnext(o)) {
    LuaAllocBlock *block = cast(LuaAllocBlock *, o) - 1;
    LuaAllocPage *page = luaMP_page(block);
    setmarked(o, mark);
    luaC_settablestate(gco2t(o), 0);
    __atomic_store_n(&block->block.page, cast(uintptr_t, page) | flags,
                     __ATOMIC_RELEASE);
    luaMP_young(page, epoch + 1);
  }
  thread->task_tables = NULL;
  thread->task_string_count = 0;
  thread->task_table_bytes = 0;
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

static int tasktable (GCObject *object) {
  return object->tt == LUA_VTABLE && luaC_tasktable(gco2t(object));
}

int luaC_taskedge (lua_State *L, GCObject *owner, GCObject *value) {
  unsigned i;
  lua_assert(L->task_tables != NULL);
  if (tasktable(owner)) {
    if (tasktable(value)) return 1;
    if (value->tt == LUA_VSHRSTR) {
      TString *name = gco2ts(value);
      for (i = 0; i < L->task_string_count; i++)
        if (L->task_string_roots[i] == name) return 1;
      if (i < sizeof(L->task_string_roots) / sizeof(L->task_string_roots[0])) {
        L->task_string_roots[i] = name;
        L->task_string_count++;
        luaC_traceobject(L, value);
        return 1;
      }
    }
    luaC_sealtask(L);
  }
  else if (tasktable(value)) luaC_sealtask(L);
  return 0;
}

void luaC_taskbacking (lua_State *L, Table *table, size_t bytes) {
  if (luaC_tasktable(table)) {
    lua_assert(L->task_tables != NULL);
    if (bytes > TASK_NURSERY_BYTES - L->task_table_bytes) luaC_sealtask(L);
    else L->task_table_bytes += bytes;
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

/* Task-private headers never enter the sweep directory. Return contiguous
** runs from the same page together after disposing of their backing parts. */
l_sinline void freesettables (lua_State *L, GCObject *o, int allocation_links) {
  while (o != NULL) {
    LuaAllocBlock *last = cast(LuaAllocBlock *, o) - 1, *first = NULL;
    LuaAllocPage *page = luaMP_page(last);
    unsigned count = 0;
    l_mem bytes = 0;
    for (;;) {
      Table *table = gco2t(o);
      LuaAllocBlock *block = cast(LuaAllocBlock *, o) - 1;
      lua_assert(luaMP_header(block) == cast(uintptr_t, page));
      lua_assert(luaC_tasktable(table) && luaA_lockowner(&table->access) == NULL);
      o = allocation_links ? luaC_allocnext(o) : o->next;
      bytes += sizeof(*table) + table->inline_bytes;
      luaH_freeparts(L, table);
      count++;
      block->block.link.next = first;
      first = block;
      if (o == NULL || luaMP_page(cast(LuaAllocBlock *, o) - 1) != page) break;
    }
    luaM_freetablebatch(L, page, first, last, count, bytes);
  }
}

void luaC_endtask (lua_State *L) {
  StkId slot;
  GCObject *o;
  lua_assert(L->task_owner != NULL && L->task_tables != NULL);
  if (!gcrunning(G(L)) || L->openupval != NULL || L->native_roots != NULL) {
    luaC_sealtask(L);
    return;
  }
  for (slot = L->stack.p; slot < L->top.p; slot++) {
    if (ttistable(s2v(slot)) && luaC_tasktable(hvalue(s2v(slot)))) {
      luaC_sealtask(L);
      return;
    }
  }
  for (; slot < L->stack_last.p + EXTRA_STACK; slot++) setnilvalue2s(slot);
  o = L->task_tables;
  L->task_tables = NULL;
  L->task_string_count = 0;
  L->task_table_bytes = 0;
  L->task_nursery_closed = 1;
  pacenursery(L, 0);
  freesettables(L, o, 1);
  luaM_flushdebt(L);
}

l_sinline int nurserytable (lua_State *L, Table *table, int task) {
  return task ? luaC_tasktable(table) : luaC_nurserytable(L, table);
}

l_sinline void marknursery (lua_State *L, GCObject *object, GCObject **gray,
                             int task) {
  Table *t = gco2t(object);
  if (nurserytable(L, t, task) && !(luaC_tablestate(t) & NURSERY_LIVE)) {
    luaC_settablestate(t, luaC_tablestate(t) | NURSERY_LIVE);
    t->gclist = *gray;
    *gray = object;
  }
}

static void tracenursery (lua_State *L, GCObject *gray, int task) {
  StkId slot;
  for (slot = L->stack.p; slot < L->top.p; slot++)
    if (ttistable(s2v(slot))) marknursery(L, gcvalue(s2v(slot)), &gray, task);
  while (gray != NULL) {
    Table *t = gco2t(gray);
    unsigned i;
    gray = t->gclist;
    if (t->metatable != NULL) marknursery(L, obj2gco(t->metatable), &gray, task);
    for (i = 0; i < t->asize; i++)
      if (*getArrTag(t, i) == ctb(LUA_VTABLE))
        marknursery(L, getArrVal(t, i)->gc, &gray, task);
    for (i = 0; i < allocsizenode(t); i++) {
      Node *node = gnode(t, i);
      if (isempty(gval(node))) continue;
      if (keytt(node) == ctb(LUA_VTABLE)) marknursery(L, gckey(node), &gray, task);
      if (ttistable(gval(node))) marknursery(L, gcvalue(gval(node)), &gray, task);
    }
  }
}

static size_t sweepnursery (lua_State *L, GCObject **link, int task) {
  GCObject *dead = NULL, *o;
  size_t bytes = 0;
  StkId slot;
  while ((o = *link) != NULL) {
    if (o->tt == LUA_VTABLE && nurserytable(L, gco2t(o), task)) {
      Table *t = gco2t(o);
      if (!(luaC_tablestate(t) & NURSERY_LIVE)) {
        *link = luaC_allocnext(o);
        if (!task) {
          LuaAllocBlock *block = cast(LuaAllocBlock *, t) - 1;
          /* Detach from the sweep directory before a backing free can wait. */
          __atomic_store_n(&block->block.page, cast(uintptr_t, luaMP_page(block)),
                           __ATOMIC_RELAXED);
          L->nursery_tables--;
        }
        o->next = dead;
        dead = o;
        continue;
      }
      luaC_settablestate(t, luaC_tablestate(t) & ~cast(uint64_t, NURSERY_LIVE));
      if (task) bytes += luaH_size(t, t);
    }
    link = luaC_alloclink(o);
  }
  if (task) pacenursery(L, bytes);
  if (dead == NULL) return bytes;
  /* A later safepoint can widen top. Discard stale registers before freeing
  ** any of their referents, including frees that release admission. */
  for (slot = L->top.p; slot < L->stack_last.p + EXTRA_STACK; slot++)
    setnilvalue2s(slot);
  if (task) freesettables(L, dead, 0);
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
  if (!gcrunning(g) || L->native_roots != NULL ||
      L->native_depth != 0 || L->stack_peer != NULL || L->reclaiming ||
      !g->gcparams[LUA_GCPSTEPSIZE] || !g->gcparams[LUA_GCPSTEPMUL]) {
    L->task_table_limit = TASK_NURSERY_BYTES + 1;
    return;
  }
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
  live = sweepnursery(L, &L->task_tables, 1);
  if (L->task_tables != NULL) {
    lua_assert(L->task_live_bytes == live);
    L->task_table_bytes = live;
    L->task_table_limit = live * 2 + TASK_NURSERY_START;
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
