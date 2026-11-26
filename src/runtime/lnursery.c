/* Private nurseries. See Copyright Notice in lua.h. */
#define LUA_CORE
#include "lprefix.h"

#include <string.h>

#include "laccess.h"
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
#define TASK_MATURE_BYTES (192u * 1024 * 1024)
/* Only page-backed tables can publish their headers in bulk. */
#define TASK_UNPAGED_BYTES (48u * 1024 * 1024)
#define TASK_YOUNG_BYTES (4 * TASK_NURSERY_START)
#define TASK_MATURE_START (TASK_NURSERY_BYTES - TASK_YOUNG_BYTES)
/* Outgoing roots use 16-bit indices and a bounded snapshot directory. */
#define TASK_NURSERY_ROOT_MAX 32768u
#define TASK_MARK_BUDGET 8192u

typedef enum NurseryScan {
  NURSERY_CONSTRUCTION, NURSERY_TASK_YOUNG, NURSERY_TASK_FULL, NURSERY_TASK_ROOTS
} NurseryScan;

static void visitnursery (lua_State *L, GCObject *object, GCObject **gray,
                          NurseryScan scan);

/* The hash stores dense indices, so root snapshots visit only actual roots.
** During a local trace the low pointer bit records which roots remain live. */
l_sinline uint16_t *rootslot (lua_State *L, uintptr_t root) {
  unsigned mask = L->task_root_capacity * 2 - 1;
  unsigned slot = cast_uint((root >> 3) ^ (root >> 17)) & mask;
  uint16_t *index = L->task_root_index;
  while (index[slot] != 0 &&
         (L->task_roots[index[slot] - 1] & ~LUA_TASK_ROOT_FLAGS) != root)
    slot = (slot + 1) & mask;
  return &index[slot];
}

static void indexroots (lua_State *L) {
  unsigned i;
  memset(L->task_root_index, 0,
         L->task_root_capacity * 2 * sizeof(*L->task_root_index));
  for (i = 0; i < L->task_root_count; i++)
    *rootslot(L, cast(uintptr_t, luaC_taskrootat(L, i))) = cast(uint16_t, i + 1);
}

static void clearroots (lua_State *L) {
  L->task_trace = (LuaNurseryTrace){0};
  L->task_mark_scan = 0;
  L->task_nursery_pending = 0;
  if (L->task_root_count > L->task_root_capacity / 32) {
    L->task_root_count = 0;
    indexroots(L);
  }
  else {
    /* Both insertion and rehashing visit dense roots in order. Removing
    ** the newest slot first cannot break a probe for an older one. */
    while (L->task_root_count != 0) {
      GCObject *root = luaC_taskrootat(L, --L->task_root_count);
      *rootslot(L, cast(uintptr_t, root)) = 0;
    }
  }
}

static int growroots (lua_State *L) {
  unsigned capacity = L->task_root_capacity * 2;
  size_t oldsize;
  uintptr_t *old, *storage, *roots;
  LuaRootSnapshot *snapshot = NULL;
  if (capacity > TASK_NURSERY_ROOT_MAX) return 0;
  storage = cast(uintptr_t *, luaM_tryalloc(L, luaC_taskrootsize(capacity)));
  if (storage == NULL) return 0;
  if (capacity > LUA_TASK_DIRECT_ROOTS)
    snapshot = cast(LuaRootSnapshot *, luaM_tryalloc(L, luaC_rootsnapshotbytes(capacity)));
  if (!luaC_hastaskheap(L) || (capacity > LUA_TASK_DIRECT_ROOTS && snapshot == NULL)) {
    /* An allocator wait can seal the cohort. Leave the old directory in
    ** place unless both replacements are available. */
    if (snapshot != NULL)
      luaM_free_(L, snapshot, luaC_rootsnapshotbytes(capacity));
    luaM_free_(L, storage, luaC_taskrootsize(capacity));
    return 0;
  }
  if (snapshot != NULL) {
    snapshot->tt = LUA_VROOTSNAPSHOT;
    setmarked(snapshot, luaC_newmark(G(L)));
    snapshot->next = snapshot->gclist = NULL;
    snapshot->capacity = capacity;
    snapshot->epoch = 0;
    luaC_publishobjects(L, obj2gco(snapshot), obj2gco(snapshot));
    *storage = cast(uintptr_t, snapshot);
  }
  roots = storage + (snapshot != NULL);
  old = luaC_taskrootallocation(L);
  oldsize = luaC_taskrootbytes(L);
  memcpy(roots, L->task_roots, L->task_root_count * sizeof(*roots));
  L->task_roots = roots;
  L->task_root_index = cast(uint16_t *, roots + capacity);
  L->task_root_capacity = capacity;
  indexroots(L);
  if (oldsize != 0) luaM_free_(L, old, oldsize);
  return 1;
}

/* A mature trace can run across several polls. Reserve room for new edges
** while it decides whether the directory needs to grow. */
l_sinline unsigned rootlimit (const lua_State *L) {
  unsigned capacity = L->task_root_capacity;
  if (L->task_old_bytes >= TASK_MATURE_START && capacity < TASK_NURSERY_ROOT_MAX)
    return capacity - capacity / 8;
  return capacity - 1;
}

void luaC_freetaskroots (lua_State *L) {
  size_t size = luaC_taskrootbytes(L);
  lua_assert(L->task_root_count == 0);
  if (size != 0) {
    uintptr_t *roots = luaC_taskrootallocation(L);
    L->task_roots = L->task_root_inline;
    L->task_root_index = L->task_root_inline_index;
    L->task_root_capacity = LUA_TASK_INLINE_ROOTS;
    indexroots(L);
    luaM_free_(L, roots, size);
  }
}

static void taskresize (lua_State *L, l_mem change) {
  lua_assert(change >= 0 || cast(size_t, -change) <= L->task_live_bytes);
  L->task_live_bytes += change;
  luaM_privatebytes(L, change);
}

void luaC_taskresize (lua_State *L, Table *t, l_mem change) {
  lua_assert(luaC_tasktable(t));
  if (L->task_trace.table == t) L->task_trace.offset = 0;
  if (luaC_tablestate(t) & NURSERY_TASKOLD) {
    lua_assert(change >= 0 || cast(size_t, -change) <= L->task_old_bytes);
    L->task_old_bytes += change;
  }
  taskresize(L, change);
}

static size_t taskcapacity (const lua_State *L) {
  return L->task_old_bytes >= TASK_MATURE_START
    ? TASK_MATURE_BYTES + TASK_YOUNG_BYTES : TASK_NURSERY_BYTES;
}

static int taskspace (const lua_State *L, size_t bytes) {
  size_t used = L->task_nursery_bytes;
  size_t capacity;
  if (used < TASK_NURSERY_BYTES && bytes <= TASK_NURSERY_BYTES - used)
    return 1;
  capacity = taskcapacity(L);
  return used <= capacity && bytes <= capacity - used;
}

static int nurseryopen (lua_State *L) {
  return !L->task_nursery_closed && !gethookmask(L) && gcrunning(G(L));
}

int luaC_tasktableeligible (lua_State *L, size_t bytes) {
  return nurseryopen(L) && taskspace(L, bytes);
}

/* Limit fragmented private pages independently of the live payload. The
** mature allowance is granted only after a local scan finds survivors. */
int luaC_taskpagefits (lua_State *L, size_t bytes) {
  size_t capacity = taskcapacity(L);
  size_t limit = capacity + capacity / 4;
  return L->task_page_bytes <= limit &&
         bytes <= limit - L->task_page_bytes;
}

static void charge (lua_State *L, size_t bytes) {
  L->task_nursery_bytes += bytes;
  /* A C API allocation has no NEWTABLE instruction to check the budget.
  ** Let the next Lua poll collect after the native roots are restored. */
  if (L->task_nursery_bytes >= L->task_nursery_limit) {
    L->task_nursery_pending = 1;
    L->poll_remaining = 1;
  }
}

int luaC_taskallocate (lua_State *L, GCObject *o, size_t bytes) {
  int table = o->tt == LUA_VTABLE;
  LuaAllocPage *allocation = luaMP_page(cast(LuaAllocBlock *, o) - 1);
  int page = allocation != NULL &&
    __atomic_load_n(&allocation->task_private, __ATOMIC_RELAXED);
  if (!page) {
    if (!nurseryopen(L)) return 0;
    if (!taskspace(L, bytes) ||
        bytes > TASK_UNPAGED_BYTES - L->task_unpaged_bytes) {
      luaC_sealtask(L);
      return 0;
    }
  }
  if (L->task_nursery_bytes == 0) L->task_nursery_limit = TASK_NURSERY_START;
  if (table)
    gco2t(o)->gcstate = page ? LUA_TASK_PAGE_STATE : NURSERY_TAG | NURSERY_TASK;
  else if (o->tt == LUA_VUSERDATA) gco2u(o)->task_flags = TASK_UDATA;
  else gco2ts(o)->extra = LSTR_TASK;
  if (!page || !table) {
    luaC_allocnext(o) = L->task_objects;
    L->task_objects = o;
    if (!page) L->task_unpaged_bytes += cast_uint(bytes);
  }
  charge(L, bytes);
  if (page) L->task_live_bytes += bytes;  /* already charged as private */
  else taskresize(L, cast(l_mem, bytes));
  return 1;
}

static LuaAllocPage *detachtaskpages (lua_State *thread) {
  LuaAllocPage *pages = thread->task_pages;
  thread->task_pages = thread->task_young_pages = NULL;
  memset(thread->task_alloc_pages, 0, sizeof(thread->task_alloc_pages));
  thread->task_page_bytes = 0;
  return pages;
}

static void sealtaskpages (lua_State *L, lua_State *thread, lu_byte mark,
                           uint64_t epoch) {
  LuaAllocPage *page = detachtaskpages(thread);
  while (page != NULL) {
    LuaAllocPage *next = page->next;
    memset(page->trace_slots, 0, sizeof(page->trace_slots));
    page->dirty_next = NULL;
    luaM_adopttaskpage(L, page, epoch, mark);
    page = next;
  }
}

/* Escape transfers the entire bounded cohort without moving any object.
** The existing root/write barriers cover an edge added by the triggering
** operation. Publish the outgoing roots before making the cohort black. */
static void sealtask (lua_State *L, lua_State *thread) {
  global_State *g = G(L);
  uintptr_t flags = LUA_BLOCK_COLLECTABLE;
  uint64_t epoch;
  GCObject *o, *next, *strings = NULL, *tail = NULL, *finalizers = NULL;
  lu_byte mark;
  unsigned i;
  if (!luaC_hastaskheap(thread)) return;
  luaC_noteyoung(g);
  luaM_privatebytes(L, -cast(l_mem, thread->task_live_bytes));
  thread->task_live_bytes = 0;
  /* Arming can rotate the stamp during the transfer. A single stamp makes
  ** the whole cohort black in this trace or visible from its roots in the
  ** next one. A black parent must never hide a child with the old stamp. */
  mark = luaC_newmark(g);
  epoch = g->sweep_plan.epoch;
  if (luaC_sweeping(L)) flags |= luaMP_birth(epoch);
  for (i = 0; i < thread->task_root_count; i++) {
    GCObject *root = luaC_taskrootat(thread, i);
    /* The outgoing directory is about to disappear. The published cohort
    ** can still refer into this thread's current construction nursery. */
    if (root->tt == LUA_VTABLE && !luaC_tracingglobal(g)) {
      Table *table = gco2t(root);
      if (luaC_nurserytable(thread, table))
        luaC_settablestate(table, luaC_tablestate(table) | NURSERY_ROOT);
    }
    luaC_traceobject(L, root);
  }
  for (o = thread->task_remembered; o != obj2gco(thread); o = next) {
    next = o->next;
    o->next = NULL;
  }
  for (o = thread->task_objects; o != NULL; o = next) {
    int finalizable = o->tt == LUA_VUSERDATA && tofinalize(o);
    next = luaC_allocnext(o);
    o->next = NULL;
    setmarked(o, mark | (finalizable ? bitmask(FINALIZEDBIT) : 0));
    if (o->tt == LUA_VTABLE) {
      LuaAllocBlock *block = cast(LuaAllocBlock *, o) - 1;
      LuaAllocPage *page = luaMP_page(block);
      luaC_settablestate(gco2t(o), 0);
      __atomic_store_n(&block->block.page, cast(uintptr_t, page) | flags,
                       __ATOMIC_RELEASE);
      luaMP_publishslot(L, page, epoch + 1);
    }
    else if (o->tt == LUA_VUSERDATA) {
      gco2u(o)->gclist = NULL;
      luaC_publishtaskuserdata(L, o);
      if (finalizable) {
        o->next = finalizers;
        finalizers = o;
      }
    }
    else {
      TString *s = gco2ts(o);
      __atomic_fetch_and(&s->extra, cast_byte(~(LSTR_TASK | LSTR_TASKOLD | LSTR_TASKLIVE)),
                         __ATOMIC_RELEASE);
      luaC_allocnext(o) = strings;
      strings = o;
      if (tail == NULL) tail = o;
    }
  }
  while (finalizers != NULL) {
    Udata *userdata = gco2u(finalizers);
    next = finalizers->next;
    __atomic_store_n(&userdata->sharing, LUA_OBJECT_SHARED, __ATOMIC_RELEASE);
    luaC_transferfinalizer(L, finalizers);
    finalizers = next;
  }
  thread->task_finalizers = NULL;
  thread->task_finalizable = 0;
  if (strings != NULL) luaC_publishobjects(L, strings, tail);
  sealtaskpages(L, thread, mark, epoch);
  thread->task_objects = NULL;
  thread->task_old_objects = NULL;
  thread->task_remembered = obj2gco(thread);
  thread->task_old_bytes = 0;
  thread->task_minor_budget = 0;
  thread->task_unpaged_bytes = 0;
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

void luaC_retireseal (lua_State *L) {
  global_State *g = G(L);
  if (__atomic_load_n(&L->task_seal_requested, __ATOMIC_RELAXED) == 0) return;
  __atomic_store_n(&L->task_seal_requested, 0, __ATOMIC_RELAXED);
  lua_assert(g->seals_pending != 0);
  if (--g->seals_pending == 0)
    luaQ_wake(&g->world.changed, &g->world.waiters);
}

static void __attribute__((cold)) sealowned (lua_State *L, lua_State *thread) {
  LuaWorld *world = &G(L)->world;
  if (__atomic_load_n(&thread->task_seal_requested, __ATOMIC_ACQUIRE) == 0)
    return;
  sealtask(L, thread);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  luaC_retireseal(thread);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void __attribute__((cold)) luaC_pollseal (lua_State *L) {
  sealowned(L, L);
  if (L->stack_peer != NULL) sealowned(L, L->stack_peer);
}

/* The full collector keeps metadata ownership so colors cannot roll over.
** Active tasks transfer their own cohorts. An idle stack may be borrowed
** under its gate, and a parked owner's cohort is stable under world.mutex.
** A task can open a new cohort after acknowledging. The shared trace
** sees its outgoing roots, as it does for cohorts born during marking. */
void __attribute__((cold)) luaC_preparefull (lua_State *L) {
  global_State *g = G(L);
  LuaWorld *world = &g->world;
  LuaTaskRoot *task;
  lua_assert(luaW_ownsheap(L) && L->world_depth == 0);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(g->seals_pending == 0);
  __atomic_fetch_or(&world->control, LUA_WORLD_SEAL_NURSERIES, __ATOMIC_SEQ_CST);
  for (task = g->tasks; task != NULL; task = task->next) {
    lua_State *thread = task->thread;
    if (thread->task_owner == task) {
      __atomic_store_n(&thread->task_seal_requested, 1, __ATOMIC_RELEASE);
      g->seals_pending++;
    }
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaC_pollseal(L);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  while (g->seals_pending != 0) {
    lua_State *idle[8];
    unsigned count = 0, i;
    for (task = g->tasks; task != NULL; task = task->next) {
      lua_State *thread = task->thread;
      int reservation;
      if (__atomic_load_n(&thread->task_seal_requested, __ATOMIC_RELAXED) == 0)
        continue;
      reservation = __atomic_load_n(&thread->root_waiting, __ATOMIC_RELAXED)
        ? 2 : luaX_tryreserve(thread);
      if (reservation == 2) {
        sealtask(L, thread);
        luaC_retireseal(thread);
        break;  /* bound the work under the world mutex */
      }
      if (reservation == 1) {
        idle[count++] = thread;
        if (count == sizeof(idle) / sizeof(idle[0])) break;
      }
    }
    if (count != 0 || task != NULL) {
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
      for (i = 0; i < count; i++) {
        sealowned(L, idle[i]);
        luaX_endreserve(idle[i], 1);
      }
      luaX_checksync(pthread_mutex_lock(&world->mutex));
    }
    else if (g->seals_pending != 0) {
      L->native_depth++;
      luaW_deactivate(L);
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
      luaW_resume(L);
      L->native_depth--;
    }
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

/* Remember the target until its first local collection. Overwriting a slot
** can leave a floating survivor until the next full pass. Private objects
** can use the header link until publication. The thread sentinel lets a
** non-null link identify even the last entry and deduplicate repeated writes. */
static void remembertarget (lua_State *L, GCObject *value) {
  int old;
  if (value->next != NULL) return;
  if (value->tt == LUA_VTABLE)
    old = (luaC_tablestate(gco2t(value)) & NURSERY_TASKOLD) != 0;
  else if (value->tt == LUA_VUSERDATA)
    old = (gco2u(value)->task_flags & TASK_UDATA_OLD) != 0;
  else old = (__atomic_load_n(&gco2ts(value)->extra, __ATOMIC_RELAXED) &
              LSTR_TASKOLD) != 0;
  if (old) return;
  value->next = L->task_remembered;
  L->task_remembered = value;
}

l_sinline int tryretaintaskroot (lua_State *L, GCObject *value, uintptr_t flags) {
  uintptr_t root = cast(uintptr_t, value);
  uint16_t *slot = rootslot(L, root);
  lua_assert(!(root & LUA_TASK_ROOT_FLAGS));
  if (L->task_mark_scan != 0) flags |= LUA_TASK_ROOT_LIVE;
  if (*slot != 0) {
    if (flags != 0) L->task_roots[*slot - 1] |= flags;
    return 1;
  }
  if (L->task_root_count < L->task_root_capacity) {
    L->task_roots[L->task_root_count++] = root | flags;
    *slot = cast(uint16_t, L->task_root_count);
    /* Grow or prune at the next instruction, including loops that only
    ** update tables. The barrier itself cannot park or allocate. */
    if (L->task_root_count == rootlimit(L) ||
        L->task_root_count == L->task_root_capacity - 1) {
      L->task_nursery_pending = 1;
      L->poll_remaining = 1;
    }
    luaC_traceobject(L, value);
    return 1;
  }
  return 0;
}

int luaC_bindtaskuserdata (lua_State *L, Udata *userdata, Table *mt,
                           int finalizable) {
  if (mt != NULL && !tryretaintaskroot(L, obj2gco(mt), 0)) {
    luaC_sealtask(L);
    return 0;
  }
  luaA_storeudatameta(userdata, mt);
  if (finalizable && luaC_markfinalizable(L, obj2gco(userdata))) {
    L->task_finalizable++;
  }
  return 1;
}

/* Immutable key layouts belong to the cohort's root directory. Only the
** pass that prunes that directory needs to visit each record's layout. */
int luaC_tasklayout (lua_State *L, Table *table, Table *keys) {
  if (!luaC_tasktable(table)) return 0;
  lua_assert(!luaC_tasktable(keys));
  if (tryretaintaskroot(L, obj2gco(keys), LUA_TASK_ROOT_LAYOUT)) return 1;
  luaC_sealtask(L);
  return 0;
}

int luaC_trytaskedge (lua_State *L, Table *table, uint64_t state,
                     GCObject *value) {
  /* Set the edge flag before visiting a possible self-reference. Visiting
  ** can add another bit to this same nursery header. */
  if (value->tt != LUA_VSHRSTR && !(state & NURSERY_TASK_EDGES))
    luaC_settablestate(table, state | NURSERY_TASK_EDGES);
  if (luaC_taskobject(value)) {
    if (l_unlikely(L->task_mark_scan != 0))
      visitnursery(L, value, &L->task_trace.gray, cast(NurseryScan, L->task_mark_scan));
    else if (state & NURSERY_TASKOLD) remembertarget(L, value);
  }
  else if (!tryretaintaskroot(L, value, 0)) return 0;
  return 1;
}

int luaC_taskedge (lua_State *L, GCObject *owner, GCObject *value) {
  Table *table = owner->tt == LUA_VTABLE ? gco2t(owner) : NULL;
  uint64_t state = table != NULL ? luaC_tablestate(table) : 0;
  lua_assert(luaC_hastaskheap(L));
  if (luaC_tasktablestate(table, state)) {
    if (luaC_trytaskedge(L, table, state, value)) return 1;
    luaC_sealtask(L);
  }
  else if (luaC_taskobject(value)) luaC_sealtask(L);
  return 0;
}

void luaC_taskbacking (lua_State *L, Table *table, size_t bytes) {
  if (luaC_tasktable(table)) {
    LuaAllocPage *page = luaMP_page(cast(LuaAllocBlock *, table) - 1);
    lua_assert(luaC_hastaskheap(L));
    if (!taskspace(L, bytes)) luaC_sealtask(L);
    else {
      if (__atomic_load_n(&page->task_private, __ATOMIC_RELAXED))
        page->task_backing = 1;
      charge(L, bytes);
    }
  }
}

static void pacenursery (lua_State *L, size_t live) {
  global_State *g = G(L);
  size_t dead = L->task_live_bytes - live;
  lua_assert(L->task_old_bytes <= L->task_live_bytes);
  size_t young = L->task_live_bytes - L->task_old_bytes;
  /* The private heap governs its own live bytes. Charge a fraction of local
  ** allocation or reclamation to keep shared weak references and finalizers
  ** progressing even in programs which mostly use private objects. */
  size_t work = young > dead ? young : dead;
  L->task_live_bytes = live;
  if (work != 0 && gcrunning(g) && !luaC_tracing(L) && !luaC_sweeping(L))
    luaP_spend(g, cast(l_mem, work / 16 + 1));
}

/* Task-private headers never enter shared sweep. Return contiguous
** runs from the same page together after disposing of their backing parts. */
l_sinline void freeset (lua_State *L, GCObject *o, int allocation_links) {
  while (o != NULL) {
    LuaAllocBlock *last = cast(LuaAllocBlock *, o) - 1, *first = NULL;
    LuaAllocPage *page = luaMP_page(last);
    int inlineonly = page != NULL &&
      __atomic_load_n(&page->task_private, __ATOMIC_ACQUIRE) && !page->task_backing;
    unsigned count = 0;
    l_mem bytes = 0;
    if (page == NULL) {
      GCObject *next = allocation_links ? luaC_allocnext(o) : o->next;
      Udata *userdata = gco2u(o);
      luaM_freeprivate_(L, userdata, sizeudata(userdata->nuvalue, userdata->len));
      o = next;
      continue;
    }
    for (;;) {
      LuaAllocBlock *block = cast(LuaAllocBlock *, o) - 1;
      GCObject *next = allocation_links ? luaC_allocnext(o) : o->next;
      if (o->tt == LUA_VUSERDATA)
        __atomic_store_n(&block->block.page, cast(uintptr_t, page),
                          __ATOMIC_RELAXED);
      lua_assert(luaMP_header(block) == cast(uintptr_t, page));
      if (o->tt == LUA_VTABLE) {
        Table *table = gco2t(o);
        lua_assert(luaC_tasktable(table) && luaA_lockowner(&table->access) == NULL);
        bytes += sizeof(*table) + table->inline_bytes;
        if (!inlineonly) luaH_freeparts(L, table, 1);
      }
      else if (o->tt == LUA_VUSERDATA) {
        Udata *userdata = gco2u(o);
        lua_assert(luaC_taskuserdata(o) && !tofinalize(o));
        bytes += sizeudata(userdata->nuvalue, userdata->len);
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
    luaM_freeobjectbatch(L, page, first, last, count, bytes, 1);
  }
}

static void discardtaskpages (lua_State *L, LuaAllocPage *page) {
  while (page != NULL) {
    LuaAllocPage *next = page->next;
    if (page->task_backing) {
      unsigned word;
      for (word = 0; word < LUA_PAGE_TRACE_WORDS; word++) {
        uint64_t slots = page->trace_slots[word];
        while (slots != 0) {
          unsigned index = word * 64 + cast_uint(__builtin_ctzll(slots));
          LuaAllocBlock *block = luaMP_slotblock(page, index);
          luaH_freeparts(L, cast(Table *, block + 1), 1);
          slots &= slots - 1;
        }
      }
    }
    luaM_discardtaskpage(L, page);
    page = next;
  }
}

void luaC_endtask (lua_State *L) {
  StkId slot;
  GCObject *o;
  LuaAllocPage *pages;
  unsigned i;
  lua_assert(L->task_owner != NULL && luaC_hastaskheap(L));
  if (!gcrunning(G(L)) || L->openupval != NULL || L->native_roots != NULL ||
      L->task_finalizable != 0) {
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
    luaC_traceobject(L, luaC_taskrootat(L, i));
  o = L->task_objects;
  pages = detachtaskpages(L);
  pacenursery(L, 0);
  L->task_objects = NULL;
  L->task_old_objects = NULL;
  L->task_remembered = obj2gco(L);
  L->task_old_bytes = 0;
  L->task_minor_budget = 0;
  L->task_unpaged_bytes = 0;
  clearroots(L);
  L->task_nursery_bytes = 0;
  L->task_nursery_closed = 1;
  freeset(L, o, 1);
  discardtaskpages(L, pages);
  luaM_flushdebt(L);
}

l_sinline int marknursery (lua_State *L, GCObject *object, GCObject **gray,
                            NurseryScan scan) {
  if (object->tt == LUA_VTABLE) {
    Table *t = gco2t(object);
    uint64_t state = luaC_tablestate(t);
    if (scan != NURSERY_CONSTRUCTION &&
        (state & LUA_TASK_PAGE_STATE) == LUA_TASK_PAGE_STATE) {
      const LuaAllocBlock *block = cast(const LuaAllocBlock *, t) - 1;
      LuaAllocPage *page = luaMP_page(block);
      unsigned slot;
      uint64_t *live, bit;
      if (!__atomic_load_n(&page->task_private, __ATOMIC_ACQUIRE)) return 0;
      if (scan == NURSERY_TASK_YOUNG && (state & NURSERY_TASKOLD)) return 1;
      slot = luaMP_slotindex(page, block);
      bit = UINT64_C(1) << (slot % 64);
      live = &luaMP_taskbits(page)->live[slot / 64];
      if (*live & bit) return 1;
      *live |= bit;
    }
    else {
      if (scan == NURSERY_CONSTRUCTION ? !luaC_nurserytable(L, t)
                                       : !luaC_tasktablestate(t, state))
        return 0;
      if ((state & NURSERY_LIVE) ||
          (scan == NURSERY_TASK_YOUNG && (state & NURSERY_TASKOLD)))
        return 1;
      luaC_settablestate(t, state | NURSERY_LIVE);
    }
    if (scan == NURSERY_CONSTRUCTION || scan == NURSERY_TASK_ROOTS ||
        (state & NURSERY_TASK_EDGES)) {
      t->gclist = *gray;
      *gray = object;
    }
  }
  else if (object->tt == LUA_VUSERDATA) {
    Udata *userdata = gco2u(object);
    lu_byte flags = __atomic_load_n(&userdata->task_flags, __ATOMIC_RELAXED);
    if (scan == NURSERY_CONSTRUCTION || !(flags & TASK_UDATA)) return 0;
    if (scan == NURSERY_TASK_YOUNG && (flags & TASK_UDATA_OLD)) return 1;
    if (!(flags & TASK_UDATA_LIVE)) {
      __atomic_store_n(&userdata->task_flags, flags | TASK_UDATA_LIVE,
                       __ATOMIC_RELAXED);
      if (scan >= NURSERY_TASK_FULL && userdata->metatable != NULL)
        visitnursery(L, obj2gco(userdata->metatable), gray, scan);
    }
  }
  else {
    TString *s;
    lu_byte flags;
    if (scan == NURSERY_CONSTRUCTION || object->tt != LUA_VLNGSTR) return 0;
    s = gco2ts(object);
    flags = __atomic_load_n(&s->extra, __ATOMIC_RELAXED);
    if (!(flags & LSTR_TASK)) return 0;
    if (scan == NURSERY_TASK_YOUNG && (flags & LSTR_TASKOLD)) return 1;
    if (!(flags & LSTR_TASKLIVE))
      __atomic_store_n(&s->extra, flags | LSTR_TASKLIVE, __ATOMIC_RELAXED);
  }
  return 1;
}

l_sinline int nurseryedge (lu_byte tag, NurseryScan scan) {
  if (scan == NURSERY_CONSTRUCTION) return tag == ctb(LUA_VTABLE);
  if (scan == NURSERY_TASK_YOUNG)
    return tag == ctb(LUA_VTABLE) || tag == ctb(LUA_VLNGSTR) ||
           tag == ctb(LUA_VUSERDATA);
  return (tag & BIT_ISCOLLECTABLE) &&
    (scan == NURSERY_TASK_ROOTS || tag != ctb(LUA_VSHRSTR));
}

l_sinline void visitnursery (lua_State *L, GCObject *object, GCObject **gray,
                              NurseryScan scan) {
  if (!marknursery(L, object, gray, scan) && scan >= NURSERY_TASK_FULL) {
    uint16_t index = *rootslot(L, cast(uintptr_t, object));
    lua_assert(index != 0 && index <= L->task_root_count);
    if (index != 0) L->task_roots[index - 1] |= LUA_TASK_ROOT_LIVE;
  }
}

/* In-place insertion may move an unscanned pair behind the cursor. Unlike
** backing replacement, it can preserve progress by visiting just that pair. */
void luaC_taskrelocate (lua_State *L, const Node *base, const Node *slot) {
  NurseryScan scan = cast(NurseryScan, L->task_mark_scan);
  lua_assert(scan == NURSERY_TASK_FULL || scan == NURSERY_TASK_ROOTS);
  if (L->task_trace.table->node != base) return;
  if (nurseryedge(keytt(slot), scan))
    visitnursery(L, gckey(slot), &L->task_trace.gray, scan);
  if (nurseryedge(rawtt(gval(slot)), scan))
    visitnursery(L, gcvalue(gval(slot)), &L->task_trace.gray, scan);
}

/* Keep immutable layouts and interned strings until the bounded directory
** needs room or the cohort dies. Removed roots still belong to any shared
** snapshot already in progress. */
static void prunetaskroots (lua_State *L, int prune_cached) {
  unsigned i, count = 0;
  for (i = 0; i < L->task_root_count; i++) {
    uintptr_t root = L->task_roots[i];
    if ((root & LUA_TASK_ROOT_LIVE) || (!prune_cached &&
          ((root & LUA_TASK_ROOT_LAYOUT) || luaC_taskrootat(L, i)->tt == LUA_VSHRSTR)))
      L->task_roots[count++] = root & ~LUA_TASK_ROOT_LIVE;
    else luaC_traceobject(L, luaC_taskrootat(L, i));
  }
  if (count != L->task_root_count) {
    L->task_root_count = count;
    indexroots(L);
  }
}

static void markstack (lua_State *L, GCObject **gray, NurseryScan scan) {
  StkId slot;
  for (slot = L->stack.p; slot < L->top.p; slot++)
    if (nurseryedge(rawtt(s2v(slot)), scan))
      marknursery(L, gcvalue(s2v(slot)), gray, scan);
}

static void seednursery (lua_State *L, LuaNurseryTrace *trace, NurseryScan scan) {
  if (scan != NURSERY_CONSTRUCTION) {
    GCObject *o = L->task_remembered;
    while (o != obj2gco(L)) {
      GCObject *next = o->next;
      o->next = NULL;
      if (scan == NURSERY_TASK_YOUNG) marknursery(L, o, &trace->gray, scan);
      o = next;
    }
    L->task_remembered = obj2gco(L);
  }
  markstack(L, &trace->gray, scan);
}

/* Charge slots as well as tables. A single large array or hash image must
** be resumable even when the rest of the gray queue is empty. */
static int drainnursery (lua_State *L, LuaNurseryTrace *trace, NurseryScan scan,
                         size_t budget) {
  GCObject *gray = trace->gray;
  Table *t = trace->table;
  size_t offset = trace->offset;
  while (budget != 0 && (gray != NULL || t != NULL)) {
    size_t start, stop, end;
    if (t == NULL) {
      t = gco2t(gray);
      gray = t->gclist;
      offset = 0;
      budget--;
    }
    start = offset;
    if (start == 0) {
      Table *keys = luaH_recordkeys(t->node);
      if (t->metatable != NULL)
        visitnursery(L, obj2gco(t->metatable), &gray, scan);
      if (keys != NULL) {
        Value *values = luaH_inlinerecordvalues(t);
        uintptr_t tags = luaH_recordtags(t->arraydata);
        unsigned i;
        if (scan == NURSERY_CONSTRUCTION || scan == NURSERY_TASK_ROOTS)
          visitnursery(L, obj2gco(keys), &gray, scan);
        for (i = 0; i < keys->asize; i++) {
          lu_byte tag = luaH_recordtag(tags, i);
          if (tag == LUA_RECORD_WIDE) {
            TValue value = {0};
            if (nurseryedge(luaH_ownedrecordvalue(t, t->arraydata, i,
                                                  tag, &value), scan))
              visitnursery(L, gcvalue(&value), &gray, scan);
          }
          else if (nurseryedge(tag, scan))
            visitnursery(L, values[i].gc, &gray, scan);
        }
        budget -= budget < keys->asize ? budget : keys->asize;
        t = NULL;
        continue;
      }
    }
    end = cast(size_t, t->asize) + allocsizenode(t);
    stop = budget < end - start ? start + budget : end;
    budget -= stop - start;
    for (; start < stop && start < t->asize; start++)
      if (nurseryedge(*getArrTag(t, start), scan))
        visitnursery(L, getArrVal(t, start)->gc, &gray, scan);
    for (; start < stop; start++) {
      Node *node = gnode(t, start - t->asize);
      if (isempty(gval(node))) continue;
      if (nurseryedge(keytt(node), scan))
        visitnursery(L, gckey(node), &gray, scan);
      if (nurseryedge(rawtt(gval(node)), scan))
        visitnursery(L, gcvalue(gval(node)), &gray, scan);
    }
    offset = stop;
    if (stop == end) t = NULL;
  }
  *trace = (LuaNurseryTrace){gray, t, offset};
  return t == NULL && gray == NULL;
}

static void tracenursery (lua_State *L, GCObject *gray, NurseryScan scan) {
  LuaNurseryTrace trace = {gray, NULL, 0};
  seednursery(L, &trace, scan);
  while (!drainnursery(L, &trace, scan, MAX_SIZET)) {}
}

/* Sweep whole words and leave mature survivors' headers untouched. Remove
** dead slots from the directory before a backing-buffer free can release
** admission. The outstanding slots pin their allocation page. */
static size_t sweeptaskpages (lua_State *L, NurseryScan scan, GCObject **dead) {
  int young = scan == NURSERY_TASK_YOUNG;
  LuaAllocPage *page = young ? L->task_young_pages : L->task_pages;
  size_t reclaimed = 0;
  L->task_young_pages = NULL;
  while (page != NULL) {
    LuaAllocPage *next = young ? page->dirty_next : page->next;
    LuaTaskPageBits *bits = luaMP_taskbits(page);
    unsigned word;
    if (page->pool >= LUA_ALLOC_USERDATA_POOL) {
      page = next;
      continue;
    }
    page->task_dirty = 0;
    for (word = 0; word < LUA_PAGE_TRACE_WORDS; word++) {
      uint64_t live = bits->live[word];
      uint64_t youngslots = bits->young[word];
      uint64_t slots = (young ? youngslots : page->trace_slots[word]) & ~live;
      uint64_t survivors = youngslots & live;
      bits->young[word] = bits->live[word] = 0;
      page->trace_slots[word] &= ~slots;
      while (survivors != 0) {
        unsigned index = word * 64 + cast_uint(__builtin_ctzll(survivors));
        Table *table = cast(Table *, luaMP_slotblock(page, index) + 1);
        survivors &= survivors - 1;
        luaC_settablestate(table, luaC_tablestate(table) | NURSERY_TASKOLD);
      }
      while (slots != 0) {
        unsigned bit = cast_uint(__builtin_ctzll(slots));
        LuaAllocBlock *block = luaMP_slotblock(page, word * 64 + bit);
        Table *table = cast(Table *, block + 1);
        slots &= slots - 1;
        /* A backing-buffer free can permit publication of this page. */
        __atomic_store_n(&block->block.page, cast(uintptr_t, page),
                          __ATOMIC_RELAXED);
        reclaimed += page->task_backing ? luaH_size(table, table)
                       : sizeof(*table) + table->inline_bytes;
        table->next = *dead;
        *dead = obj2gco(table);
      }
    }
    page = next;
  }
  return reclaimed;
}

static size_t sweepnursery (lua_State *L, GCObject **link, NurseryScan scan) {
  int task = scan != NURSERY_CONSTRUCTION;
  GCObject *end = scan == NURSERY_TASK_YOUNG ? L->task_old_objects : NULL;
  GCObject *dead = NULL, *o, *finalizing = NULL;
  GCObject **finalizers = &L->task_finalizers;
  GCObject **finalizing_tail = &finalizing;
  size_t bytes = 0, reclaimed = task ? sweeptaskpages(L, scan, &dead) : 0;
  size_t finalizing_bytes = 0;
  StkId slot;
  while ((o = *link) != end) {
    if (o->tt == LUA_VTABLE) {
      Table *t = gco2t(o);
      if (task || luaC_nurserytable(L, t)) {
        uint64_t state = luaC_tablestate(t);
        lua_assert(!task || luaC_tasktablestate(t, state));
        if (!(state & NURSERY_LIVE)) goto detach;
        state &= ~cast(uint64_t, NURSERY_LIVE);
        if (task) state |= NURSERY_TASKOLD;
        luaC_settablestate(t, state);
      }
    }
    else if (task && o->tt == LUA_VUSERDATA) {
      Udata *userdata = gco2u(o);
      lu_byte flags = __atomic_load_n(&userdata->task_flags, __ATOMIC_RELAXED);
      if (!(flags & TASK_UDATA_LIVE)) {
        GCObject *gray = NULL;
        if (!tofinalize(o)) goto detach;
        *finalizers = o;
        finalizers = &userdata->gclist;
        if (scan >= NURSERY_TASK_FULL && userdata->metatable != NULL)
          visitnursery(L, obj2gco(userdata->metatable), &gray, scan);
        lua_assert(gray == NULL);
        /* Finalizer retention lasts until the next nursery scan. Treat it
        ** as young so dead objects cannot accumulate among old survivors. */
        finalizing_bytes += sizeudata(userdata->nuvalue, userdata->len);
        *link = luaC_allocnext(o);
        *finalizing_tail = o;
        finalizing_tail = luaC_alloclink(o);
        __atomic_store_n(&userdata->task_flags, TASK_UDATA, __ATOMIC_RELAXED);
        continue;
      }
      __atomic_store_n(&userdata->task_flags,
                       (flags & ~TASK_UDATA_LIVE) | TASK_UDATA_OLD,
                       __ATOMIC_RELAXED);
    }
    else if (task) {
      TString *s = gco2ts(o);
      lu_byte flags = __atomic_load_n(&s->extra, __ATOMIC_RELAXED);
      lua_assert(flags & LSTR_TASK);
      if (!(flags & LSTR_TASKLIVE)) goto detach;
      __atomic_store_n(&s->extra, (flags & ~LSTR_TASKLIVE) |
                                  LSTR_TASKOLD, __ATOMIC_RELAXED);
    }
    link = luaC_alloclink(o);
    continue;
  detach:
    *link = luaC_allocnext(o);
    if (task) {
      size_t bytes;
      if (o->tt == LUA_VTABLE) {
        Table *table = gco2t(o);
        bytes = sizeof(*table) + table->inline_bytes;
        reclaimed += luaH_size(table, table);
      }
      else if (o->tt == LUA_VUSERDATA) {
        Udata *userdata = gco2u(o);
        bytes = sizeudata(userdata->nuvalue, userdata->len);
        reclaimed += bytes;
      }
      else {
        bytes = sizestrreg(gco2ts(o)->u.lnglen);
        reclaimed += bytes;
      }
      {
        LuaAllocPage *page = luaMP_page(cast(LuaAllocBlock *, o) - 1);
        if (page == NULL ||
            !__atomic_load_n(&page->task_private, __ATOMIC_RELAXED)) {
          lua_assert(bytes <= L->task_unpaged_bytes);
          L->task_unpaged_bytes -= cast_uint(bytes);
        }
      }
    }
    else {
      LuaAllocBlock *block = cast(LuaAllocBlock *, o) - 1;
      /* Detach from the sweep directory before a backing free can wait. */
      __atomic_store_n(&block->block.page, cast(uintptr_t, luaMP_page(block)),
                       __ATOMIC_RELAXED);
      L->nursery_tables--;
    }
    o->next = dead;
    dead = o;
  }
  if (task) *finalizers = NULL;
  if (task) {
    lua_assert(reclaimed <= L->task_live_bytes);
    bytes = L->task_live_bytes - reclaimed;
    if (scan >= NURSERY_TASK_FULL)
      prunetaskroots(L, scan == NURSERY_TASK_ROOTS || bytes == 0);
    pacenursery(L, bytes);
    L->task_old_objects = L->task_objects;
    L->task_old_bytes = bytes - finalizing_bytes;
    if (finalizing != NULL) {
      *finalizing_tail = L->task_objects;
      L->task_objects = finalizing;
    }
  }
  if (dead == NULL) return bytes;
  /* A later safepoint can widen top. Discard stale registers before freeing
  ** any of their referents, including frees that release admission. */
  for (slot = L->top.p; slot < L->stack_last.p + EXTRA_STACK; slot++)
    setnilvalue2s(slot);
  if (task) {
    freeset(L, dead, 0);
    luaM_trimtaskpages(L);
  }
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

static int cansteptask (lua_State *L) {
  global_State *g = G(L);
  return gcrunning(g) && L->native_roots == NULL && L->native_depth == 0 &&
    L->stack_peer == NULL && !L->reclaiming &&
    g->gcparams[LUA_GCPSTEPSIZE] && g->gcparams[LUA_GCPSTEPMUL];
}

void luaC_finishtasknursery (lua_State *L, void *argument) {
  size_t live;
  UNUSED(argument);
  if (!cansteptask(L) || L->task_mark_scan != 0) return;
  /* The completed coroutine has only its return values on the stack.
  ** Visit them before finalizing the unreachable remainder of its cohort. */
  tracenursery(L, NULL, NURSERY_TASK_FULL);
  live = sweepnursery(L, &L->task_objects, NURSERY_TASK_FULL);
  if (luaC_hastaskheap(L)) L->task_nursery_bytes = live;
  luaC_runtaskfinalizers(L);
}

void luaC_taskstep (lua_State *L) {
  size_t live, young_bytes;
  NurseryScan scan;
  L->task_nursery_pending = 0;
  if (!cansteptask(L)) {
    L->task_nursery_limit = taskcapacity(L) + 1;
    L->task_nursery_pending = L->task_mark_scan != 0;
    return;
  }
  if (L->task_old_bytes < TASK_NURSERY_START / 2 &&
      L->task_root_count >= rootlimit(L) &&
      L->task_nursery_bytes < L->task_nursery_limit &&
      growroots(L)) return;
  /* If insertions consumed the reserve, extend it without restarting the
  ** current trace or discarding the live bits discovered so far. */
  if (L->task_mark_scan != 0 && L->task_root_count >= L->task_root_capacity - 1)
    growroots(L);
  if (!luaC_hastaskheap(L)) return;
#if defined(LUAI_ASSERT)
  {
    UpVal *uv;
    /* Open private bindings still live in the scanned part of the stack.
    ** Materializing a binding uses the ordinary heap write barrier. */
    for (uv = L->openupval; uv != NULL; uv = uv->open.next)
      lua_assert(!luaF_localupval(L, uv) || uplevel(uv) < L->top.p);
  }
#endif
  /* Charge each young interval to a budget fixed by the last full scan. */
  lua_assert(L->task_nursery_bytes >= L->task_old_bytes);
  young_bytes = L->task_nursery_bytes - L->task_old_bytes;
  if (L->task_mark_scan != 0) scan = cast(NurseryScan, L->task_mark_scan);
  else if (L->task_root_count >= rootlimit(L)) scan = NURSERY_TASK_ROOTS;
  else if (L->task_old_bytes >= TASK_NURSERY_START / 2 &&
           L->task_nursery_bytes < taskcapacity(L) - TASK_YOUNG_BYTES &&
           young_bytes < L->task_minor_budget)
    scan = NURSERY_TASK_YOUNG;
  else scan = NURSERY_TASK_FULL;
  if (L->task_mark_scan != 0 ||
      (scan >= NURSERY_TASK_FULL && L->task_old_bytes >= TASK_MATURE_START)) {
    size_t limit = taskcapacity(L) - TASK_YOUNG_BYTES;
    if (L->task_nursery_bytes >= limit) {
      /* Keep the same space bound if application allocation outruns a mark. */
      luaC_sealtask(L);
      return;
    }
    if (L->task_mark_scan == 0) {
      L->task_mark_scan = cast_byte(scan);
      L->task_nursery_limit = limit;
      seednursery(L, &L->task_trace, scan);
    }
    if (drainnursery(L, &L->task_trace, scan, TASK_MARK_BUDGET)) {
      /* Values can move from a heap edge to a register during the trace.
      ** The insertion barrier covers table writes and this closes the stack. */
      markstack(L, &L->task_trace.gray, scan);
      if (L->task_trace.gray == NULL) {
        L->task_mark_scan = 0;
        L->task_trace = (LuaNurseryTrace){0};
      }
    }
    if (L->task_mark_scan != 0) {
      L->task_nursery_pending = 1;
      return;
    }
  }
  else tracenursery(L, NULL, scan);
  live = sweepnursery(L, &L->task_objects, scan);
  if (luaC_hastaskheap(L)) {
    if (scan == NURSERY_TASK_YOUNG)
      L->task_minor_budget -= cast_uint(young_bytes);
    else
      L->task_minor_budget = cast_uint(live > TASK_NURSERY_BYTES
        ? live : TASK_NURSERY_BYTES);
    lua_assert(L->task_live_bytes == live);
    L->task_nursery_bytes = live;
    if (live >= TASK_MATURE_START) {
      if (live >= TASK_MATURE_BYTES) luaC_sealtask(L);
      else {
        L->task_nursery_limit = live + TASK_YOUNG_BYTES;
        if (L->task_nursery_limit > TASK_MATURE_BYTES)
          L->task_nursery_limit = TASK_MATURE_BYTES;
      }
    }
    else {
      L->task_nursery_limit = TASK_NURSERY_START +
        (live < TASK_NURSERY_START / 2 ? live * 2 : live);
      if (L->task_nursery_limit > TASK_NURSERY_BYTES - TASK_NURSERY_START)
        L->task_nursery_limit = TASK_NURSERY_BYTES - TASK_NURSERY_START;
    }
    if (luaC_hastaskheap(L) && L->task_root_count >= rootlimit(L)) growroots(L);
  }
  luaC_runtaskfinalizers(L);
}

/* The admitted stack owns these nursery links. Quiescence detaches them
** before the shared collector can reuse a table's allocation prefix. */
void luaC_collectnursery (lua_State *L) {
  enum { NURSERY_MIN_TABLES = 32, NURSERY_SCAN_LIMIT = 16384,
         NURSERY_SCANS_PER_CYCLE = 16 };
  global_State *g = G(L);
  GCObject *gray = NULL, *o;
  size_t remaining, slots, unpinned = 0;
  unsigned i;
  if (L->nursery_tables < NURSERY_MIN_TABLES ||
      L->openupval != NULL || L->native_roots != NULL ||
      L->native_depth != 0 || L->stack_peer != NULL || L->reclaiming ||
      luaC_hasfinalizers(g) || luaW_stopped(&g->world) ||
      g->gcparams[LUA_GCPSTEPSIZE] == 0 || g->gcparams[LUA_GCPSTEPMUL] == 0 ||
      __atomic_load_n(&g->pacer.nursery_scans, __ATOMIC_RELAXED) >=
        NURSERY_SCANS_PER_CYCLE)
    return;
  /* Bound both directory walks, the stack cleanup, and every possible edge
  ** visit before changing any local mark state. Large cohorts use shared GC. */
  slots = cast_sizet(L->stack_last.p - L->stack.p) + EXTRA_STACK +
    L->task_root_count;
  if (slots >= NURSERY_SCAN_LIMIT) return;
  remaining = NURSERY_SCAN_LIMIT - slots;
  for (o = L->new_tables; o != NULL; o = luaC_allocnext(o)) {
    if (--remaining == 0) return;
    if (o->tt == LUA_VTABLE && luaC_nurserytable(L, gco2t(o))) {
      Table *t = gco2t(o);
      Table *keys = luaH_recordkeys(t->node);
      unsigned nodes = allocsizenode(t);
      if (keys != NULL) nodes += keys->asize + 1;
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
      marknursery(L, o, &gray, NURSERY_CONSTRUCTION);
  /* A task can reopen its private cohort while construction tables from
  ** before the handoff remain unpublished. Its outgoing edges are roots
  ** even when no stack slot points directly into that older cohort. */
  for (i = 0; i < L->task_root_count; i++)
    marknursery(L, luaC_taskrootat(L, i), &gray, NURSERY_CONSTRUCTION);
  tracenursery(L, gray, NURSERY_CONSTRUCTION);
  sweepnursery(L, &L->new_tables, NURSERY_CONSTRUCTION);
}
