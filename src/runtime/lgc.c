/*
** $Id: lgc.c $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#define lgc_c
#define LUA_CORE

#include "lprefix.h"

#include <string.h>
#include <errno.h>


#include "lua.h"

#include "laccess.h"
#include "lcallback.h"
#include "ltask.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lmempage.h"
#include "lobject.h"
#include "lstate.h"
#include "lroots.h"
#include "lscheduler.h"
#include "luthr_hook_policy.h"
#include "lstring.h"
#include "ltable.h"
#include "ltablestorage.h"
#include "ltm.h"


/*
** Maximum number of elements to sweep in each single step.
** (Large enough to dissipate fixed overheads but small enough
** to allow small steps for the collector.)
*/
#define GCSWEEPMAX	20


/*
** Cost (in work units) of running one finalizer.
*/
#define CWUFIN	10


/* A queued mark and a remembered owner each exclusively own their trace link. */
#define set2gray(x) l_setbit((x)->marked, QUEUEDBIT)


#define valiswhite(g,x) (iscollectable(x) && iswhite((g),gcvalue(x)))


/*
** Protected access to objects in values
*/
#define gcvalueN(o)     (iscollectable(o) ? gcvalue(o) : NULL)


/*
** Access to collectable objects in array part of tables
*/
#define gcvalarr(a,i)  \
	((*arraytag(a,i) & BIT_ISCOLLECTABLE) ? arrayval(a,i)->gc : NULL)


#define markvalue(g,o) { checkliveness(mainthread(g),o); \
  if (valiswhite(g,o)) reallymarkobject(g,gcvalue(o)); }

#define markobject(g,t) { if (iswhite(g,t)) reallymarkobject(g, obj2gco(t)); }

/*
** mark an object that can be NULL (either because it is really optional,
** or it was stripped as debug info, or inside an uncompleted structure)
*/
#define markobjectN(g,t)	{ if (t) markobject(g,t); }


static void reallymarkobject (global_State *g, GCObject *o);
static int claimmark (global_State *g, GCObject *o);
static int atomic (lua_State *L);
static lu_mem tracework (lua_State *L, uint64_t epoch, lu_mem budget,
                         int complete);
static lu_mem sweepwork (lua_State *L, lu_mem budget, int complete);
static void waitcollection (lua_State *L);
static void entersweep (lua_State *L);
static void setidledebt (global_State *g);
static int scantrace (lua_State *L, LuaTraceWork *work, GCObject *object,
                       int quiescent);
static void finishyoung (lua_State *L, global_State *g, l_mem marked);
static void atomic2gen (lua_State *L, global_State *g);
static int checkmajorminor (lua_State *L, global_State *g);



/*
** {======================================================
** Generic functions
** =======================================================
*/


/*
** one after last element in a hash array
*/

l_mem luaC_objsize (GCObject *o) {
  lu_mem res;
  switch (o->tt) {
    case LUA_VTABLE: {
      res = luaH_size(gco2t(o), gco2t(o));
      break;
    }
    case LUA_VLCL: {
      LClosure *cl = gco2lcl(o);
      res = sizeLclosure(cl->nupvalues);
      break;
    }
    case LUA_VCCL: {
      CClosure *cl = gco2ccl(o);
      res = sizeCclosure(cl->nupvalues);
      break;
    }
    case LUA_VUSERDATA: {
      Udata *u = gco2u(o);
      res = sizeudata(u->nuvalue, u->len);
      break;
    }
    case LUA_VPROTO: {
      res = luaF_protosize(gco2p(o));
      break;
    }
    case LUA_VROOTSNAPSHOT: {
      res = luaC_rootsnapshotbytes(gco2roots(o)->capacity);
      break;
    }
    case LUA_VTHREAD: {
      res = luaE_threadsize(gco2th(o));
      break;
    }
    case LUA_VSHRSTR: {
      TString *ts = gco2ts(o);
      res = sizestrshr(cast_uint(ts->shrlen));
      break;
    }
    case LUA_VLNGSTR: {
      TString *ts = gco2ts(o);
      res = luaS_sizelngstr(ts->u.lnglen, ts->shrlen);
      break;
    }
    case LUA_VUPVAL: {
      res = sizeof(UpVal);
      break;
    }
    default: res = 0; lua_assert(0);
  }
  return cast(l_mem, res);
}


/*
** Clear keys for empty entries in tables. If entry is empty, mark its
** entry as dead. This allows the collection of the key, but keeps its
** entry in the table: its removal could break a chain and could break
** a table traversal.  Other places never manipulate dead keys, because
** its associated empty value is enough to signal that the entry is
** logically empty.
*/
static void clearkey (Node *n) {
  lua_assert(isempty(gval(n)));
  if (keyiscollectable(n))
    setdeadkey(n);  /* unused key; remove it */
}


/*
** tells whether a key or value can be cleared from a weak
** table. Non-collectable objects are never removed from weak
** tables. Strings behave as 'values', so are never removed too. for
** other objects: if really collected, cannot keep them; for objects
** being finalized, keep them in keys, but not in values
*/
static void markwork (LuaTraceWork *work, GCObject *object);

static int iscleared (LuaTraceWork *work, const GCObject *o) {
  if (o == NULL) return 0;  /* non-collectable value */
  else if (novariant(o->tt) == LUA_TSTRING) {
    markwork(work, obj2gco(o));  /* strings are values, never weak */
    return 0;
  }
  else return iswhite(work->global, o);
}


/* Mark work can be produced by any execution. Claiming a white object is
** monotonic during tracing; a producer never reads mutable object payloads.
** Its participation covers the claim-to-publication gap at termination. */
void luaC_traceobject (lua_State *L, GCObject *o) {
  if (!luaC_tracing(L)) return;
  if (!claimmark(G(L), o)) return;
  /* A new ephemeron round that observes this claim must also see its colour. */
  __atomic_fetch_add(&G(L)->trace_changes, 1, __ATOMIC_RELEASE);
  luaC_publishbarrier(L, o, 0);
}

/* Marking shades new edges and the owner's age claims its remembered link.
** Neither obligation substitutes for the other. Participation covers both.
** No allocation, ownership wait or safepoint occurs on this path. */
void luaC_syncbarrier (lua_State *L, GCObject *o, GCObject *v) {
  global_State *g = G(L);
  lu_byte marked, claimed;
  int tracing = luaC_tracing(L);
  if (tracing) luaC_traceobject(L, v);
  if (luaC_kind(g) == KGC_GENMINOR) {
    unsigned touched = tracing ? G_TOUCHED0 : G_TOUCHED1;
    unsigned age;
    if (isold(v)) return;
    marked = getmarked(o);
    do {
      age = marked & AGEBITS;
      if (age <= G_SURVIVAL_CLEAN) {
        if (o->tt != LUA_VTABLE || age == G_NEW || age == G_SURVIVAL_DIRTY ||
            (age == G_SURVIVAL && !tracing)) return;
        /* A writer and the marker compete on the same age byte. A younger
        ** edge vetoes the proof even when it precedes the marker's scan. */
        claimed = cast_byte((marked & ~AGEBITS) | G_SURVIVAL_DIRTY);
      }
      else {
        if (age == touched) return;
        claimed = cast_byte((marked & ~AGEBITS) | touched |
                            bitmask(QUEUEDBIT));
      }
    } while (!__atomic_compare_exchange_n(&o->marked, &marked, claimed, 0,
                                          __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    /* A queued owner already owns its link, including private scan/sweep
    ** batches. Only a fresh claim publishes another remembered root. */
    if (age > G_SURVIVAL_CLEAN) {
      luaC_noteyoung(g);
      if (!testbit(marked, QUEUEDBIT)) luaC_publishbarrier(L, o, 1);
    }
    return;
  }
}


void luaC_fix (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  GCObject **link;
  GCObject *previous = NULL;
  lua_assert(o->tt != LUA_VTABLE);
  luaW_heapenter(L);
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  link = &L->new_objects;
  while (*link != NULL && *link != o) {
    previous = *link;
    link = luaC_alloclink(*link);
  }
  if (*link == o) {
    if (L->new_objects_tail == o) L->new_objects_tail = previous;
  }
  else {
    link = &g->allgc;
    lua_assert(*link == o);  /* initialization's already-published head */
  }
  setmarked(o, cast_byte((getmarked(o) & ~MAJORBITS) | bitmask(QUEUEDBIT)));
  setage(o, G_OLD);  /* and old forever */
  if (luaC_sweeping(L) && G(L)->sweep_plan.append == luaC_alloclink(o))
    G(L)->sweep_plan.append = link;
  *link = luaC_allocnext(o);
  o->next = g->fixedgc;  /* link it to 'fixedgc' list */
  g->fixedgc = o;
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  luaW_heapleave(L);
}


/* The caller owns the new list. Outside quiescence it joins the current
** coroutine's construction list until the collector can publish it. */
static void publishobjects (lua_State *L, GCObject *head, GCObject *tail,
                            GCObject **link, int collector) {
  global_State *g = G(L);
  luaC_noteyoung(g);
  if (collector) {
    *link = g->allgc;
    if (luaC_sweeping(L) && g->sweep_plan.append == &g->allgc)
      g->sweep_plan.append = link;
    g->allgc = head;
  }
  else {
    *link = L->new_objects;
    if (L->new_objects == NULL) {
      luaW_trackpublisher(L);
      L->new_objects_tail = tail;
    }
    L->new_objects = head;
  }
}

void luaC_publishobjects (lua_State *L, GCObject *head, GCObject *tail) {
  publishobjects(L, head, tail, luaC_alloclink(tail), luaW_iscollector(L));
}

static void publishslot (lua_State *L, LuaAllocBlock *block, LuaAllocPage *page) {
  uint64_t epoch = G(L)->sweep_plan.epoch;
  uintptr_t flags = LUA_BLOCK_COLLECTABLE;
  luaC_noteyoung(G(L));
  if (luaC_sweeping(L)) flags |= luaMP_birth(epoch);
  __atomic_store_n(&block->block.page, cast(uintptr_t, page) | flags,
                   __ATOMIC_RELEASE);
  luaMP_publishslot(L, page, epoch + 1);
}

GCObject *luaC_trytasktable (lua_State *L, size_t size, int refill) {
  GCObject *o;
  if (L->task_owner == NULL || !luaC_tasktableeligible(L, size) ||
      (refill && luaW_iscollector(L))) return NULL;
  o = luaM_trytasktable(L, size, refill);
  if (o != NULL) {
    int owned;
    setmarked(o, LUA_TASK_PAGE_MARK);
    o->tt = LUA_VTABLE;
    o->next = NULL;
    owned = luaC_taskallocate(L, o, size);
    lua_assert(owned);
    UNUSED(owned);
  }
  return o;
}

static GCObject *newobjdt (lua_State *L, lu_byte tt, size_t sz, size_t offset,
                           int tasklocal) {
  global_State *g = G(L);
  char *p;
  GCObject *o;
  int collector;
  if (tt == LUA_VTABLE && tasklocal) {
    o = luaC_trytasktable(L, sz, 1);
    if (o != NULL) return o;
  }
  p = cast_charp(luaM_newobject(L, novariant(tt), sz));
  o = cast(GCObject *, p + offset);
  setmarked(o, luaC_newmark(g));
  o->tt = tt;
  o->next = NULL;
  if (tt == LUA_VLNGSTR) gco2ts(o)->extra = 0;
  collector = luaW_iscollector(L);
  if (tt == LUA_VTABLE) {
    LuaAllocBlock *block = cast(LuaAllocBlock *, p) - 1;
    LuaAllocPage *page = luaMP_page(block);
    lua_assert(page != NULL && offset == 0);
    if (tasklocal && !collector && L->task_owner != NULL &&
        luaC_taskallocate(L, o, sz))
      return o;
    gco2t(o)->gcstate = collector ? 0 : L->nursery_epoch;
    if (!collector) {
      if (L->new_tables == NULL) luaW_trackpublisher(L);
      luaC_allocnext(o) = L->new_tables;
      L->new_tables = o;
      L->nursery_tables++;
    }
    publishslot(L, block, page);
    return o;
  }
  if (tt == LUA_VUSERDATA) {
    LuaAllocBlock *block = cast(LuaAllocBlock *, p) - 1;
    LuaAllocPage *page = luaMP_page(block);
    if (page != NULL) {
      lua_assert(offset == 0);
      publishslot(L, block, page);
      return o;
    }
  }
  if (tasklocal && !collector && L->task_owner != NULL &&
      luaC_taskallocate(L, o, sz)) return o;
  publishobjects(L, o, o, &(cast(LuaAllocBlock *, p) - 1)->block.link.object,
                 collector);
  return o;
}

GCObject *luaC_newobjdt (lua_State *L, lu_byte tt, size_t sz, size_t offset) {
  return newobjdt(L, tt, sz, offset, 0);
}

GCObject *luaC_newtable (lua_State *L, size_t size, int tasklocal) {
  return newobjdt(L, LUA_VTABLE, size, 0, tasklocal);
}

GCObject *luaC_newresultstring (lua_State *L, size_t size) {
  return newobjdt(L, LUA_VLNGSTR, size, 0, size <= LUA_ALLOC_MAX);
}

/*
** create a new collectable object with no offset.
*/
GCObject *luaC_newobj (lua_State *L, lu_byte tt, size_t sz) {
  return luaC_newobjdt(L, tt, sz, 0);
}

/* }====================================================== */



/*
** {======================================================
** Mark functions
** =======================================================
*/


/*
** Claim the one publication link for a white object. Mutators and parallel
** scanners compete through this same monotonic color transition. A winner
** publishes either to its local batch or to a shared root list.
*/
static int claimmark (global_State *g, GCObject *o) {
  unsigned state = luaC_markstate(g);
  lu_byte marked, claimed;
  if (luaC_taskobject(o)) return 0;
  marked = getmarked(o);
  do {
    if (!luaC_unmarked(state, marked)) return 0;
    claimed = cast_byte(luaC_stamp(state, marked) | bitmask(QUEUEDBIT));
  } while (!__atomic_compare_exchange_n(&o->marked, &marked, claimed, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
  return 1;
}

lu_byte luaC_materializemark (const GCObject *o) {
  LuaAllocBlock *block = cast(LuaAllocBlock *, o) - 1;
  LuaAllocPage *page = luaMP_page(block);
  Table *table = gco2t(cast(GCObject *, o));
  lu_byte mark = LUA_TASK_PAGE_MARK;
  uint64_t state;
  lua_assert(o->tt == LUA_VTABLE && page != NULL);
  if (__atomic_load_n(&page->task_private, __ATOMIC_ACQUIRE)) return 0;
  /* Retire the nursery state without overwriting a weak mode installed by
  ** another owner. All competing first readers use the same page mark. */
  state = __atomic_load_n(&table->gcstate, __ATOMIC_RELAXED);
  if ((state & LUA_TASK_PAGE_STATE) == LUA_TASK_PAGE_STATE)
    __atomic_compare_exchange_n(&table->gcstate, &state, 0, 0,
                                 __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  if (__atomic_compare_exchange_n(&table->marked, &mark, page->task_mark, 0,
                                   __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    return page->task_mark;
  return mark;
}

static void reallymarkobject (global_State *g, GCObject *o) {
  if (claimmark(g, o)) {
    if (luaC_markstate(g) & LUA_MARK_ACTIVE)
      __atomic_fetch_add(&g->trace_changes, 1, __ATOMIC_RELAXED);
    luaC_linktrace(g, o);
  }
}

/* Shared root/barrier publication owns world.mutex or quiescence. Strings
** are immutable leaves; all other payloads are queued for a scanner. */
void luaC_linktrace (global_State *g, GCObject *o) {
  if (novariant(o->tt) == LUA_TSTRING) {
    g->GCmarked += luaC_objsize(o);
    nw2black(o);
  }
  else if (o->tt == LUA_VTABLE && (luaC_markstate(g) & LUA_MARK_ACTIVE))
    luaC_queuetracepage(g, o);
  else {
    *luaC_graylink(o) = g->gray;
    g->gray = o;
  }
}

static void queuework (LuaTraceList *list, GCObject *object) {
  GCObject **next = luaC_graylink(object);
  if (list->head == NULL) list->tail = next;
  *next = list->head;
  list->head = object;
}

static void linkwork (LuaTraceList *list, GCObject *object) {
  lua_assert(!isgray(object));
  queuework(list, object);
  set2gray(object);
}

static void markwork (LuaTraceWork *work, GCObject *object) {
  if (!claimmark(work->global, object)) return;
  work->claims++;
  if (novariant(object->tt) == LUA_TSTRING) {
    work->marked += luaC_objsize(object);
    nw2black(object);
  }
  else if (work->local_remaining != 0) {
    work->local_remaining--;
    *luaC_graylink(object) = work->local;
    work->local = object;
  }
  else if (object->tt == LUA_VTABLE) luaC_stagetracepage(work, object);
  else queuework(&work->gray, object);
}

static void __attribute__((noinline))
scanrootsnapshot (LuaTraceWork *work, LuaRootSnapshot *snapshot) {
  enum { SNAPSHOT_BATCH = 512 };
  const uintptr_t *roots = cast(const uintptr_t *, snapshot + 1);
  unsigned i, first = snapshot->next_root;
  unsigned end = first + SNAPSHOT_BATCH;
  if (end > snapshot->count) end = snapshot->count;
  for (i = first; i < end; i++)
    markwork(work, cast(GCObject *, roots[i] & ~LUA_TASK_ROOT_FLAGS));
  work->scanned += (end - first) * sizeof(uintptr_t);
  snapshot->next_root = end;
  if (end < snapshot->count) queuework(&work->gray, obj2gco(snapshot));
  else {
    work->marked += cast(l_mem, snapshot->marked_bytes);
    nw2black(snapshot);
  }
}

void luaC_traceedges (LuaTraceWork *work, GCObject **objects, unsigned count) {
  unsigned i;
  for (i = 0; i < count; i++) markwork(work, objects[i]);
}

/* Retain link ownership until both age consumption and the remembered-list
** decision are committed. A writer either updates the age of our queued
** owner, or claims a released link; it can never publish our private link. */
void luaC_finishstrong (LuaTraceWork *work, GCObject *object,
                        int old_edges) {
  lu_byte marked = getmarked(object), changed;
  int remember;
  do {
    unsigned age = marked & AGEBITS;
    if (luaC_kind(work->global) == KGC_GENMINOR) {
      if (old_edges && age == G_SURVIVAL) age = G_SURVIVAL_CLEAN;
      else if (age == G_TOUCHED0) age = G_TOUCHED1;
      else if (age == G_TOUCHED1) age = G_TOUCHED2;
      else if (age == G_TOUCHED2) age = G_OLD;
    }
    remember = luaC_kind(work->global) == KGC_GENMINOR &&
               (age == G_TOUCHED1 || age == G_TOUCHED2);
    changed = cast_byte((marked & ~AGEBITS) | age);
    if (!remember) changed &= cast_byte(~bitmask(QUEUEDBIT));
  } while (!__atomic_compare_exchange_n(&object->marked, &marked, changed, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  if (remember) queuework(&work->grayagain, object);
}

#define tracevalue(w,v) \
  { if (iscollectable(v)) markwork(w, gcvalue(v)); }
#define tracekey(w,n) \
  { if (keyiscollectable(n)) markwork(w, gckey(n)); }
#define traceobjectN(w,o) \
  { if ((o) != NULL) markwork(w, obj2gco(o)); }


/*
** mark metamethods for basic types
*/
static void markmt (global_State *g) {
  int i;
  LuaTaskRoot *task;
  LuaParticipation *participant;
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  for (participant = g->world.participants; participant != NULL;
       participant = participant->next) {
    lua_State *thread = participant->thread;
    if (participant->collector ||
        __atomic_load_n(&thread->owned_operations, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&thread->operation_roots, __ATOMIC_ACQUIRE) != 0)
      markobject(g, thread);
  }
  for (task = g->tasks; task != NULL; task = task->next)
    markobject(g, task->thread);
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  {
    Table *pipe = __atomic_load_n(&g->pipe_meta, __ATOMIC_ACQUIRE);
    markobjectN(g, pipe);
  }
  for (i=0; i < LUA_NUMTYPES; i++) {
    Table *mt = __atomic_load_n(&g->mt[i], __ATOMIC_ACQUIRE);
    markobjectN(g, mt);
  }
}


static void cleargraylists (global_State *g) {
  lua_assert(g->trace_stacks == NULL && !luaC_pagetracepending(g));
  g->gray = g->grayagain = NULL;
  g->weak = g->allweak = g->ephemeron = NULL;
  g->trace_deferred = NULL;
}

void luaC_waittrace (lua_State *L) {
  if (luaC_tracing(L)) {
    LuaGCFrame frame;
    luaC_entergc(L, &frame);
    luaW_stop(L);
    if (luaC_tracing(L)) atomic(L);
    luaW_start(L);
    luaC_leavegc(L, &frame);
  }
}

/* A packed weak stamp has fewer bits than the trace epoch. On wrap,
** invalidate old caches at the existing quiescent trace boundary. */
static void resetweakstamps (global_State *g) {
  unsigned last = luaMP_sizeclass(LUA_TABLE_INLINE_LIMIT);
  unsigned pool;
  for (pool = luaMP_sizeclass(sizeof(Table)); pool <= last; pool++) {
    LuaAllocPage *page;
    size_t stride = sizeof(LuaAllocBlock) + luaMP_binbytes(pool);
    for (page = g->sweep_pages[pool]; page != NULL; page = page->sweep_next) {
      size_t offset, limit = luaMP_slotend(page);
      if (page->state != LUA_PAGE_OPEN || page->sweep_epoch == 0) continue;
      for (offset = LUA_ALLOC_PAGE_HEADER; offset + stride <= limit;
           offset += stride) {
        LuaAllocBlock *block = cast(LuaAllocBlock *, cast_charp(page) + offset);
        Table *table = cast(Table *, block + 1);
        if ((luaMP_header(block) & LUA_BLOCK_COLLECTABLE) &&
            table->tt == LUA_VTABLE &&
            !(luaC_tablestate(table) & NURSERY_TAG))
          luaC_settablestate(table, 0);
      }
    }
  }
  __atomic_store_n(&g->weak_stamp, LUA_WEAK_STAMP_STEP, __ATOMIC_RELAXED);
}

static void publishmarkstate (LuaWorld *world, unsigned mark) {
  uint64_t control = __atomic_load_n(&world->control, __ATOMIC_RELAXED);
  uint64_t next;
  lua_assert(!(mark & ~LUA_WORLD_MARK_MASK));
  do {
    next = (control & ~cast(uint64_t, LUA_WORLD_MARK_MASK)) | mark;
  } while (!__atomic_compare_exchange_n(&world->control, &control, next, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

enum { FINALIZER_ROOT_SCAN = 1 };

static GCObject *finalizerhead (uintptr_t head) {
  return cast(GCObject *, head & ~cast(uintptr_t, FINALIZER_ROOT_SCAN));
}

/* Close the dequeue fast path before taking a root cursor. A pop that
** already changed the head belongs to an admitted invocation. Its root
** is covered by the following stack rendezvous. */
static void beginpendingfinalizers (global_State *g) {
  uintptr_t head;
  luaX_checksync(pthread_mutex_lock(&g->trace_mutex));
  head = __atomic_load_n(&g->tobefnz, __ATOMIC_ACQUIRE);
  lua_assert(!(head & FINALIZER_ROOT_SCAN));
  while (head != 0 &&
         !__atomic_compare_exchange_n(&g->tobefnz, &head,
              head | FINALIZER_ROOT_SCAN, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
  g->trace_plan.pending = finalizerhead(head);
  luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
}

static void starttrace (lua_State *L, unsigned finish, int user_incremental) {
  global_State *g = G(L);
  unsigned stamps = luaC_markstate(g) & MARKSTAMPS;
  int arming = L->world_depth == 0;
  if ((g->gcstp & GCSTPCLS) && !g->gcemergency) return;
  lua_assert((!arming || (finish != TRACE_GENERATIONAL && luaW_ownsheap(L))) &&
             !luaC_tracing(L) && !luaC_sweeping(L));
  lua_assert(!arming || finish == TRACE_YOUNG ||
             !(luaC_markstate(g) & LUA_MARK_YOUNG));
  /* Stop publishing weak closure-cache entries before their epoch changes.
  ** Allocation and shading switch together once the plan is initialized. */
  if (arming)
    __atomic_fetch_or(&g->world.control, LUA_MARK_ARMING, __ATOMIC_RELEASE);
  if (finish == TRACE_YOUNG) {
    stamps ^= bitmask(MINORBIT);
    stamps |= LUA_MARK_YOUNG;
    /* The preceding sweep left queued roots for the new epoch. Make them
    ** available alongside stack roots at the initial trace boundary. */
    lua_assert(g->gray == NULL && !luaC_pagetracepending(g));
    g->gray = g->grayagain;
    g->grayagain = NULL;
  }
  else {
    unsigned major = stamps & MAJORBITS;
    cleargraylists(g);
    g->GCmarked = 0;
    stamps = (stamps & ~MAJORBITS) |
      (major == MAJORBITS ? bitmask(MAJOR0BIT) : major + bitmask(MAJOR0BIT));
    if (finish == TRACE_GENERATIONAL || luaC_kind(g) == KGC_GENMAJOR)
      stamps |= G_OLD;
  }
  __atomic_fetch_add(&g->trace_epoch, 1, __ATOMIC_RELEASE);
  if (l_unlikely(__atomic_add_fetch(&g->weak_stamp, LUA_WEAK_STAMP_STEP,
                                    __ATOMIC_RELAXED) == 0)) {
    lua_assert(!arming);
    resetweakstamps(g);
  }
  __atomic_store_n(&g->trace_weakcutoff, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g->trace_changes, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g->trace_ready, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g->trace_workers, 0, __ATOMIC_RELAXED);
  g->trace_deferred = NULL;
  g->trace_plan.again = g->trace_plan.next = NULL;
  g->trace_plan.deferred = g->trace_plan.tail = NULL;
  beginpendingfinalizers(g);
  g->trace_plan.finalizers = g->finobj != g->finobjold1 ? &g->finobj : NULL;
  g->trace_plan.finalizer_gray = NULL;
  g->trace_plan.finalizer_tail = NULL;
  g->trace_plan.finalizer_new = NULL;
  g->trace_plan.finalizer_newtail = &g->trace_plan.finalizer_new;
  g->trace_plan.base_marked = g->GCmarked;
  __atomic_store_n(&g->trace_plan.phase, TRACE_INIT, __ATOMIC_RELAXED);
  g->trace_plan.finish = finish;
  g->trace_plan.roots_pending = arming;
  g->trace_plan.user_incremental = user_incremental;
  publishmarkstate(&g->world,
    stamps | LUA_MARK_ACTIVE | (arming ? LUA_MARK_ARMING : 0));
  if (arming) {
    /* Prepare the root queue before workers can complete the handshake. */
    luaC_initialtrace(L);
    luaX_checksync(pthread_mutex_lock(&g->trace_mutex));
    luaC_readytrace(g);
    luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
    luaW_armtrace(L);
  }
  else luaC_publishtraceplan(g);
}

void luaC_publishtraceplan (global_State *g) {
  /* The arm rendezvous or world stop has settled older allocation batches.
  ** The sweep can subtract that cohort. */
  luaP_begin(g);
  g->GCcyclelive = g->pacer.start;
  __atomic_fetch_add(&g->world.work_epoch, 1, __ATOMIC_RELEASE);
  __atomic_fetch_and(&g->world.control, ~cast(uint64_t, LUA_MARK_ARMING),
                     __ATOMIC_RELEASE);
}

static void endtrace (lua_State *L) {
  global_State *g = G(L);
  lua_assert(L->world_depth != 0 && g->trace_stacks == NULL &&
             !__atomic_load_n(&g->trace_busy, __ATOMIC_ACQUIRE));
  __atomic_fetch_and(&g->world.control, ~cast(uint64_t, LUA_MARK_ACTIVE),
                   __ATOMIC_RELEASE);
  g->trace_weakcutoff = 0;
  luaX_checksync(pthread_mutex_lock(&g->trace_mutex));
  luaQ_wake(&g->trace_changed, &g->trace_waiters);
  luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
}


/*
** mark root set and reset all gray lists, to start a new collection.
** 'GCmarked' is initialized to count the total number of live bytes
** during a cycle.
*/
static void marknativeframes (global_State *g) {
  LuaParticipation *participant;
  LuaCallback *callback;
  lua_State *reclaimer;
  for (participant = g->world.participants; participant != NULL;
       participant = participant->next)
    if (participant->thread->native_depth != 0)
      markobject(g, participant->thread);
  for (reclaimer = __atomic_load_n(&g->world.reclaimers, __ATOMIC_RELAXED);
       reclaimer != NULL; reclaimer = reclaimer->reclaim_next)
    markobject(g, reclaimer);  /* shared queues outlive a suspended driver */
  for (callback = g->callbacks; callback != NULL; callback = callback->next) {
    markvalue(g, &callback->function);
    markobject(g, callback->thread);
    markobject(g, callback->close_error);
  }
}

static void restartcollection (lua_State *L, global_State *g, unsigned finish) {
  starttrace(L, finish, 0);
  markobject(g, mainthread(g));
  marknativeframes(g);
  markvalue(g, &g->l_registry);
  markmt(g);
}

/* }====================================================== */


/*
** {======================================================
** Traverse functions
** =======================================================
*/


/*
** A remembered owner covers the next two young traces. A marking-time write
** also retains the current trace, which cannot age a black newborn. Advance
** its age only after scanning its payload, while tracing barriers cover
** subsequent writes.
** Sweeping cannot consume an age: a concurrent write may refer to a NEW
** allocation outside that sweep's cohort, which still needs both traces.
*/
static void genlink (LuaTraceWork *work, GCObject *o) {
  lua_assert(isblack(work->global, o));
  if (luaC_kind(work->global) == KGC_GENMINOR &&
      (getage(o) == G_TOUCHED0 || getage(o) >= G_TOUCHED1))
    linkwork(&work->grayagain, o);
}

/* Advance remembered age once per payload scan, including weak tables.
** Convergence passes keep their queue but cannot consume another age. */
static void blacken (LuaTraceWork *work, GCObject *o) {
  nw2black(o);
  if (luaC_kind(work->global) == KGC_GENMINOR) {
    unsigned age = getage(o);
    if (age == G_TOUCHED0) setage(o, G_TOUCHED1);
    else if (age == G_TOUCHED1) setage(o, G_TOUCHED2);
    else if (age == G_TOUCHED2) setage(o, G_OLD);
  }
}


/*
** Traverse a table with weak values and link it to proper list. During
** propagate phase, keep it in 'grayagain' list, to be revisited in the
** atomic phase. In the atomic phase, if table has any white value,
** put it in 'weak' list, to be cleared; otherwise, call 'genlink'
** to check table age in generational mode.
*/
static void traverseweakvalue (LuaTraceWork *work, Table *h) {
  global_State *g = work->global;
  unsigned p, parts = luaH_hashparts(h);
  /* if there is array part, assume it may have white values (it is not
     worth traversing it now just to check) */
  int hasclears = luaH_arrayslots(h) > 0 || luaH_recordkeys(h->node) != NULL;
  for (p = 0; p < parts; ++p) {
    Node *n = luaH_hashnodes(h, p), *limit = n + luaH_nodesize(n);
    for (; n < limit; n++) {
      if (isempty(gval(n)))
        clearkey(n);
      else {
        lua_assert(!keyisnil(n));
        tracekey(work, n);
        if (!hasclears && iscleared(work, gcvalueN(gval(n))))
          hasclears = 1;
      }
    }
  }
  if (luaC_state(g) == GCSpropagate)
    linkwork(&work->grayagain, obj2gco(h));
  else if (hasclears)
      linkwork(&work->weak, obj2gco(h));
  else
    genlink(work, obj2gco(h));
}


/*
** Traverse the array part of a table.
*/
static int traversearray (LuaTraceWork *work, Table *h) {
  int marked = 0;  /* true if some object is marked in this traversal */
  unsigned p, parts = luaH_hashparts(h);
  for (p = 0; p < parts; p++) {
    unsigned block;
    Value *array;
    for (block = 0; (array = luaH_arrayblock(h, p, block)) != NULL; ++block) {
      unsigned i, asize = luaH_arraycapacity(array);
      for (i = 0; i < asize; i++) {
        GCObject *o = gcvalarr(array, i);
        if (o != NULL && iswhite(work->global, o)) {
          marked = 1;
          markwork(work, o);
        }
      }
    }
  }
  return marked;
}

static int traverserecord (LuaTraceWork *work, Table *h, Table *keys) {
  uintptr_t image = h->arraydata;
  uintptr_t tags = luaH_recordtags(image);
  unsigned i;
  int marked = 0;
  for (i = 0; i < keys->asize; i++) {
    lu_byte tag = luaH_recordtag(tags, i);
    if (tag == LUA_RECORD_WIDE || (tag & BIT_ISCOLLECTABLE)) {
      TValue value = {0};
      tag = luaH_ownedrecordvalue(h, image, i, tag, &value);
      if ((tag & BIT_ISCOLLECTABLE) && iswhite(work->global, gcvalue(&value))) {
        markwork(work, gcvalue(&value));
        marked = 1;
      }
    }
  }
  return marked;
}


/*
** Traverse an ephemeron table and link it to proper list. Returns true
** iff any object was marked during this traversal (which implies that
** convergence has to continue). During propagation phase, keep table
** in 'grayagain' list, to be visited again in the atomic phase. In
** the atomic phase, if table has any white->white entry, it has to
** be revisited during ephemeron convergence (as that key may turn
** black). Otherwise, if it has any white key, table has to be cleared
** (in the atomic phase). In generational mode, some tables
** must be kept in some gray list for post-processing; this is done
** by 'genlink'.
*/
static int traverseephemeron (LuaTraceWork *work, Table *h, int inv) {
  global_State *g = work->global;
  int hasclears = 0;  /* true if table has white keys */
  int hasww = 0;  /* true if table has entry "white-key -> white-value" */
  unsigned p, parts = luaH_hashparts(h);
  Table *keys = luaH_recordkeys(h->node);
  /* Compact records have only short-string keys, which are never weak. */
  int marked = keys != NULL ? traverserecord(work, h, keys)
                            : traversearray(work, h);
  /* Alternate direction between convergence rounds to shorten long chains. */
  for (p = 0; p < parts; ++p) {
    Node *nodes = luaH_hashnodes(h, inv ? parts - 1 - p : p);
    unsigned i, size = luaH_nodesize(nodes);
    for (i = 0; i < size; i++) {
      Node *n = nodes + (inv ? size - 1 - i : i);
      if (isempty(gval(n)))
        clearkey(n);
      else if (iscleared(work, gckeyN(n))) {
        hasclears = 1;
        if (valiswhite(g, gval(n))) hasww = 1;
      }
      else if (valiswhite(g, gval(n))) {
        marked = 1;
        markwork(work, gcvalue(gval(n)));
      }
    }
  }
  /* link table into proper list */
  if (luaC_state(g) == GCSpropagate)
    linkwork(&work->grayagain, obj2gco(h));
  else if (hasww)  /* table has white->white entries? */
    linkwork(&work->ephemeron, obj2gco(h));
  else if (hasclears)  /* table has white keys? */
    linkwork(&work->allweak, obj2gco(h));
  else
    genlink(work, obj2gco(h));
  return marked;
}


static void traversestrongtable (LuaTraceWork *work, Table *h) {
  unsigned p, parts = luaH_hashparts(h);
  Table *keys = luaH_recordkeys(h->node);
  if (keys != NULL) {
    traverserecord(work, h, keys);
    genlink(work, obj2gco(h));
    return;
  }
  traversearray(work, h);
  for (p = 0; p < parts; ++p) {
    Node *n = luaH_hashnodes(h, p), *limit = n + luaH_nodesize(n);
    for (; n < limit; n++) {
      if (isempty(gval(n)))
        clearkey(n);
      else {
        lua_assert(!keyisnil(n));
        tracekey(work, n);
        tracevalue(work, gval(n));
      }
    }
  }
  genlink(work, obj2gco(h));
}


/*
** (result & 1) iff weak values; (result & 2) iff weak keys.
*/
static int cachemode (global_State *g, Table *h, const TValue *mode) {
  int result;
  if (mode == NULL || !ttisstring(mode))
    result = 0;  /* ignore non-string modes */
  else {
    const char *smode = getstr(tsvalue(mode));
    const char *weakkey = strchr(smode, 'k');
    const char *weakvalue = strchr(smode, 'v');
    result = ((weakkey != NULL) << 1) | (weakvalue != NULL);
  }
  if (luaC_markstate(g) & LUA_MARK_ACTIVE) {
    unsigned cutoff = __atomic_load_n(&g->trace_weakcutoff, __ATOMIC_RELAXED);
    uint64_t state = luaC_weakstamp(g) |
      cast(uint64_t, result << LUA_WEAK_MODE_SHIFT);
    if (cutoff & 1) state |= LUA_WEAK_VALUES_LATE;
    luaC_settablestate(h, state);
  }
  return result;
}

static int getmode (global_State *g, Table *h) {
  TValue mode;
  if (luaC_markstate(g) & LUA_MARK_ACTIVE) {
    uint64_t state = luaC_tablestate(h);
    if (luaC_weakvalid(g, state)) return cast_int(luaC_weakmode(state));
  }
  return cachemode(g, h, luaT_gettmunlocked(h->metatable, TM_MODE,
                                           g->tmname[TM_MODE], &mode));
}

/* The table owner freezes its mode; its metatable only needs a validated read.
** Collectors must not claim an otherwise private, strong metatable. */
static int preparemode (lua_State *L, Table *h) {
  TValue mode;
  Table *meta = h->metatable;
  if (luaC_weakvalid(G(L), luaC_tablestate(h)))
    return 1;
  if (meta == NULL) setnilvalue(&mode);
  else if (!luaH_readshortstr(G(L), NULL, meta, G(L)->tmname[TM_MODE], &mode)) {
    if (luaA_lockowner(&meta->access) != luaX_token(L))
      return 0;
    /* A nested collection borrows this execution's stable partial image. */
    getmode(G(L), h);
    return 1;
  }
  cachemode(G(L), h, &mode);
  return 1;
}


static l_mem traversetable (LuaTraceWork *work, Table *h) {
  global_State *g = work->global;
  Table *keys = luaH_recordkeys(h->node);
  traceobjectN(work, h->metatable);
  if (keys != NULL) markwork(work, obj2gco(keys));
  switch (getmode(g, h)) {
    case 0:  /* not weak */
      traversestrongtable(work, h);
      break;
    case 1:  /* weak values */
      traverseweakvalue(work, h);
      break;
    case 2:  /* weak keys */
      traverseephemeron(work, h, 0);
      break;
    case 3:  /* all weak; nothing to traverse */
      if (luaC_state(g) == GCSpropagate)
        linkwork(&work->grayagain, obj2gco(h));
      else
        linkwork(&work->allweak, obj2gco(h));
      break;
  }
  return keys != NULL ? 2 + keys->asize : luaH_tablework(h);
}


static void capturevalue (const TValue *value, GCObject **edges, unsigned *n) {
  lu_byte tag = __atomic_load_n(&value->tt_, __ATOMIC_ACQUIRE);
  if (tag & BIT_ISCOLLECTABLE)
    __atomic_load(&value->value_.gc, &edges[(*n)++], __ATOMIC_ACQUIRE);
}

/* Raw pointer representations only. Following them belongs after validation. */
unsigned luaC_objectchunk (GCObject *object, unsigned first, unsigned count,
                           GCObject **edges) {
  unsigned i, n = 0;
  if (object->tt == LUA_VLCL) {
    LClosure *closure = gco2lcl(object);
    if (first == 0) {
      Proto *proto = __atomic_load_n(&closure->p, __ATOMIC_ACQUIRE);
      if (proto != NULL) edges[n++] = cast(GCObject *, proto);
    }
    for (i = first; i < first + count; i++) {
      UpVal *cell = __atomic_load_n(&closure->upvals[i], __ATOMIC_ACQUIRE);
      if (cell != NULL) edges[n++] = cast(GCObject *, cell);
    }
  }
  else if (object->tt == LUA_VCCL) {
    CClosure *closure = gco2ccl(object);
    for (i = first; i < first + count; i++)
      capturevalue(&closure->upvalue[i], edges, &n);
  }
  else if (object->tt == LUA_VUSERDATA) {
    Udata *userdata = gco2u(object);
    if (first == 0) {
      Table *meta = __atomic_load_n(&userdata->metatable, __ATOMIC_ACQUIRE);
      if (meta != NULL) edges[n++] = cast(GCObject *, meta);
    }
    for (i = first; i < first + count; i++)
      capturevalue(&userdata->uv[i].uv, edges, &n);
  }
  else capturevalue(&gco2upv(object)->value, edges, &n);
  return n;
}

static l_mem traverseudata (LuaTraceWork *work, Udata *u) {
  int i;
  traceobjectN(work, u->metatable);
  for (i = 0; i < u->nuvalue; i++)
    tracevalue(work, &u->uv[i].uv);
  genlink(work, obj2gco(u));
  return 1 + u->nuvalue;
}


/*
** Traverse a prototype. (While a prototype is being build, its
** arrays can be larger than needed; the extra slots are filled with
** NULL, so the use of 'markobjectN')
*/


static l_mem traverseCclosure (LuaTraceWork *work, CClosure *cl) {
  int i;
  for (i = 0; i < cl->nupvalues; i++)  /* mark its upvalues */
    tracevalue(work, &cl->upvalue[i]);
  return 1 + cl->nupvalues;
}

/*
** Traverse a Lua closure, marking its prototype and its upvalues.
** (Both can be NULL while closure is being created.)
*/
static l_mem traverseLclosure (LuaTraceWork *work, LClosure *cl) {
  int i;
  traceobjectN(work, cl->p);
  for (i = 0; i < cl->nupvalues; i++) {  /* visit its upvalues */
    UpVal *uv = cl->upvals[i];
    traceobjectN(work, uv);
  }
  return 1 + cl->nupvalues;
}


/*
** Traverse a thread, marking the elements in the stack up to its top
** and cleaning the rest of the stack in the final traversal. That
** ensures that the entire stack have valid (non-dead) objects.
** Threads have no barriers. In gen. mode, old threads must be visited
** at every cycle, because they might point to young objects.  In inc.
** mode, the thread can still be modified before the end of the cycle,
** and therefore it must be visited again in the atomic phase. To ensure
** these visits, threads must return to a gray list if they are not new
** (which can only happen in generational mode) or if the traverse is in
** the propagate phase (which can only happen in incremental mode).
*/



/*
** traverse one gray object, turning it to black. Return an estimate
** of the number of slots traversed.
*/

static int scantrace (lua_State *L, LuaTraceWork *work, GCObject *o,
                       int quiescent) {
  global_State *g = G(L);
  int reservation = 0;
  uintptr_t *sharing = luaA_sharing(o);
  if (sharing != NULL) {
    uintptr_t state = __atomic_load_n(sharing, __ATOMIC_ACQUIRE);
    GCObject *next = cast(GCObject *, state & ~LUA_OBJECT_SHARED);
    /* An unpublished promotion list is a temporary heap graph. Trace its
    ** links concurrently instead of walking it under root-phase ownership. */
    if ((state & LUA_OBJECT_SHARED) && next != NULL) markwork(work, next);
  }
  if (o->tt == LUA_VTHREAD) {
    lua_State *thread = gco2th(o);
    if (__atomic_load_n(&thread->trace_snapshot, __ATOMIC_ACQUIRE) !=
        luaC_traceepoch(g)) {
      int reservation = quiescent ? 2 : luaX_tryreserve(thread);
      if (reservation == 0) {
        if (!luaC_snapshotparked(L, thread)) return 0;
      }
      else {
        luaC_snapshot(L, thread);
        luaX_endreserve(thread, reservation);
      }
    }
    work->marked += cast(l_mem, thread->trace_bytes);
    blacken(work, o);
    if (luaC_kind(g) == KGC_GENMINOR && isold(o)) linkwork(&work->grayagain, o);
    return 1;
  }
  if (o->tt == LUA_VPROTO) {
    Proto *proto = gco2p(o);
    lua_State *builder = __atomic_load_n(&proto->builder, __ATOMIC_ACQUIRE);
    int reservation = builder != NULL && !quiescent
      ? luaX_tryreserve(builder) : 2;
    if (reservation == 0) {
      /* Even a cached snapshot needs stable ownership while consuming its
      ** remembered age. The parked builder cannot resume under this mutex. */
      luaX_checksync(pthread_mutex_lock(&g->world.mutex));
      if (!__atomic_load_n(&builder->root_waiting, __ATOMIC_RELAXED)) {
        luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
        return 0;
      }
    }
    if (builder != NULL) luaC_traceobject(L, obj2gco(builder));
    luaC_snapshotproto(L, proto);
    work->marked += cast(l_mem, proto->trace_bytes);
    blacken(work, o);
    genlink(work, o);
    if (reservation == 0)
      luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
    else if (builder != NULL) luaX_endreserve(builder, reservation);
    return 1;
  }
  if (l_unlikely(o->tt == LUA_VROOTSNAPSHOT)) {
    scanrootsnapshot(work, gco2roots(o));
    return 1;
  }
  if (!quiescent && (o->tt == LUA_VUPVAL || o->tt == LUA_VLCL ||
      o->tt == LUA_VCCL || o->tt == LUA_VUSERDATA)) {
    int scanned = luaC_scanvalues(L, work, o);
    if (scanned >= 0) return scanned;
  }
  if (o->tt == LUA_VUPVAL) {
    UpVal *upvalue = gco2upv(o);
    lua_assert(quiescent);
    tracevalue(work, &upvalue->value);
    work->marked += sizeof(UpVal);
    blacken(work, o);
    genlink(work, o);
    return 1;
  }
  if (o->tt == LUA_VTABLE && !quiescent) {
    int scanned = luaC_scanstrong(L, work, gco2t(o));
    if (scanned >= 0) return scanned;
  }
  if (!quiescent && !(reservation = luaA_tryscan(L, o))) return 0;
  if (o->tt == LUA_VTABLE) {
    Table *table = gco2t(o);
    if (reservation && !preparemode(L, table)) {
      luaA_endscan(L, o, reservation);
      return 0;
    }
    work->marked += luaC_objsize(o);
    blacken(work, o);
    traversetable(work, table);
  }
  else {
    work->marked += luaC_objsize(o);
    blacken(work, o);
    switch (o->tt) {
      case LUA_VUSERDATA: traverseudata(work, gco2u(o)); break;
      case LUA_VLCL:
        traverseLclosure(work, gco2lcl(o)); genlink(work, o); break;
      case LUA_VCCL:
        traverseCclosure(work, gco2ccl(o)); genlink(work, o); break;
      default: lua_assert(0); break;
    }
  }
  if (reservation) luaA_endscan(L, o, reservation);
  return 1;
}

/* Runnable payload work and phase admission are different resources. An
** empty phase cannot advance behind another metadata owner. Ordinary assists
** keep their debt and retry when either resource changes; no allocation-based
** backoff, lost credit or queued continuation belongs to this check. Producer
** barriers supply payload work even while the phase permit is occupied. */
static int traceavailable (lua_State *L) {
  return luaC_tracerunnable(G(L)) ||
    (__atomic_load_n(&G(L)->trace_busy, __ATOMIC_ACQUIRE) == 0 &&
     !luaC_waitingstacks(G(L)) &&
     !luaW_heapheld(&G(L)->world));
}

void luaC_assisttrace (lua_State *L) {
  global_State *g = G(L);
  LuaGCFrame frame;
  lu_mem budget = L->trace_debt;
  uint64_t epoch = luaC_traceepoch(g);
  int pressure = luaC_cyclepressure(L);
  if (!gcrunning(g) || (budget < LUA_GC_ASSIST_BYTES && !pressure) ||
      luaC_ingc(L) || L->reclaiming ||
      L->world_depth != 0 || L->object_scopes != NULL ||
      luaW_scope(L).heap != 0 || !luaC_tracing(L))
    return;
  /* At the hard limit an allocator must join the in-progress collection,
  ** including its startup handshake. A soft assist can simply retry. */
  if (!luaC_scanready(g) && !pressure) return;
  if (!pressure && !traceavailable(L) && !luaC_needassist(L)) return;
  luaC_entergc(L, &frame);
  /* Earlier cycles can still have physical garbage. A stream of new marking
  ** jobs must not monopolize every allocation assist and starve that storage. */
  if (luaC_needassist(L)) luaC_assistreclaim(L);
  if (pressure || traceavailable(L)) {
    if (pressure || L->assist_epoch ==
        __atomic_load_n(&g->world.work_epoch, __ATOMIC_ACQUIRE)) {
      lu_mem due = pressure ? budget :
        (luaC_scanready(g) ? luaP_budget(L, budget) : 0);
      if (pressure || due != 0) tracework(L, epoch, due, pressure);
      else L->trace_debt = 0;
    }
    else L->trace_debt = 0;
  }
  if (luaC_retired(L) != NULL || L->reclaim_pages) luaC_reclaim(L);
  luaC_leavegc(L, &frame);
}

/* Supply marking capacity even when applications stop allocating. This uses
** the VM's existing safepoints, never another carrier or a retained driver.
** One whole-object batch is the minimum slice; it owns no unpublished work
** when the application resumes. */
void luaC_polltrace (lua_State *L) {
  LuaGCFrame frame;
  if (!luaC_scanready(G(L)) || !gcrunning(G(L)) || luaC_ingc(L) || L->reclaiming ||
      L->world_depth != 0 || L->object_scopes != NULL ||
      luaW_scope(L).heap != 0 || !traceavailable(L))
    return;
  luaC_entergc(L, &frame);
  tracework(L, luaC_traceepoch(G(L)), 1, 0);
  if (luaC_retired(L) != NULL || L->reclaim_pages) luaC_reclaim(L);
  luaC_leavegc(L, &frame);
}



/* A claimed weak table retains its gclist for later weak phases. Its
** allocation link is idle here: the trace has published its cohort, and a
** table born during tracing is black and cannot enter these weak lists. */
static void deferweak (GCObject **head, GCObject **tail, GCObject *object) {
  luaC_allocnext(object) = NULL;
  if (*tail != NULL)
    luaC_allocnext(*tail) = object;
  else *head = object;
  *tail = object;
}

static GCObject *takeweak (GCObject **head, GCObject **tail) {
  GCObject *object = *head;
  *head = luaC_allocnext(object);
  if (*head == NULL) *tail = NULL;
  return object;
}



/* Claims/publication serialize list ownership, never payload scanning. A batch
** stays participating until its discoveries are published; stopping the world
** therefore also joins all scanners before a phase can change. */
/* A marked object cannot become a candidate in this trace. The cursor stays
** at the first possible candidate until strong and weak-value work is done. */
unsigned luaC_skiplivefinalizers (global_State *g, unsigned limit) {
  LuaTracePlan *plan = &g->trace_plan;
  unsigned count = 0;
  while (plan->finalizers != NULL && count < limit &&
         !iswhite(g, *plan->finalizers)) {
    GCObject *object = *plan->finalizers;
    plan->finalizers = object->next != g->finobjold1 ? &object->next : NULL;
    count++;
  }
  return count;
}

void luaC_readytrace (global_State *g) {
  LuaTracePlan *plan = &g->trace_plan;
  unsigned ready = g->gray != NULL || luaC_pagetracepending(g) || plan->next != NULL ||
    plan->pending != NULL || (plan->finalizers != NULL &&
      (plan->phase == TRACE_FINALIZERS || !iswhite(g, *plan->finalizers)));
  __atomic_store_n(&g->trace_workers, ready, __ATOMIC_RELEASE);
  __atomic_store_n(&g->trace_ready,
    ready || g->trace_deferred != NULL || plan->deferred != NULL,
    __ATOMIC_RELEASE);
}

/* Check admission hints without borrowing any stack storage. The scan still
** acquires the gate or world.mutex before reading a stack. An offered slot
** leaves the stack available to a collector. */
static int stackready (global_State *g, GCObject *object) {
  lua_State *thread = gco2th(object);
  return __atomic_load_n(&thread->trace_snapshot, __ATOMIC_SEQ_CST) ==
           luaC_traceepoch(g) ||
         __atomic_load_n(&thread->root_waiting, __ATOMIC_SEQ_CST) ||
         (__atomic_load_n(&thread->stack_gate.word, __ATOMIC_SEQ_CST) &
          ~(LUA_GATE_OFFER_MASK | LUA_GATE_DORMANT)) == 0;
}

static void finishstackqueue (global_State *g) {
  if (g->trace_stacks == NULL)
    __atomic_fetch_and(&g->world.control, ~cast(uint64_t, LUA_WORLD_STACK_ROOTS),
                       __ATOMIC_SEQ_CST);
}

/* A release may run after its state has been retired. Match the address
** against the current cohort's retained objects before dereferencing it. */
int luaC_readystack (global_State *g, uintptr_t stack) {
  GCObject **link, *object;
  int ready = 0;
  luaX_checksync(pthread_mutex_lock(&g->trace_mutex));
  link = &g->trace_stacks;
  while ((object = *link) != NULL) {
    int match = cast(uintptr_t, object) == stack;
    if ((match || stack == 0) && stackready(g, object)) {
      *link = *luaC_graylink(object);
      *luaC_graylink(object) = g->gray;
      g->gray = object;
      ready = 1;
    }
    else link = luaC_graylink(object);
    if (match) break;
  }
  if (ready) {
    luaC_readytrace(g);
    finishstackqueue(g);
    luaQ_wake(&g->trace_changed, &g->trace_waiters);
  }
  luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
  return ready;
}

/* Both walks own only collection links and atomic colours, never payloads.
** Publication of new registrations requires a rendezvous, so it cannot change
** these links while this admitted batch holds world.mutex. */
static unsigned pendingfinalizers (global_State *g, unsigned limit) {
  LuaTracePlan *plan = &g->trace_plan;
  unsigned count = 0;
  while (plan->pending != NULL && count < limit) {
    GCObject *object = plan->pending;
    count++;
    plan->pending = object->next;
    markobject(g, object);
  }
  if (count != 0 && plan->pending == NULL)
    __atomic_fetch_and(&g->tobefnz, ~cast(uintptr_t, FINALIZER_ROOT_SCAN),
                       __ATOMIC_RELEASE);
  return count;
}

/* A userdata without user values needs no second scan when its only edge
** is already marked. Metatable writes shade both pointers before publishing,
** and a finalized receiver has completed sharing before it enters this list. */
static int markfinalizerleaf (global_State *g, GCObject *object) {
  Udata *u;
  Table *mt;
  if (object->tt != LUA_VUSERDATA || isold(object)) return 0;
  u = gco2u(object);
  if (u->nuvalue != 0) return 0;
  mt = __atomic_load_n(&u->metatable, __ATOMIC_ACQUIRE);
  if (mt != NULL && iswhite(g, mt)) return 0;
  g->GCmarked += cast(l_mem, sizeudata(0, u->len));
  nw2black(object);
  return 1;
}

static unsigned discoverfinalizers (global_State *g, unsigned limit) {
  LuaTracePlan *plan = &g->trace_plan;
  unsigned count = 0;
  while (plan->finalizers != NULL && count < limit) {
    GCObject *object = *plan->finalizers;
    if (object == g->finobjold1) {
      plan->finalizers = NULL;
      break;
    }
    count++;
    lua_assert(tofinalize(object));
    /* A weak reader may promote the object concurrently. The colour claim
    ** decides whether discovery or promotion wins, without a racy test/set. */
    if (!claimmark(g, object)) plan->finalizers = &object->next;
    else {
      if (object == g->finobjsur) g->finobjsur = object->next;
      *plan->finalizers = object->next;
      object->next = NULL;
      *plan->finalizer_newtail = object;
      plan->finalizer_newtail = &object->next;
      if (markfinalizerleaf(g, object)) continue;
      /* Discover every candidate before tracing their outgoing edges. Doing
      ** so in an earlier batch could hide another unreachable finalizable
      ** object and defer its callback to a later collection. */
      if (plan->finalizer_gray == NULL)
        plan->finalizer_tail = luaC_graylink(object);
      *luaC_graylink(object) = plan->finalizer_gray;
      plan->finalizer_gray = object;
    }
  }
  if (plan->finalizers != NULL && *plan->finalizers == g->finobjold1)
    plan->finalizers = NULL;
  if (plan->finalizers == NULL && plan->finalizer_gray != NULL) {
    *plan->finalizer_tail = g->gray;
    g->gray = plan->finalizer_gray;
    plan->finalizer_gray = NULL;
    plan->finalizer_tail = NULL;
  }
  return count;
}

void luaC_claimtrace (lua_State *L, LuaTraceWork *work, unsigned limit) {
  global_State *g = G(L);
  LuaTracePlan *plan = &g->trace_plan;
  l_mem marked = g->GCmarked;
  work->global = g;
  if (!luaC_scanready(g)) return;
  luaC_drainbarriers(L);
  if (plan->phase == TRACE_PENDING)
    work->scanned += pendingfinalizers(g, limit) * sizeof(GCObject);
  else if (plan->phase == TRACE_FINALIZERS)
    work->scanned += discoverfinalizers(g, limit) * sizeof(GCObject);
  work->scanned += cast(lu_mem, g->GCmarked - marked);
  if (g->gray == NULL && !luaC_pagetracepending(g) && g->trace_deferred != NULL) {
    g->gray = g->trace_deferred;
    g->trace_deferred = NULL;
  }
  work->phase = TRACE_MARK;
  while (g->gray != NULL && work->count < limit) {
    GCObject *object = g->gray;
    g->gray = *luaC_graylink(object);
    work->objects[work->count++] = object;
  }
  /* Full or emergency collection may close admission while another stack
  ** owns a request. All such stacks are scannable under quiescence. */
  if (g->trace_stacks != NULL && luaW_iscollector(L)) {
    while (g->trace_stacks != NULL && work->count < limit) {
      GCObject *object = g->trace_stacks;
      g->trace_stacks = *luaC_graylink(object);
      work->objects[work->count++] = object;
    }
    finishstackqueue(g);
  }
  work->object_count = work->count;
  if (work->count < limit)
    luaC_claimtracepage(work, limit);
  if (work->count == 0 &&
      (plan->phase == TRACE_EPHEMERONS || plan->phase == TRACE_VALUES ||
       plan->phase == TRACE_CLEAR)) {
    work->phase = plan->phase;
    work->direction = plan->direction;
    while ((plan->next != NULL || plan->deferred != NULL) &&
           work->count < limit) {
      GCObject *object;
      if (plan->next != NULL) {
        object = plan->next;
        plan->next = *luaC_graylink(object);
      }
      else object = takeweak(&plan->deferred, &plan->tail);
      work->objects[work->count++] = object;
    }
    work->object_count = work->count;
  }
  if (work->count != 0)
    __atomic_fetch_add(&g->trace_busy, 1, __ATOMIC_RELAXED);
  else if (plan->phase != TRACE_FINALIZERS)
    work->scanned += luaC_skiplivefinalizers(g, limit) * sizeof(GCObject);
  luaC_readytrace(g);
}

static void deferscan (LuaTraceWork *work, GCObject *object) {
  queuework(object->tt == LUA_VTHREAD ? &work->stacks : &work->deferred, object);
}

void luaC_scantrace (lua_State *L, LuaTraceWork *work, unsigned limit) {
  unsigned index;
  int quiescent = luaW_iscollector(L);
  lua_assert(__atomic_load_n(&G(L)->trace_busy, __ATOMIC_RELAXED));
  lua_assert(work->count <= limit);
  work->local_remaining = limit - work->count;
  for (index = 0; index < work->object_count; index++) {
    GCObject *object = work->objects[index];
    if (work->phase == TRACE_MARK) {
      if (!scantrace(L, work, object, quiescent))
        deferscan(work, object);
    }
    else {
      int reservation = quiescent ? 0 : luaA_tryscan(L, object);
      if (!quiescent && reservation == 0) {
        deferweak(&work->retry, &work->retry_tail, object);
        continue;
      }
      if (work->phase == TRACE_EPHEMERONS) {
        nw2black(object);
        if (traverseephemeron(work, gco2t(object), work->direction))
          work->changed = 1;
      }
      else luaC_prepareweak(L, gco2t(object));
      work->scanned += luaC_objsize(object);
      if (reservation) luaA_endscan(L, object, reservation);
    }
  }
  for (index = 0; index < work->page_count; index++) {
    const LuaTracePageSlice *slice = &work->pages[index];
    size_t stride = sizeof(LuaAllocBlock) + slice->page->blockbytes;
    char *first = cast_charp(slice->page) + LUA_ALLOC_PAGE_HEADER +
      slice->word * 64 * stride + sizeof(LuaAllocBlock);
    uint64_t bits = slice->bits;
    do {
      unsigned slot = cast_uint(__builtin_ctzll(bits));
      GCObject *object = cast(GCObject *, first + slot * stride);
      lua_assert(object->tt == LUA_VTABLE && isgray(object));
      if (!scantrace(L, work, object, quiescent))
        deferscan(work, object);
    } while ((bits &= bits - 1) != 0);
  }
  /* Narrow chains can keep scanning within this claim. Wide frontiers
  ** publish their excess discoveries as page bits during the same walk. */
  while (work->local != NULL) {
    GCObject *object = work->local;
    work->local = *luaC_graylink(object);
    if (!scantrace(L, work, object, quiescent))
      deferscan(work, object);
  }
  luaC_flushtracepage(work);
}

static void publishwork (LuaTraceList *list, GCObject **head) {
  if (list->head != NULL) {
    *list->tail = *head;
    *head = list->head;
  }
}

void luaC_publishtrace (lua_State *L, LuaTraceWork *work) {
  global_State *g = G(L);
  LuaTracePlan *plan = &g->trace_plan;
  publishwork(&work->gray, &g->gray);
  publishwork(&work->grayagain, &g->grayagain);
  publishwork(&work->weak, &g->weak);
  publishwork(&work->allweak, &g->allweak);
  publishwork(&work->ephemeron, &g->ephemeron);
  publishwork(&work->deferred, &g->trace_deferred);
  if (work->stacks.head != NULL) {
    GCObject *object = work->stacks.head;
    /* Arm notifications before rechecking. Either a readiness transition
    ** sees the request or this check observes the transition. */
    __atomic_fetch_or(&g->world.control, LUA_WORLD_STACK_ROOTS, __ATOMIC_SEQ_CST);
    do {
      GCObject *next = *luaC_graylink(object);
      GCObject **head = stackready(g, object) ? &g->gray : &g->trace_stacks;
      *luaC_graylink(object) = *head;
      *head = object;
      object = next;
    } while (object != NULL);
    finishstackqueue(g);
  }
  if (work->retry != NULL) {
    luaC_allocnext(work->retry_tail) = plan->deferred;
    if (plan->deferred == NULL) plan->tail = work->retry_tail;
    plan->deferred = work->retry;
  }
  plan->changed |= work->changed;
  g->GCmarked += work->marked;
  /* Phase decisions join every batch, so claim counts can be published once
  ** with the work rather than contending on one cache line per object. */
  __atomic_fetch_add(&g->trace_changes, work->claims, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&g->trace_busy, 1, __ATOMIC_RELEASE);
  luaC_readytrace(g);
}

/* }====================================================== */


/*
** {======================================================
** Sweep Functions
** =======================================================
*/



/* Trim idle stacks under their own gate. Pin the target before a possible
** allocation wait so a collection cannot retire it. Running owners trim at
** their next VM safepoint. */
static void trimstacks (lua_State *L) {
  global_State *g = G(L);
  LuaWorld *world = &g->world;
  LuaWorldScope scope;
  LuaParticipation *participant;
  uint64_t interrupted = luaC_traceepoch(g);
  if (g->gcemergency || L->object_scopes != NULL || luaW_scope(L).heap != 0)
    return;
  scope = luaW_suspend(L);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  participant = world->participants;
  while (participant != NULL) {
    lua_State *thread = participant->thread;
    int reservation = 0, trim = 0;
    participant = participant->next;
    if (__atomic_load_n(&thread->trace_snapshot, __ATOMIC_ACQUIRE) ==
        luaC_traceepoch(g))
      reservation = luaX_tryreserve(thread);
    if (reservation == 0) continue;
    if (!thread->heap_waiting && !thread->object_waiting &&
        !thread->reclaiming && thread->trace_trimmed != thread->trace_snapshot) {
      trim = luaD_cantrimstack(thread);
      if (trim) thread->native_depth++;
      else thread->trace_trimmed = thread->trace_snapshot;
    }
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    if (trim) {
      if (luaC_traceepoch(g) == interrupted) {
        thread->trace_trimmed = thread->trace_snapshot;
        thread->reclaiming = 1;
        luaD_shrinkstack(thread, L);
        luaM_flushdebt(L);
        thread->reclaiming = 0;
      }
      thread->native_depth--;
    }
    luaX_endreserve(thread, reservation);
    luaX_pollrequests(L);
    luaQ_poll(L);
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    if (luaC_traceepoch(g) != interrupted) break;  /* saved cursor may be gone */
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaW_restore(L, scope, L->native_depth);
}




/*
** Classify one exclusively claimed record. Major survivors need no writes.
** Generational ages advance with a CAS that preserves mutator barriers.
*/
int luaC_sweepobject (lua_State *L, GCObject *o, unsigned kind,
                       LuaSweepWork *work) {
  global_State *g = G(L);
  lu_byte marked, changed;
  l_mem bytes = 0;
  int watching, promoted;
  if (isdead(g, o)) {
    int retired;
    if (o->tt == LUA_VTABLE) {
      l_mem size = luaH_freepooled(L, gco2t(o));
      if (size != 0) {
        work->freed += size;
        return SWEEP_RETIRED;
      }
    }
    retired = luaC_tryretire(L, o);
    if (retired != SWEEP_LIVE) return retired;
  }
  if (kind == SWEEP_MAJOR) return SWEEP_LIVE;
  marked = getmarked(o);
  if (kind == SWEEP_YOUNG && (marked & AGEBITS) != G_NEW &&
      (marked & AGEBITS) <= G_SURVIVAL_CLEAN) {
    if (o->tt == LUA_VTABLE && !luaW_iscollector(L)) {
      if (!luaC_tablesize(gco2t(o), &bytes)) return SWEEP_RETRY;
    }
    /* A root or builder can keep changing its storage after its snapshot.
    ** Promotion accounting uses that cycle's stable scanned size. */
    if (o->tt == LUA_VTHREAD) bytes = cast(l_mem, gco2th(o)->trace_bytes);
    else if (o->tt == LUA_VPROTO) bytes = cast(l_mem, gco2p(o)->trace_bytes);
    else if (o->tt != LUA_VTABLE || luaW_iscollector(L))
      bytes = luaC_objsize(o);
  }
  do {
    unsigned age = marked & AGEBITS;
    unsigned next = age;
    int edges = novariant(o->tt) != LUA_TSTRING && o->tt != LUA_VROOTSNAPSHOT &&
                age != G_SURVIVAL_CLEAN;
    promoted = kind == SWEEP_YOUNG && age != G_NEW && age <= G_SURVIVAL_CLEAN;
    if (promoted) next = edges ? G_TOUCHED1 : G_OLD;
    else if (kind == SWEEP_YOUNG && age == G_NEW) next = G_SURVIVAL;
    else if (kind == SWEEP_OLD && age <= G_SURVIVAL_CLEAN) next = G_OLD;
    /* Changing age and claiming the remembered link are one operation.
    ** Mutators can update the age, and must leave our claimed link alone. */
    watching = ((promoted && edges) ||
                (o->tt == LUA_VTHREAD && next > G_SURVIVAL_CLEAN)) &&
               !testbit(marked, QUEUEDBIT);
    changed = cast_byte((marked & ~AGEBITS) | next);
    if (watching) changed |= bitmask(QUEUEDBIT);
    if (changed == marked) break;
  } while (!__atomic_compare_exchange_n(&o->marked, &marked, changed, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
  if (promoted) work->addedold += bytes;
  if ((changed & AGEBITS) <= G_SURVIVAL_CLEAN ||
      (watching && o->tt != LUA_VTHREAD)) work->minor_work = 1;
  return watching ? SWEEP_WATCH : SWEEP_LIVE;
}

/* }====================================================== */


/*
** {======================================================
** Finalization
** =======================================================
*/

/*
** If possible, shrink string table.
*/
static void checkSizes (lua_State *L, global_State *g) {
  int i;
  for (i = 0; i < LUA_STRING_SHARDS; i++) {
    stringtable *tb = &g->strt[i];
    int target = tb->size;
    while (target > LUA_STRING_INITIAL_BUCKETS && tb->nuse < target / 4)
      target /= 2;
    if (target == tb->size) continue;
    /* A parked allocator may retain the old buckets. Retry that shard
    ** during the next cycle if it cannot shrink here. */
    if (luaA_lockowner(&tb->access) == NULL)
      luaS_resize(L, tb, target);
    if (tb->size != target)
      __atomic_store_n(&g->shrink_strings, 1, __ATOMIC_RELAXED);
  }
}


static void dothecall (lua_State *L, void *ud) {
  UNUSED(ud);
  luaD_callnoyield(L, L->top.p - 2, 0);
}


/* A stopped world appends new cohorts and retires objects. An admitted
** dequeue can therefore sample a node without a reclamation hazard or ABA.
** Re-registration before that rendezvous can change its link, so the fast
** path reads it atomically and follows it only if its head claim wins. */
static GCObject *takefinalizer (global_State *g) {
  uintptr_t head = __atomic_load_n(&g->tobefnz, __ATOMIC_ACQUIRE);
  while (head != 0) {
    GCObject *object;
    GCObject *next;
    if (head & FINALIZER_ROOT_SCAN) {
      luaX_checksync(pthread_mutex_lock(&g->trace_mutex));
      head = __atomic_load_n(&g->tobefnz, __ATOMIC_ACQUIRE);
      if (head & FINALIZER_ROOT_SCAN) {
        uintptr_t updated;
        object = finalizerhead(head);
        next = object->next;
        if (g->trace_plan.pending == object) g->trace_plan.pending = next;
        updated = cast(uintptr_t, next);
        if (g->trace_plan.pending != NULL) updated |= FINALIZER_ROOT_SCAN;
        __atomic_store_n(&g->tobefnz, updated, __ATOMIC_RELEASE);
        luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
        return object;
      }
      luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
    }
    else {
      object = finalizerhead(head);
      next = __atomic_load_n(&object->next, __ATOMIC_RELAXED);
      if (__atomic_compare_exchange_n(&g->tobefnz, &head, cast(uintptr_t, next),
                                       0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return object;
    }
  }
  return NULL;
}

static int GCTM (lua_State *L) {
  global_State *g = G(L);
  LuaNativeRoot object, method;
  GCObject *o;
  lua_assert(!g->gcemergency);
  if (!luaC_hasfinalizers(g)) return 0;
  luaR_pushroot(L, &object);
  o = takefinalizer(g);
  if (o == NULL) {
    luaR_poproot(L, &object);
    return 0;
  }
  lua_assert(tofinalize(o));
  /* Transfer the queue root before any operation that can withdraw. */
  setgcovalue(L, &object.value, o);
  resetbit(o->marked, FINALIZEDBIT);
  luaC_tracepointer(L, o);
  luaR_pushroot(L, &method);
  luaT_gettmbyobj(L, &object.value, TM_GC, &method);
  if (!ttisnil(&method.value)) {
    TStatus status;
    lu_byte oldah = L->allowhook;
    LuaWorldScope depth;
    /* Embedding limits also account for Lua finalizer instructions. */
    L->allowhook = (gethookmask(L) & LUTHR_LUA_RUNTIME_MASK_FINALIZERS) ? 1 : 0;
    setobj2s(L, L->top.p++, &method.value);  /* push finalizer... */
    setobj2s(L, L->top.p++, &object.value);  /* ... and its argument */
    L->ci->callstatus |= CIST_FIN;  /* will run a finalizer */
    depth = luaW_suspend(L);
    status = luaD_pcall(L, dothecall, NULL, savestack(L, L->top.p - 2), 0);
    luaW_restore(L, depth, L->native_depth);
    L->ci->callstatus &= ~CIST_FIN;  /* not running a finalizer anymore */
    L->allowhook = oldah;  /* restore hooks */
    if (l_unlikely(status != LUA_OK)) {  /* error while running __gc? */
      luaE_warnerror(L, "__gc");
      L->top.p--;  /* pops error object */
    }
  }
  luaR_poproot(L, &method);
  luaR_poproot(L, &object);
  return 1;
}


/* Release collector ownership once per drain. An individual callback can
** park while another execution collects and takes the next pending root. */
static l_mem drainfinalizers (lua_State *L, l_mem limit) {
  LuaWorldScope scope;
  l_mem count = 0;
  int suspend = L->object_scopes == NULL;
  if (!luaC_hasfinalizers(G(L))) return 0;
  if (suspend) scope = luaW_suspend(L);
  while (count < limit && GCTM(L)) count++;
  if (suspend) luaW_restore(L, scope, L->native_depth);
  return count;
}

void luaC_runfinalizers (lua_State *L) {
  drainfinalizers(L, MAX_LMEM);
}


/* Only a stopped collector appends. The last pop can leave a stale tail
** behind, which is ignored until a new nonempty list supplies its own tail. */
static void appendfinalizers (global_State *g, GCObject *head, GCObject **tail) {
  lua_assert(!(__atomic_load_n(&g->tobefnz, __ATOMIC_RELAXED) &
               FINALIZER_ROOT_SCAN));
  if (!luaC_hasfinalizers(g))
    __atomic_store_n(&g->tobefnz, cast(uintptr_t, head), __ATOMIC_RELEASE);
  else
    *g->tobefnz_tail = head;
  g->tobefnz_tail = tail;
}


/* Shutdown alone selects every remaining finalizer, regardless of reachability.
** Ordinary collection discovers candidates in admitted, bounded batches. */
static void separateallfinalizers (global_State *g) {
  GCObject *curr;
  while ((curr = g->finobj) != NULL) {
    lua_assert(tofinalize(curr));
    g->finobj = curr->next;
    curr->next = NULL;
    appendfinalizers(g, curr, &curr->next);
  }
}


/* New registrations prepend to the stable index. Before candidate selection
** they invalidate the already checked prefix. */
static void resetfinalizercursor (lua_State *L) {
  global_State *g = G(L);
  LuaTracePlan *plan = &g->trace_plan;
  if (luaC_tracing(L) &&
      (plan->phase <= TRACE_MARK || plan->phase == TRACE_VALUES ||
       (plan->phase == TRACE_EPHEMERONS &&
        (plan->after == TRACE_VALUES || plan->after == TRACE_FINALIZERS))))
    plan->finalizers = &g->finobj;
}

/* Finalization is an independent index. Registration never searches or
** modifies allocation membership, generation boundaries, or a sweep cursor. */
l_sinline void __attribute__((always_inline)) registerfinalizer (lua_State *L,
                                                                GCObject *o) {
  global_State *g = G(L);
  if (__atomic_fetch_or(&o->marked, bitmask(FINALIZEDBIT), __ATOMIC_RELAXED) &
      bitmask(FINALIZEDBIT)) return;
  luaC_noteyoung(g);
  if (luaW_iscollector(L)) {
    o->next = g->finobj;
    g->finobj = o;
    resetfinalizercursor(L);
  }
  else {
    /* An exchange orders concurrent registrations. Producers initialize
    ** only their own link, and publication joins all admitted producers
    ** before reading either the links or the first producer's tail. */
    GCObject *next = __atomic_exchange_n(&g->new_finalizers, o, __ATOMIC_RELAXED);
    __atomic_store_n(&o->next, next, __ATOMIC_RELAXED);
    if (next == NULL) g->new_finalizers_tail = o;
  }
}

void luaC_publish (lua_State *L) {
  global_State *g = G(L);
  GCObject *finalizers;
  int shard;
  lua_assert(!completestate(g) || luaW_iscollector(L));
  luaM_publishdebt(L);
  /* Only batch producers have outstanding lists. Withdrawal has already
  ** charged the other owners' local debt. */
  for (shard = 0; shard < LUA_PUBLISH_SHARDS; shard++) {
    lua_State *thread = g->publishers[shard].head;
    g->publishers[shard].head = NULL;
    while (thread != NULL) {
      lua_State *next = thread->publish_next;
      luaM_flushdebt(thread);
      if (thread->new_objects != NULL) {
        luaC_allocnext(thread->new_objects_tail) = g->allgc;
        if (luaC_sweeping(L) && g->sweep_plan.append == &g->allgc)
          g->sweep_plan.append = luaC_alloclink(thread->new_objects_tail);
        g->allgc = thread->new_objects;
        thread->new_objects = thread->new_objects_tail = NULL;
      }
      if (thread->new_tables != NULL) {
        thread->new_tables = NULL;
        thread->nursery_tables = 0;
        thread->nursery_epoch += NURSERY_STEP;
      }
      luaHS_publishpending(thread);
      __atomic_store_n(&thread->publish_previous, NULL, __ATOMIC_RELAXED);
      thread->publish_next = NULL;
      thread = next;
    }
  }
  finalizers = __atomic_exchange_n(&g->new_finalizers, NULL, __ATOMIC_RELAXED);
  if (finalizers != NULL) {
    g->new_finalizers_tail->next = g->finobj;
    g->finobj = finalizers;
    g->new_finalizers_tail = NULL;
    resetfinalizercursor(L);
  }
#if defined(LUAI_ASSERT)
  {
    LuaParticipation *participant;
    for (participant = g->world.participants; participant != NULL;
         participant = participant->next) {
      lua_State *owner = participant->thread;
      lua_assert(owner->new_objects == NULL && owner->new_tables == NULL &&
                 owner->storage_retired == NULL &&
                 owner->allocation_debt.pending == 0 && owner->reclaim_credit == 0 &&
                 owner->withdrawn_debt == 0);
    }
  }
#endif
}

void luaC_checkfinalizer (lua_State *L, GCObject *o, Table *mt) {
  global_State *g = G(L);
  TValue method;
  /* Binding the metatable already roots both objects and owns their layout. */
  lua_assert(luaW_iscollector(L) ||
             luaA_lockowner(&mt->access) == luaX_token(L));
  if (tofinalize(o) || (g->gcstp & GCSTPCLS) ||
      (gettableflags(mt) & (1u << TM_GC))) return;
  /* Promotion may have crossed a weak cutoff since the layout was acquired. */
  if (luaC_tracing(L) && !luaW_iscollector(L)) luaC_prepareweak(L, mt);
  if (tagisempty(luaH_getshortowned(mt, g->tmname[TM_GC], &method))) {
    __atomic_fetch_or(&mt->flags, cast_byte(1u << TM_GC), __ATOMIC_RELAXED);
    return;
  }
  luaA_share(L, o);
  registerfinalizer(L, o);
}

void luaC_registerfinalizer (lua_State *L, GCObject *o) {
  lua_assert(__atomic_load_n(luaA_sharing(o), __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED);
  registerfinalizer(L, o);
}

/* }====================================================== */


/*
** {======================================================
** Generational Collector
** =======================================================
*/

/*
** Trace and generational-mode heuristics use 'GCmarked' and 'GCmajorminor'.
** Allocation pacing instead uses logical live storage and the pacer's targets.
** The tracing counters play several roles, depending on the collector mode:
** * KGC_INC:
**     GCmarked: number of marked bytes during a cycle.
**     GCmajorminor: not used.
** * KGC_GENMINOR
**     GCmarked: number of bytes that became old since last major collection.
**     GCmajorminor: number of bytes marked in last major collection.
** * KGC_GENMAJOR
**     GCmarked: number of bytes that became old since last major collection.
**     GCmajorminor: number of bytes marked in last major collection.
*/


/*
** Pacing counts every live allocation, including non-GC runtime records and
** allocations made black during tracing. Pending destruction is not live.
** The cycle's fixed goal survives later physical reclamation: refreshing debt
** must not grant a fresh growth allowance for intervening allocations.
*/
static void setidledebt (global_State *g) {
  luaP_idle(g);
}

static void finishincremental (lua_State *L, global_State *g) {
  /* Allocations made after marking already consume the next cycle's
  ** allowance. Only the swept cohort sets its live base. */
  luaC_setstate(g, GCSpause);
  __atomic_store_n(&g->pacer.goal, applygcparam(g, PAUSE, g->GCcyclelive),
                   __ATOMIC_RELAXED);
  setidledebt(g);
  L->reclaim_pages = 1;
  __atomic_fetch_add(&g->major_completed, 1, __ATOMIC_RELEASE);
}


/*
** Finish a young-generation collection.
*/
static void finishgencycle (global_State *g) {
  l_mem live = g->GCcyclelive;
  l_mem growth = applygcparam(g, MINORMUL, g->GCmajorlive);
  lua_assert(live >= 0);
  __atomic_store_n(&g->pacer.goal,
    growth > MAX_LMEM - live ? MAX_LMEM : live + growth, __ATOMIC_RELAXED);
  luaC_setstate(g, GCSpropagate);  /* skip restart */
}


/*
** Shift from minor to major collections. The next major rotates its stamp;
** no sweep is required to reset the old generation's marking state.
*/
static void minor2inc (global_State *g, lu_byte kind) {
  g->GCmajorminor = g->GCmarked;  /* number of live bytes */
  luaC_setkind(g, kind);
  g->old1 = g->survival = NULL;
  g->finobjold1 = g->finobjsur = NULL;
  luaC_setstate(g, GCSpause);  /* the next major rotates its stamp in constant time */
  /* set a debt equal to the step size */
  luaE_setdebt(g, applygcparam(g, STEPSIZE, 100));
}


/*
** Decide whether to shift to major mode. It shifts if the accumulated
** number of added old bytes (counted in 'GCmarked') is larger than
** 'minormajor'% of the number of lived bytes after the last major
** collection. (This number is kept in 'GCmajorminor'.)
*/
static int checkminormajor (global_State *g) {
  l_mem limit = applygcparam(g, MINORMAJOR, g->GCmajorminor);
  if (limit == 0)
    return 0;  /* special case: 'minormajor' 0 stops major collections */
  return (g->GCmarked >= limit);
}

static void finishyoung (lua_State *L, global_State *g, l_mem marked) {
  UNUSED(g);
  luaC_beginsweep(L, SWEEP_YOUNG, marked);
}


/*
** Clears all gray lists, sweeps objects, and prepare sublists to enter
** generational mode. The sweeps remove dead objects and turn all
** surviving objects to old. Threads go back to 'grayagain'; everything
** else is turned black (not in any gray list).
*/
static void atomic2gen (lua_State *L, global_State *g) {
  luaC_beginsweep(L, SWEEP_OLD, g->GCmarked);
}

int luaC_sweepgray (GCObject *o) {
  lu_byte marked = getmarked(o), changed;
  int keep;
  do {
    unsigned age = marked & AGEBITS;
    keep = age > G_SURVIVAL_CLEAN &&
      (age == G_TOUCHED0 || age >= G_TOUCHED1 || o->tt == LUA_VTHREAD);
    changed = keep ? marked : cast_byte(marked & ~bitmask(QUEUEDBIT));
    if (changed == marked) break;
  } while (!__atomic_compare_exchange_n(&o->marked, &marked, changed, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  return keep;
}

static void sweepslot (lua_State *L, LuaSweepWork *work, LuaAllocBlock *block,
                       uintptr_t header) {
  GCObject *object = cast(GCObject *, block + 1);
  int result = luaC_sweepobject(L, object, work->kind, work);
  if (result == SWEEP_RETRY) {
    if (work->retry == NULL) work->retry_tail = luaC_alloclink(object);
    luaC_allocnext(object) = work->retry;
    work->retry = object;
  }
  else if (result != SWEEP_RETIRED) {
    if (!isold(object))
      luaMP_young(luaMP_pageof(header), G(L)->sweep_plan.epoch + 1);
    if (header & LUA_BLOCK_BIRTH)
      __atomic_store_n(&block->block.page, header & ~cast(uintptr_t, LUA_BLOCK_BIRTH),
                       __ATOMIC_RELAXED);
    if (result == SWEEP_WATCH) queuework(&work->watch, object);
  }
}

/* A full, unowned page of dead objects can be discarded in one operation.
** Validate every slot and backing buffer before releasing any of them.
** Any backing requiring the host allocator uses ordinary deferred sweep. */
static int release_dead_page (lua_State *L, LuaSweepWork *work,
                              size_t stride, uintptr_t birth) {
  global_State *g = G(L);
  LuaAllocPage *page = work->page;
  l_mem bytes = 0, headers = 0;
  unsigned count = 0;
  size_t offset, limit = luaMP_slotend(page);
  for (offset = LUA_ALLOC_PAGE_HEADER; offset + stride <= limit;
       offset += stride) {
    LuaAllocBlock *block = cast(LuaAllocBlock *, cast_charp(page) + offset);
    uintptr_t header = luaMP_header(block);
    GCObject *object = cast(GCObject *, block + 1);
    size_t size;
    if (!(header & LUA_BLOCK_COLLECTABLE) || (header & LUA_BLOCK_BIRTH) == birth ||
        (work->kind == SWEEP_YOUNG && isold(object)) ||
        !iswhite(g, object)) return 0;
    if (object->tt == LUA_VTABLE) {
      Table *table = gco2t(object);
      size = luaH_pooledsize(table);
      if (size == 0) return 0;
      headers += cast(l_mem, sizeof(*table) + table->inline_bytes);
    }
    else {
      Udata *u = gco2u(object);
      lua_assert(!tofinalize(object));
      size = sizeudata(u->nuvalue, u->len);
      headers += cast(l_mem, size);
    }
    bytes += cast(l_mem, size);
    count++;
  }
  lua_assert(count != 0 && count == page->used);
  if (headers != bytes) {
    for (offset = LUA_ALLOC_PAGE_HEADER; offset + stride <= limit;
         offset += stride) {
      LuaAllocBlock *block = cast(LuaAllocBlock *, cast_charp(page) + offset);
      GCObject *object = cast(GCObject *, block + 1);
      if (object->tt == LUA_VTABLE) luaH_freeparts(L, gco2t(object), 0);
    }
  }
  work->page = NULL;
  work->count = count;
  work->freed = bytes;
  luaM_releasepage(L, page, headers);
  return 1;
}

static void sweep_page_batch (lua_State *L, LuaSweepWork *work) {
  global_State *g = G(L);
  if (work->page != NULL) {
    LuaAllocPage *page = work->page;
    size_t stride = sizeof(LuaAllocBlock) + page->blockbytes;
    uintptr_t birth = luaMP_birth(g->sweep_plan.epoch);
    unsigned count = 0, returned = 0;
    LuaAllocBlock *first = NULL, *last = NULL;
    l_mem bytes = 0;
    size_t offset, limit = luaMP_slotend(page);
    if (!work->full_page || !release_dead_page(L, work, stride, birth)) {
      for (offset = LUA_ALLOC_PAGE_HEADER; offset + stride <= limit;
           offset += stride) {
        LuaAllocBlock *block = cast(LuaAllocBlock *, cast_charp(page) + offset);
        uintptr_t header = luaMP_header(block);
        GCObject *object = cast(GCObject *, block + 1);
        if (!(header & LUA_BLOCK_COLLECTABLE) || (header & LUA_BLOCK_BIRTH) == birth)
          continue;
        if (work->kind == SWEEP_YOUNG && isold(object))
          continue;
        if (object->tt == LUA_VUSERDATA && isdead(g, object)) {
          Udata *u = gco2u(object);
          lua_assert(!tofinalize(object));
          bytes += cast(l_mem, sizeudata(u->nuvalue, u->len));
          __atomic_store_n(&block->block.page, cast(uintptr_t, page),
                           __ATOMIC_RELAXED);
          block->block.link.next = first;
          first = block;
          if (last == NULL) last = block;
          returned++;
        }
        else sweepslot(L, work, block, header);
        count++;
      }
      if (returned != 0) {
        luaM_freeobjectbatch(L, page, first, last, returned, bytes, 0);
        work->freed += bytes;
      }
      work->count = count;
    }
  }
  else {
    GCObject *object = work->next;
    while (object != NULL) {
      GCObject *next = luaC_allocnext(object);
      LuaAllocBlock *block = cast(LuaAllocBlock *, object) - 1;
      sweepslot(L, work, block, luaMP_header(block));
      object = next;
    }
    if (work->next == NULL) work->count = 0;
  }
  if (work->freed != 0) {
    /* Freeing a slot already credits heap debt. Settle the frozen cohort's
    ** estimate before publishing the batch, alongside deferred retirement. */
    if (!(g->gcstp & GCSTPCLS))
      __atomic_fetch_sub(&g->GCcyclelive, work->freed, __ATOMIC_RELAXED);
    luaM_flushdebt(L);
  }
}

void luaC_scansweep (lua_State *L, LuaSweepWork *work) {
  global_State *g = G(L);
  GCObject *object = work->next;
  if (work->paged) {
    sweep_page_batch(L, work);
    return;
  }
  if (work->phase == SWEEP_GRAY) {
    while (object != NULL) {
      GCObject *next = *luaC_graylink(object);
      if (luaC_sweepgray(object)) {
        queuework(&work->watch, object);
        /* Threads stay on the root list even when they have no young edges.
        ** Other remembered owners still need to age out of that list. */
        if (object->tt != LUA_VTHREAD) work->minor_work = 1;
      }
      object = next;
    }
    return;
  }
  work->live = object;
  work->tail = &work->live;
  while ((object = *work->tail) != NULL) {
    GCObject *next = luaC_allocnext(object);
    int result;
    if (work->phase == SWEEP_OBJECTS && object->tt == LUA_VTHREAD &&
        isdead(g, object)) {
      *work->tail = next;
      if (work->threads == NULL) work->threads_tail = luaC_alloclink(object);
      luaC_allocnext(object) = work->threads;
      work->threads = object;
      continue;
    }
    result = luaC_sweepobject(L, object, work->kind, work);
    if (result == SWEEP_RETRY) {
      *work->tail = next;
      if (work->phase == SWEEP_THREADS) {
        if (work->threads == NULL) work->threads_tail = luaC_alloclink(object);
        luaC_allocnext(object) = work->threads;
        work->threads = object;
      }
      else {
        if (work->retry == NULL) work->retry_tail = luaC_alloclink(object);
        luaC_allocnext(object) = work->retry;
        work->retry = object;
      }
    }
    else if (result == SWEEP_RETIRED)
      *work->tail = next;
    else {
      work->tail = luaC_alloclink(object);
      if (result == SWEEP_WATCH) queuework(&work->watch, object);
    }
  }
}

void luaC_endsweep (lua_State *L) {
  global_State *g = G(L);
  LuaSweepPlan *plan = &g->sweep_plan;
  if (plan->kind == SWEEP_MAJOR) {
    /* Pending finalizers run in a separate incremental step. */
    if (luaC_hasfinalizers(g) && !g->gcemergency) {
      luaC_setstate(g, GCScallfin);
      luaE_setdebt(g, 0);
    }
    else finishincremental(L, g);
    return;
  }
  /* Joining the lists also fills empty band heads with the following band. */
  g->survival = plan->live[0];
  g->old1 = plan->kind == SWEEP_YOUNG ? plan->live[1] : plan->live[0];
  g->finobjsur = plan->finhead;
  g->finobjold1 = plan->kind == SWEEP_YOUNG ? plan->finsurvival : plan->finhead;
  if (plan->kind == SWEEP_YOUNG) {
    g->GCmarked = plan->base_marked + plan->addedold;
    if (checkminormajor(g)) {
      minor2inc(g, KGC_GENMAJOR);
      g->GCmarked = 0;
      return;
    }
  }
  else {
    g->GCmajorlive = getgcbytes(g);
    g->GCmajorminor = plan->base_marked;
    g->GCmarked = 0;
  }
  L->reclaim_pages = 1;
  finishgencycle(g);
  setidledebt(g);
  if (plan->kind == SWEEP_OLD)
    __atomic_fetch_add(&g->major_completed, 1, __ATOMIC_RELEASE);
}

/* Rehash intern tables only after joining mutators and sweep workers. */
void luaC_shrinkstrings (lua_State *L) {
  global_State *g = G(L);
  lu_byte stopem = g->gcstopem;
  lua_assert(luaW_iscollector(L));
  if (g->gcemergency) return;
  __atomic_store_n(&g->shrink_strings, 0, __ATOMIC_RELAXED);
  g->gcstopem = 1;
  checkSizes(L, g);
  g->gcstopem = stopem;
}

/*
** Finish physical page reclamation independently of the cycle's fixed budget.
*/
void luaC_finishreclaim (lua_State *L) {
  if (G(L)->gcstp & GCSTPCLS) return;
  /* Reclamation pays physical credits without rebasing logical pacing. Page
  ** lifetime work owns its own protocol; accounting needs no rendezvous. */
  luaM_reclaimpages(L);
}


/*
** Enter generational mode. Must go until the end of an atomic cycle
** to ensure that all objects are correctly marked and weak tables
** are cleared. Then, turn all objects into old and finishes the
** collection.
*/
static void entergen (lua_State *L, global_State *g) {
  luaC_runtilstate(L, GCSpause, 1);
  if (luaC_kind(g) == KGC_GENMINOR) return;
  restartcollection(L, g, TRACE_GENERATIONAL);
  luaC_setstate(g, GCSpropagate);
  atomic(L);
  luaC_waitsweep(L);
  L->reclaim_pages = 1;
}


/*
** Change collector mode to 'newmode'.
*/
void luaC_changemode (lua_State *L, int newmode) {
  global_State *g = G(L);
  waitcollection(L);
  if (luaC_kind(g) == KGC_GENMAJOR)  /* doing major collections? */
    luaC_setkind(g, KGC_INC);  /* already incremental but in name */
  if (newmode != luaC_kind(g)) {  /* does it need to change? */
    if (newmode == KGC_INC)  /* entering incremental mode? */
      minor2inc(g, KGC_INC);  /* entering incremental mode */
    else {
      lua_assert(newmode == KGC_GENMINOR);
      entergen(L, g);
    }
  }
  luaC_waitsweep(L);
}


/*
** Does a full collection in generational mode.
*/
static void fullgen (lua_State *L, global_State *g) {
  minor2inc(g, KGC_INC);
  entergen(L, g);
}


/*
** After an atomic incremental step from a major collection,
** check whether collector could return to minor collections.
** It checks whether the number of bytes 'tobecollected'
** is greater than 'majorminor'% of the number of bytes added
** since the last collection ('addedbytes').
*/
static int checkmajorminor (lua_State *L, global_State *g) {
  if (luaC_kind(g) == KGC_GENMAJOR) {  /* generational mode? */
    l_mem numbytes = gettotalbytes(g);
    l_mem addedbytes = numbytes - g->GCmajorminor;
    l_mem limit = applygcparam(g, MAJORMINOR, addedbytes);
    l_mem tobecollected = numbytes - g->GCmarked;
    if (tobecollected > limit) {
      atomic2gen(L, g);  /* return to generational mode */
      return 1;  /* exit incremental collection */
    }
  }
  g->GCmajorminor = g->GCmarked;  /* prepare for next collection */
  return 0;  /* stay doing incremental collections */
}

/* }====================================================== */


/*
** {======================================================
** GC control
** =======================================================
*/


/*
** Detach the completed mark's cohort. Later publications stay outside it.
*/
static void entersweep (lua_State *L) {
  global_State *g = G(L);
  luaC_beginsweep(L, SWEEP_MAJOR, g->GCmarked);
}


/*
** Delete all objects in list 'p' until (but not including) object
** 'limit'.
*/
static void deletelist (lua_State *L, GCObject *p, GCObject *limit, int fixed) {
  while (p != limit) {
    GCObject *next = fixed ? p->next : luaC_allocnext(p);
    luaC_retireobject(L, p);
    p = next;
  }
}

static void retire_pages (lua_State *L) {
  global_State *g = G(L);
  unsigned i, pools = luaMP_sweepcount(g);
  for (i = 0; i < pools; i++) {
    LuaAllocPage *page;
    unsigned pool = luaMP_sweeppool(i);
    for (page = *luaMP_sweephead(g, pool); page != NULL; page = page->sweep_next) {
      size_t stride = sizeof(LuaAllocBlock) + page->blockbytes;
      size_t offset, limit = luaMP_slotend(page);
      if (__atomic_load_n(&page->sweep_epoch, __ATOMIC_RELAXED) == 0) continue;
      for (offset = LUA_ALLOC_PAGE_HEADER; offset + stride <= limit;
           offset += stride) {
        LuaAllocBlock *block = cast(LuaAllocBlock *, cast_charp(page) + offset);
        if (luaMP_header(block) & LUA_BLOCK_COLLECTABLE)
          luaC_retireobject(L, cast(GCObject *, block + 1));
      }
    }
  }
}


/*
** Call all finalizers of the objects in the given Lua state, and
** then free all objects, except for the main thread.
*/
void luaC_freeallobjects (lua_State *L) {
  global_State *g = G(L);
  luaC_sealtasks(L);
  g->gcstp = GCSTPCLS;  /* no extra finalizers after here */
  if (luaC_tracing(L)) {
    lu_byte emergency = g->gcemergency;
    /* An automatic slice has no suspended driver to finish at shutdown.
    ** Join its graph work without running finalizers or releasing the world;
    ** the complete finalization list is separated below. */
    g->gcemergency = 1;
    atomic(L);
    g->gcemergency = emergency;
  }
  luaC_changemode(L, KGC_INC);
  luaC_waitsweep(L);
  separateallfinalizers(g);  /* teardown finalizes every remaining object */
  lua_assert(g->finobj == NULL);
  luaC_runfinalizers(L);
  retire_pages(L);
  deletelist(L, g->allgc, obj2gco(mainthread(g)), 0);
  lua_assert(g->finobj == NULL);  /* no new finalizers */
  luaC_reclaim(L);  /* external callbacks precede fixed DLL handle destruction */
  deletelist(L, g->fixedgc, NULL, 1);  /* collect fixed objects */
  luaC_reclaim(L);  /* shutdown owns the complete VM until destruction ends */
  { int i; for (i = 0; i < LUA_STRING_SHARDS; i++)
      lua_assert(g->strt[i].nuse == 0); }
}


void luaC_initialtrace (lua_State *L) {
  global_State *g = G(L);
  lua_assert(luaC_tracing(L) && (L->world_depth != 0 || luaW_ownsheap(L)));
  g->trace_plan.again = g->grayagain;
  g->grayagain = NULL;
  lua_assert(g->ephemeron == NULL && g->weak == NULL);
  markobject(g, mainthread(g));
  luaC_setstate(g, GCSatomic);
  luaX_checksync(pthread_mutex_lock(&g->trace_mutex));
  __atomic_store_n(&g->trace_plan.phase,
    g->trace_plan.pending != NULL ? TRACE_PENDING : TRACE_MARK, __ATOMIC_RELAXED);
  luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
  markobject(g, L);  /* mark running thread */
  if (!g->trace_plan.roots_pending) {
    marknativeframes(g);
    markvalue(g, &g->l_registry);
  }
  else {
    /* A concurrent replacement is covered by the stopped root closure. */
    TValue registry;
    if (luaA_tryregistry(g, &registry)) markvalue(g, &registry);
  }
  markmt(g);  /* mark global metatables */
}

/* Close roots whose owners can be paused across startup before making any
** weak decisions. This includes a registry slot busy during the handshake. */
void luaC_finishroots (lua_State *L) {
  global_State *g = G(L);
  lua_assert(L->world_depth != 0);
  if (g->trace_plan.roots_pending) {
    g->trace_plan.roots_pending = 0;
    marknativeframes(g);
    markvalue(g, &g->l_registry);
  }
}

void luaC_finishtrace (lua_State *L) {
  global_State *g = G(L);
  unsigned finish = g->trace_plan.finish;
  l_mem marked = g->trace_plan.base_marked;
  lua_assert(L->world_depth != 0 && luaC_tracing(L));
  luaS_clearcache(g);
  /* A previous cycle's callbacks may still be running. Publish this cohort
  ** only after its reachability and weak decisions are complete. */
  if (g->trace_plan.finalizer_new != NULL) {
    appendfinalizers(g, g->trace_plan.finalizer_new,
                     g->trace_plan.finalizer_newtail);
    g->trace_plan.finalizer_new = NULL;
    g->trace_plan.finalizer_newtail = &g->trace_plan.finalizer_new;
  }
  endtrace(L);
  g->trace_plan.finish = 0;
  lua_assert(g->gray == NULL && !luaC_pagetracepending(g));
  if (finish == TRACE_YOUNG) {
    finishyoung(L, g, marked);
  }
  else if (finish == TRACE_GENERATIONAL) {
    atomic2gen(L, g);
    L->reclaim_pages = 1;
  }
  else if (!checkmajorminor(L, g))
    entersweep(L);
}

/* Only one complete assist queues for a phase transition. Others either scan
** newly published work or wait for progress without reserving metadata. */
static void waittraceprogress (lua_State *L, uint64_t epoch) {
  global_State *g = G(L);
  int advance = 0;
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  if (luaC_tracing(L) && luaC_traceepoch(g) == epoch) {
    int blocked = luaC_traceblocked(g);
    if (!blocked && !luaC_tracepending(g)) {
      __atomic_store_n(&g->trace_advance, 1, __ATOMIC_RELEASE);
      advance = 1;
    }
    else if (blocked && !luaC_markready(g))
      luaW_waitcollector(L, LUA_WAIT_TRACE);
  }
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  if (advance) {
    luaW_collectenter(L);
    if (luaC_tracing(L) && luaC_traceepoch(g) == epoch)
      luaC_advancetrace(L);
    luaW_heapleave(L);
    /* Pair with wait registration under trace_mutex. No lock or collector
    ** scope is kept by followers while the claimant seeks admission. */
    luaX_checksync(pthread_mutex_lock(&g->trace_mutex));
    __atomic_store_n(&g->trace_advance, 0, __ATOMIC_RELEASE);
    luaQ_wake(&g->trace_changed, &g->trace_waiters);
    luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
  }
}

/* Only scalar tickets and ownership scopes survive a scheduler poll. Every
** graph cursor is shared, so another execution can finish this very job. */
static lu_mem tracework (lua_State *L, uint64_t epoch, lu_mem budget,
                         int complete) {
  global_State *g = G(L);
  lu_mem worked = 0;
  if (luaC_markstate(g) & LUA_MARK_ARMING) {
    /* Wait for ready work without queuing a metadata grant for every
    ** allocator or explicit step that joins the same startup. */
    luaX_checksync(pthread_mutex_lock(&g->world.mutex));
    if (luaC_traceepoch(g) == epoch && (luaC_markstate(g) & LUA_MARK_ARMING))
      luaW_waitcollector(L, LUA_WAIT_ARM);
    luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  }
  if (!luaC_tracing(L) || luaC_traceepoch(g) != epoch) return 0;
  if (g->gcemergency || L->object_scopes != NULL || luaW_scope(L).heap != 0) {
    /* These callers may borrow mutable storage which cannot cross admission.
    ** Emergency retries must finish before their allocator resumes. */
    while (luaC_tracing(L) && luaC_traceepoch(g) == epoch) {
      luaC_advancetrace(L);
      if (!luaC_tracing(L) || luaC_traceepoch(g) != epoch) break;
      luaC_tracebatch(L, LUA_TRACE_BATCH);
    }
  }
  else {
    LuaWorldScope scope;
    if (!luaC_tracepending(g))
      luaC_advancetrace(L);
    if (L->world_depth != 0) g->gcstopem = 0;
    scope = luaW_suspend(L);
    while (luaC_tracing(L) && luaC_traceepoch(g) == epoch) {
      lu_mem done;
      if (!luaC_tracepending(g))
        luaC_advancetrace(L);
      if (!luaC_tracing(L) || luaC_traceepoch(g) != epoch) break;
      done = luaC_tracebatch(L, LUA_TRACE_BATCH);
      worked = done > cast(lu_mem, MAX_LMEM) - worked
        ? cast(lu_mem, MAX_LMEM) : worked + done;
      if (!complete && (worked >= budget || done == 0)) {
        /* The last publisher also services termination. Deferring an empty
        ** job until another allocation slice would retain every black object
        ** allocated in that gap and inflate the following cycle's live base. */
        if (luaC_tracing(L) && luaC_traceepoch(g) == epoch &&
            !luaC_tracepending(g))
          luaC_advancetrace(L);
        break;
      }
      luaX_pollrequests(L);
      luaQ_poll(L);
      if (done == 0) waittraceprogress(L, epoch);
    }
    luaW_restore(L, scope, L->native_depth);
  }
  if (luaC_traceepoch(g) == epoch && L->assist_epoch ==
      __atomic_load_n(&g->world.work_epoch, __ATOMIC_ACQUIRE))
    L->trace_debt = worked >= L->trace_debt ? 0 : L->trace_debt - worked;
  return worked;
}

static int atomic (lua_State *L) {
  global_State *g = G(L);
  uint64_t epoch = luaC_traceepoch(g);
  tracework(L, epoch, cast(lu_mem, MAX_LMEM), 1);
  return luaC_traceepoch(g) == epoch;
}

/* Each slice returns all claimed records before a poll or ownership handoff.
** Explicit and emergency callers pay the complete job; allocation assists and
** VM polls can supply bounded work without retaining a driver. */
static lu_mem sweepwork (lua_State *L, lu_mem budget, int complete) {
  global_State *g = G(L);
  uint64_t epoch = g->sweep_plan.epoch;
  lu_mem worked = 0;
  int quiescent = g->gcemergency || L->object_scopes != NULL ||
                  luaW_scope(L).heap != 0;
  LuaWorldScope scope = {0};
  if (!quiescent) {
    if (L->world_depth != 0) g->gcstopem = 0;
    scope = luaW_suspend(L);
  }
  while (luaC_sweeping(L) && g->sweep_plan.epoch == epoch) {
    lu_mem done = luaC_sweepbatch(L, LUA_TRACE_BATCH, complete && !quiescent);
    worked = done > cast(lu_mem, MAX_LMEM) - worked
      ? cast(lu_mem, MAX_LMEM) : worked + done;
    if (!complete && (worked >= budget || done == 0)) break;
    if (!quiescent) {
      luaX_pollrequests(L);
      luaQ_poll(L);
      if (done == 0) {
        luaX_checksync(pthread_mutex_lock(&g->world.mutex));
        if (luaC_sweeping(L) && g->sweep_plan.epoch == epoch &&
            g->sweep_plan.busy != 0 && !luaC_sweepready(g))
          luaW_waitcollector(L, LUA_WAIT_PHASE);
        luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
      }
    }
  }
  if (!quiescent) luaW_restore(L, scope, L->native_depth);
  return worked;
}

void luaC_waitsweep (lua_State *L) {
  LuaGCFrame frame;
  if (!luaC_sweeping(L)) return;
  luaC_entergc(L, &frame);
  while (luaC_sweeping(L)) sweepwork(L, cast(lu_mem, MAX_LMEM), 1);
  luaC_leavegc(L, &frame);
}

/* Restoring a caller's quiescent scope can admit a later cycle before the
** scope is reacquired. Recheck both shared jobs before changing collector
** mode; draining marking and sweeping only once leaves that handoff open. */
static void waitcollection (lua_State *L) {
  while (luaC_tracing(L) || luaC_sweeping(L)) {
    if (luaC_tracing(L)) atomic(L);
    luaC_waitsweep(L);
  }
}

static void assistsweep (lua_State *L, int allocating) {
  LuaGCFrame frame;
  lu_mem budget;
  int pressure = allocating && luaC_cyclepressure(L);
  if (!luaC_sweeping(L) || !gcrunning(G(L)) || luaC_ingc(L) ||
      L->reclaiming || L->object_scopes != NULL || luaW_scope(L).heap != 0)
    return;
  budget = allocating && L->trace_debt >= LUA_GC_ASSIST_BYTES
    ? L->trace_debt : 1;
  luaC_entergc(L, &frame);
  {
    /* The shared allowance applies to sweeping as well as marking. A worker
    ** with little local debt must help finish an overdue cohort before its
    ** next allocation; concurrent helpers retain ordinary batch ownership. */
    lu_mem due = pressure || budget == 1 ? budget : luaP_budget(L, budget);
    lu_mem worked = pressure || due != 0 ? sweepwork(L, due, pressure) : 0;
    if (!pressure && due == 0) L->trace_debt = 0;
    L->trace_debt = worked >= L->trace_debt ? 0 : L->trace_debt - worked;
  }
  if (luaC_retired(L) != NULL || L->reclaim_pages) luaC_reclaim(L);
  luaC_leavegc(L, &frame);
}

void luaC_pollsweep (lua_State *L) {
  assistsweep(L, 0);
}

void luaC_assistsweep (lua_State *L) {
  assistsweep(L, 1);
}


/*
** Do a sweep step. The normal case (not fast) sweeps at most GCSWEEPMAX
** elements. The fast case sweeps the whole list.
*/
static int sweepstep (lua_State *L, global_State *g, int fast) {
  uint64_t interrupted = g->trace_interrupt, epoch = luaC_traceepoch(g);
  sweepwork(L, GCSWEEPMAX * sizeof(GCObject), fast);
  return interrupted == g->trace_interrupt && epoch == luaC_traceepoch(g);
}


/*
** Performs one incremental "step" in an incremental garbage collection.
** Marking drives the shared concurrent job; any execution may complete its
** phases. Sweeping retires GCSWEEPMAX objects, or the whole list when 'fast'.
*/

#define step2pause	-3  /* finished collection; entered pause state */
#define atomicstep	-2  /* atomic step */
#define step2minor	-1  /* moved to minor collections */
#define stepinterrupted -4  /* an emergency completed suspended collection */


static l_mem singlestep (lua_State *L, int fast, l_mem budget) {
  global_State *g = G(L);
  l_mem stepresult;
  lua_assert(!g->gcstopem);  /* collector is not reentrant */
  g->gcstopem = 1;  /* no emergency collections while collecting */
  switch (luaC_state(g)) {
    case GCSpause: {
      restartcollection(L, g, TRACE_MAJOR);
      luaC_setstate(g, GCSpropagate);
      stepresult = luaC_tracing(L) ? 0 : 1;
      break;
    }
    case GCSpropagate: {
      lua_assert(luaC_tracing(L));
      luaC_setstate(g, GCSenteratomic);
      stepresult = 0;
      break;
    }
    case GCSenteratomic: case GCSatomic: {
      if (!atomic(L)) {
        stepresult = stepinterrupted;
        break;
      }
      if (luaC_kind(g) == KGC_GENMINOR)
        stepresult = step2minor;
      else
        stepresult = atomicstep;
      break;
    }
    case GCSswpallgc: {  /* sweep every allocation exactly once */
      if (!sweepstep(L, g, fast)) stepresult = stepinterrupted;
      else stepresult = luaC_state(g) == GCSpause ? step2pause : GCSWEEPMAX;
      break;
    }
    case GCScallfin: {  /* call finalizers */
      if (luaC_hasfinalizers(g) && !g->gcemergency && luaD_checkminstack(L)) {
        uint64_t epoch = luaC_traceepoch(g);
        l_mem limit = budget == 0 ? MAX_LMEM : 1 + (budget - 1) / CWUFIN;
        l_mem count;
        g->gcstopem = 0;  /* ok collections during finalizers */
        count = drainfinalizers(L, limit);
        if (luaC_traceepoch(g) != epoch || luaC_state(g) != GCScallfin)
          return stepinterrupted;
        if (luaC_hasfinalizers(g)) {
          stepresult = count > MAX_LMEM / CWUFIN ? MAX_LMEM : count * CWUFIN;
          break;
        }
      }
      /* No pending work can run in this collection. */
      finishincremental(L, g);
      stepresult = step2pause;
      break;
    }
    default: lua_assert(0); return 0;
  }
  g->gcstopem = 0;
  return stepresult;
}


/*
** Advances the garbage collector until it reaches the given state.
** (The option 'fast' is only for testing; in normal code, 'fast'
** here is always true.)
*/
void luaC_runtilstate (lua_State *L, int state, int fast) {
  global_State *g = G(L);
  lua_assert(luaC_kind(g) != KGC_GENMINOR);
  while (state != luaC_state(g) && luaC_kind(g) != KGC_GENMINOR) {
    l_mem result = singlestep(L, fast, 0);
    if (result == stepinterrupted || result == step2minor) return;
  }
}



/*
** Performs a basic incremental step. The step size is
** converted from bytes to "units of work"; then the function loops
** running single steps until adding that many units of work or
** finishing a cycle (pause state). Finally, it sets the debt that
** controls when next step will be performed.
*/
static l_mem incrementalwork (global_State *g, l_mem bytes) {
  return applygcparam(g, STEPMUL, bytes / cast_int(sizeof(void *)));
}

static l_mem incstepwork (global_State *g, l_mem stepsize, l_mem debt) {
  l_mem work = incrementalwork(g, stepsize);
  /* Admission and local accounting can accumulate several workers' bytes
  ** beyond the trigger. Pay for that overshoot before resetting the debt;
  ** otherwise a faster allocator can outrun a fixed tracing quantum forever.
  ** Explicit basic steps pass zero and retain the configured work size. */
  if (work != 0 && debt < 0) {
    l_mem allocated = stepsize > MAX_LMEM + debt ? MAX_LMEM : stepsize - debt;
    work = incrementalwork(g, allocated);
  }
  return work;
}

static lu_mem tracebudget (l_mem work) {
  return work > MAX_LMEM / cast_int(sizeof(void *))
    ? cast(lu_mem, MAX_LMEM) : cast(lu_mem, work * cast_int(sizeof(void *)));
}

static void incstep (lua_State *L, global_State *g, l_mem debt, int concurrent) {
  l_mem stepsize = applygcparam(g, STEPSIZE, 100);
  l_mem work2do = incstepwork(g, stepsize, debt);
  l_mem stres;
  int fast = (work2do == 0);  /* special case: do a full collection */
  do {  /* repeat until enough work */
    if (!fast && luaC_tracing(L)) {
      uint64_t epoch = luaC_traceepoch(g);
      lu_mem budget = tracebudget(work2do);
      if (concurrent && L->world_depth != 0) {
        /* Initialize under this request's existing boundary. The automatic
        ** request borrows no mutable Lua fields while doing graph work, so
        ** it owes no restoration of a global pause when its slice returns. */
        if (luaC_tracephase(g) == TRACE_INIT) luaC_advancetrace(L);
        g->gcstopem = 0;
        luaW_start(L);
      }
      tracework(L, epoch, budget, 0);
      if (concurrent) return;  /* shared completion owns the next idle budget */
      if (luaC_traceepoch(g) != epoch || luaC_kind(g) == KGC_GENMINOR) return;
      break;
    }
    stres = singlestep(L, fast, fast ? 0 : work2do);
    if (stres == stepinterrupted) return;
    if (stres == step2minor)  /* returned to minor collections? */
      return;  /* nothing else to be done here */
    else if (stres == step2pause || (stres == atomicstep && !fast))
      break;  /* end of cycle or atomic */
    else
      work2do -= stres;
  } while (fast || work2do > 0);
  if (luaC_state(g) != GCSpause)
    luaE_setdebt(g, stepsize);
}


#if !defined(luai_tracegc)
#define luai_tracegc(L,f)		((void)0)
#endif

/* Ordinary bounded traces use the barrier handshake. The first major after a
** young trace closes admission so outstanding remembered publications give
** up their intrusive links before rotating the major mark stamp. */
static void preparestep (lua_State *L, int concurrent) {
  global_State *g = G(L);
  if (!concurrent ||
      (luaC_kind(g) != KGC_GENMINOR && (luaC_state(g) != GCSpause ||
        (luaC_markstate(g) & LUA_MARK_YOUNG) ||
        incrementalwork(g, applygcparam(g, STEPSIZE, 100)) == 0)) ||
      (g->gcstp & GCSTPCLS) ||
      luaC_weakstamp(g) > UINT64_MAX - LUA_WEAK_STAMP_STEP) {
    luaW_stop(L);
    luaW_heapleave(L);
  }
}

static void finishrequest (global_State *g) {
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  __atomic_store_n(&g->gcstep_pending, 0, __ATOMIC_RELEASE);
  luaQ_wake(&g->phase_changed, &g->phase_waiters);
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
}

static int markstep (lua_State *L, l_mem debt, int concurrent, int user) {
  global_State *g = G(L);
  int young = luaC_kind(g) == KGC_GENMINOR;
  int arming = L->world_depth == 0;
  l_mem size = applygcparam(g, STEPSIZE, 100);
  lu_mem budget = young ? cast(lu_mem, applygcparam(g, STEPMUL, size))
                       : tracebudget(incstepwork(g, size, debt));
  int complete = young && (user || budget == 0);
  uint64_t epoch;
  lua_assert(young ? luaC_state(g) == GCSpropagate : (arming && budget != 0));
  starttrace(L, young ? TRACE_YOUNG : TRACE_MAJOR, user && !young);
  if (arming && !user) finishrequest(g);
  epoch = luaC_traceepoch(g);
  if (arming) {
    luaW_heapleave(L);
    luaC_checksnapshot(L);
  }
  else if (concurrent) {
    luaC_advancetrace(L);
    g->gcstopem = 0;
    luaW_start(L);
  }
  if (user || complete || luaC_scanready(g))
    tracework(L, epoch, budget, complete);
  if (young && user && luaC_sweeping(L) && luaC_traceepoch(g) == epoch)
    sweepwork(L, cast(lu_mem, MAX_LMEM), 1);
  if (luaC_traceepoch(g) != epoch) return 0;
  if (luaC_tracing(L)) return 0;
  if (!young) return luaC_state(g) == GCSpause || luaC_kind(g) == KGC_GENMINOR;
  if (luaC_sweeping(L)) return 0;
  if (!concurrent) setidledebt(g);
  return 1;
}

/* Consume the step's metadata admission or existing world stop. */
static void runstep (lua_State *L, l_mem debt, int concurrent, int user) {
  global_State *g = G(L);
  LuaGCFrame frame;
  int reclaim = 0;
  luaC_entergc(L, &frame);
  lua_assert(!g->gcemergency);
  luai_tracegc(L, 1);  /* for internal debugging */
  if (L->world_depth == 0 || luaC_kind(g) == KGC_GENMINOR)
    reclaim = markstep(L, debt, concurrent, user);
  else {
    incstep(L, g, debt, concurrent);
    reclaim = !luaC_tracing(L) &&
              (luaC_state(g) == GCSpause || luaC_kind(g) == KGC_GENMINOR);
  }
  luai_tracegc(L, 0);  /* for internal debugging */
  /* Retiring all allocation caches and scanning every page belongs to a
  ** completed collection, not each marking/partial-sweep step. Emergency and
  ** explicit full collections still reclaim before retrying an allocation. */
  if (reclaim) L->reclaim_pages = 1;
  if (L->world_depth != 0) luaW_start(L);
  if (!L->reclaiming && L->world_depth == 0 &&
      (luaC_retired(L) != NULL || L->reclaim_pages))
    luaC_reclaim(L);
  luaC_leavegc(L, &frame);
}


/*
** Performs a basic GC step while automatic collection is enabled.
*/
void luaC_step (lua_State *L) {
  global_State *g = G(L);
  LuaGCFrame frame;
  int automatic = L->world_depth == 0;
  l_mem debt;
  if (luaC_sweeping(L)) {
    l_mem stepsize = applygcparam(g, STEPSIZE, 100);
    l_mem allocated = stepsize;
    l_mem outstanding = L->world_depth == 0
      ? luaE_getdebt(g) : 0;
    lu_mem budget;
    if (stepsize != 0 && outstanding < 0)
      allocated = stepsize > MAX_LMEM + outstanding
        ? MAX_LMEM : stepsize - outstanding;
    budget = cast(lu_mem, applygcparam(g, STEPMUL, allocated));
    uint64_t trace_epoch = luaC_traceepoch(g);
    if ((!gcrunning(g) && !luaC_ingc(L)) ||
        (L->world_depth == 0 &&
         (L->object_scopes != NULL || luaW_scope(L).heap != 0))) return;
    luaC_entergc(L, &frame);
    sweepwork(L, budget, budget == 0);
    if (budget == 0 && luaC_traceepoch(g) == trace_epoch &&
        luaC_kind(g) != KGC_GENMINOR && luaC_state(g) != GCSpause) {
      luaW_stop(L);
      luaC_runtilstate(L, GCSpause, 1);
      luaW_start(L);
    }
    luaC_leavegc(L, &frame);
    if (!L->reclaiming && L->world_depth == 0 &&
        (luaC_retired(L) != NULL || L->reclaim_pages)) luaC_reclaim(L);
    return;
  }
  if (luaC_tracing(L)) { luaC_assisttrace(L); return; }
  if (L->world_depth == 0 &&
      (L->object_scopes != NULL || luaW_scope(L).heap != 0)) return;
  if ((!gcrunning(g) && !luaC_ingc(L)) ||
      (!luaC_needstep(L) && !luaC_cyclepressure(L))) return;
  if (automatic) {
    luaC_collectnursery(L);
    if (!gcrunning(g) || luaC_tracing(L) || luaC_sweeping(L) ||
        (!luaC_needstep(L) && !luaC_cyclepressure(L))) return;
  }
  if (L->world_depth == 0 && luaC_needassist(L))
    luaC_assistreclaim(L);
  if (automatic) {
    unsigned expected = 0;
    /* One automatic request seeks metadata while other mutators keep working. */
    if (!__atomic_compare_exchange_n(&g->gcstep_pending, &expected, 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      if (!luaC_cyclepressure(L)) return;
      /* Allocators at the hard limit wait for the startup decision. They can
      ** assist the resulting cycle, but need no separate metadata grant. */
      luaX_checksync(pthread_mutex_lock(&g->world.mutex));
      if (__atomic_load_n(&g->gcstep_pending, __ATOMIC_ACQUIRE)) {
        L->native_depth++;
        luaW_waitcollector(L, LUA_WAIT_START);
        L->native_depth--;
      }
      luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
      if (luaC_tracing(L)) luaC_assisttrace(L);
      else if (luaC_sweeping(L)) luaC_assistsweep(L);
      return;
    }
  }
  /* An automatic step needs no queued metadata grant until shared allocation
  ** reaches the pressure limit. A full collector preparing its trace can
  ** still need the allocating task to reach a nursery checkpoint. */
  if (automatic && !luaC_cyclepressure(L)) {
    if (!luaW_trycollectenter(L)) {
      finishrequest(g);
      return;
    }
  }
  else luaW_collectenter(L);
  /* Revalidate under metadata ownership, including unpublished local debt. */
  if (luaC_tracing(L) || luaC_sweeping(L) || (!gcrunning(g) && !luaC_ingc(L)) ||
      (!luaC_needstep(L) && !luaC_cyclepressure(L))) {
    if (automatic) finishrequest(g);
    luaW_heapleave(L);
    return;
  }
  preparestep(L, automatic);
  if (automatic && L->world_depth != 0)
    finishrequest(g);  /* followers wait for the world to resume */
  debt = automatic ? luaE_getdebt(g) : 0;
  runstep(L, debt, automatic, 0);
}


static void completefinalizers (lua_State *L, uint64_t epoch) {
  global_State *g = G(L);
  if (luaC_traceepoch(g) != epoch || luaC_state(g) != GCScallfin) return;
  luaW_collectenter(L);
  if (luaC_traceepoch(g) == epoch && luaC_state(g) == GCScallfin &&
      luaD_checkminstack(L)) {
    luaW_heapleave(L);
    luaC_runfinalizers(L);
    luaW_collectenter(L);
  }
  if (luaC_traceepoch(g) == epoch && luaC_state(g) == GCScallfin)
    finishincremental(L, g);
  luaW_heapleave(L);
}

/* Join the current cohort. A basic incremental request lends one slice;
** a young-generation request finishes the minor collection it joined. */
static void stepactive (lua_State *L) {
  global_State *g = G(L);
  uint64_t epoch = luaC_traceepoch(g);
  int marking = luaC_tracing(L);
  int young = marking ? g->trace_plan.finish == TRACE_YOUNG
                       : g->sweep_plan.kind == SWEEP_YOUNG;
  l_mem size = applygcparam(g, STEPSIZE, 100);
  l_mem work = incrementalwork(g, size);
  int complete = young || work == 0;
  if (marking) {
    lu_mem budget = young ? cast(lu_mem, applygcparam(g, STEPMUL, size))
                         : tracebudget(work);
    tracework(L, epoch, budget, complete);
  }
  if (luaC_traceepoch(g) == epoch && luaC_sweeping(L) && (!marking || complete))
    sweepwork(L, cast(lu_mem, applygcparam(g, STEPMUL, size)), complete);
  if (!young && complete) completefinalizers(L, epoch);
  if (!L->reclaiming && L->world_depth == 0 &&
      (luaC_retired(L) != NULL || L->reclaim_pages)) luaC_reclaim(L);
}


typedef struct LuaUserStep {
  uint64_t major, epoch;
} LuaUserStep;

static void userstep (lua_State *L, void *argument) {
  global_State *g = G(L);
  LuaUserStep *step = argument;
  if (!luaC_tracing(L) && !luaC_sweeping(L)) {
    luaW_collectenter(L);
    /* Another admitted request may have started or completed this step
    ** while we waited. Keep the permit only when starting new work. */
    if (!luaC_tracing(L) && !luaC_sweeping(L) && luaC_traceepoch(g) == step->epoch &&
        __atomic_load_n(&g->major_completed, __ATOMIC_ACQUIRE) == step->major) {
      LuaWorldScope scope = luaW_scope(L);
      int concurrent = scope.collection == 0 && scope.heap == 1 &&
                       L->object_scopes == NULL;
      preparestep(L, concurrent);
      runstep(L, 0, concurrent, 1);
      return;
    }
    luaW_heapleave(L);
  }
  if (luaC_tracing(L) || luaC_sweeping(L)) stepactive(L);
}

/* world.mutex orders ownership, completion and wait registration. Odd
** request numbers distinguish the current driver from an older caller
** which may still be unwinding or running post-collection callbacks. */
static void releaseminor (global_State *g) {
  LuaScheduler *s = __atomic_load_n(&g->scheduler, __ATOMIC_ACQUIRE);
  lua_assert(g->minor_request & 1);
  g->minor_request++;
  if (s != NULL) __atomic_store_n(&s->minor_driver, NULL, __ATOMIC_RELEASE);
  luaQ_wake(&g->phase_changed, &g->minor_waiters);
}

void luaC_publishminor (global_State *g) {
  g->minor_completed++;
  if (g->minor_request & 1) releaseminor(g);
}

static int joinminor (lua_State *L, uint64_t *request, int basic) {
  global_State *g = G(L);
  uint64_t completed;
  int assist;
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  completed = g->minor_completed;
  while (g->minor_completed == completed && luaC_kind(g) == KGC_GENMINOR &&
         (!luaC_sweeping(L) || g->sweep_plan.kind == SWEEP_YOUNG)) {
    if (!(g->minor_request & 1)) {
      /* A basic step has no shared minor work when all previous survivors
      ** were promoted or retired and there is no remembered work left. */
      if (basic &&
          __atomic_load_n(&g->young_work, __ATOMIC_ACQUIRE) == 0 &&
          !__atomic_load_n(&g->barrier_ready, __ATOMIC_ACQUIRE) &&
          !__atomic_load_n(&g->shrink_strings, __ATOMIC_RELAXED) &&
          !luaC_tracing(L) && !luaC_sweeping(L) && !luaW_stopped(&g->world)) {
        luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
        return 0;
      }
      *request = ++g->minor_request;
      if (!luaC_tracing(L) && !luaC_sweeping(L)) luaSC_leadminor(L);
      break;
    }
    /* An abandoned request publishes no completion. Try again as a driver. */
    luaW_waitcollector(L, LUA_WAIT_MINOR);
  }
  assist = g->minor_completed == completed;
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  return assist;
}

int luaC_userstep (lua_State *L, l_mem bytes) {
  global_State *g = G(L);
  LuaUserStep step = {__atomic_load_n(&g->major_completed, __ATOMIC_ACQUIRE),
                      luaC_traceepoch(g)};
  LuaGCFrame frame;
  int status = LUA_OK;
  if (g->gcstp & (GCSTPGC | GCSTPCLS)) return -1;
  if (bytes > 0) {
    luaP_spend(g, bytes);
    if (!luaC_needstep(L)) return 0;
  }
  luaC_entergc(L, &frame);
  if (luaC_kind(g) == KGC_GENMINOR && L->object_scopes == NULL &&
      !luaW_ownsheap(L)) {
    uint64_t request = 0;
    if (joinminor(L, &request, bytes == 0)) {
      step.epoch = luaC_traceepoch(g);
      if (request == 0) userstep(L, &step);
      else {
        status = luaD_rawrunprotected(L, userstep, &step);
        luaX_checksync(pthread_mutex_lock(&g->world.mutex));
        if (g->minor_request == request) releaseminor(g);
        luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
      }
    }
  }
  else userstep(L, &step);
  luaC_leavegc(L, &frame);
  if (status != LUA_OK) luaD_throw(L, status);
  return __atomic_load_n(&g->major_completed, __ATOMIC_ACQUIRE) != step.major;
}


/*
** Perform a full collection in incremental mode.
** A new major rotates its stamp, so no preparatory whitening sweep is needed.
*/
static void fullinc (lua_State *L, global_State *g) {
  uint64_t interrupted = g->trace_interrupt;
  /* finish any pending sweep phase to start a new cycle */
  luaC_runtilstate(L, GCSpause, 1);
  if (g->trace_interrupt != interrupted || luaC_kind(g) == KGC_GENMINOR) return;
  singlestep(L, 1, 0);  /* begin exactly one fresh cycle */
  luaC_runtilstate(L, GCSpause, 1);  /* finish collection */
}

/* Each phase can be completed by a helper. Check the trace epoch before
** driving the next phase so a caller never takes over a later collection. */
static void completecycle (lua_State *L, uint64_t epoch) {
  global_State *g = G(L);
  if (luaC_traceepoch(g) != epoch) return;
  if (luaC_tracing(L)) tracework(L, epoch, cast(lu_mem, MAX_LMEM), 1);
  if (luaC_traceepoch(g) == epoch && luaC_sweeping(L))
    sweepwork(L, cast(lu_mem, MAX_LMEM), 1);
  completefinalizers(L, epoch);
}

/* Publish the upcoming epoch before sealing its first nursery. A follower
** entering during preparation can already have objects in a new cohort
** and must wait for a fresh full cycle. Metadata ownership prevents other
** cycles from starting before this epoch has been installed. */
static uint64_t startfullcycle (lua_State *L) {
  global_State *g = G(L);
  unsigned finish = TRACE_MAJOR;
  uint64_t epoch = luaC_traceepoch(g) + 1;
  __atomic_store_n(&g->full_trace_epoch, epoch, __ATOMIC_RELEASE);
  luaC_preparefull(L);
  luaW_stop(L);
  luaW_heapleave(L);
  if (luaC_kind(g) == KGC_GENMINOR) {
    minor2inc(g, KGC_INC);
    finish = TRACE_GENERATIONAL;
  }
  starttrace(L, finish, 0);
  luaC_advancetrace(L);
  lua_assert(epoch == luaC_traceepoch(g));
  __atomic_fetch_and(&g->world.control, ~cast(uint64_t, LUA_WORLD_SEAL_NURSERIES),
                     __ATOMIC_SEQ_CST);
  luaW_start(L);
  return epoch;
}

/* Wait for a cycle that starts after this caller entered. Drivers own no
** private continuation, so a follower can finish the published cycle even
** if its starter parks or unwinds. */
static void fullconcurrent (lua_State *L) {
  global_State *g = G(L);
  uint64_t before = __atomic_load_n(&g->full_trace_epoch, __ATOMIC_ACQUIRE);
  uint64_t epoch, interrupted = g->trace_interrupt;
  int started = 0;
  for (;;) {
    luaW_collectenter(L);
    epoch = __atomic_load_n(&g->full_trace_epoch, __ATOMIC_RELAXED);
    if (epoch != before) {
      luaW_heapleave(L);
      break;
    }
    if (!luaC_tracing(L) && !luaC_sweeping(L) && luaC_state(g) != GCScallfin) {
      interrupted = g->trace_interrupt;
      epoch = startfullcycle(L);
      started = 1;
      break;
    }
    epoch = luaC_traceepoch(g);
    luaW_heapleave(L);
    completecycle(L, epoch);
  }
  completecycle(L, epoch);
  if (g->trace_interrupt != interrupted && luaC_hasfinalizers(g))
    luaC_runfinalizers(L);
  if (started) {
    trimstacks(L);
    L->reclaim_pages = 1;
  }
  if (luaC_retired(L) != NULL || L->reclaim_pages) luaC_reclaim(L);
}

/*
** Performs a full GC cycle; if 'isemergency', set a flag to avoid
** some operations which could change the interpreter state in some
** unexpected ways (running finalizers and shrinking some structures).
** Emergency retries complete the shared trace under quiescence, including
** fields held by the failing allocator, before returning reclaimed storage.
*/
void luaC_fullgc (lua_State *L, int isemergency) {
  global_State *g = G(L);
  LuaGCFrame frame;
  int kind;
  uint64_t interrupted;
  if (!isemergency && L->world_depth == 0 && !luaW_ownsheap(L) &&
      L->object_scopes == NULL && !L->reclaiming) {
    luaC_entergc(L, &frame);
    fullconcurrent(L);
    luaC_leavegc(L, &frame);
    luaHS_reclaim(L, LUAHS_DRAIN);
    return;
  }
  luaW_stop(L);
  luaC_sealtasks(L);
  luaC_entergc(L, &frame);
  lua_assert(!g->gcemergency);
  interrupted = g->trace_interrupt;
  g->gcemergency = cast_byte(isemergency);  /* set flag */
  if (isemergency) luaM_drainsparepages(L);
  if (isemergency && luaC_sweeping(L)) g->trace_interrupt++;
  if (isemergency && luaC_tracing(L)) {
    g->trace_interrupt++;
    L->trace_snapshot = 0;
    luaC_snapshot(L, L);
  }
  waitcollection(L);
  kind = luaC_kind(g);
  switch (kind) {
    case KGC_GENMINOR: fullgen(L, g); break;
    case KGC_INC: case KGC_GENMAJOR: fullinc(L, g); break;
  }
  luaC_waitsweep(L);
  L->reclaim_pages = 1;
  /* An allocation retry may still borrow its caller's stack/object storage.
  ** Finish emergency destruction before relinquishing that quiescent scope. */
  if (isemergency) {
    luaHS_reclaim(L, LUAHS_DRAIN);
    luaC_reclaim(L);
  }
  g->gcemergency = 0;
  if (!isemergency && g->trace_interrupt != interrupted && luaC_hasfinalizers(g))
    luaC_runfinalizers(L);
  if (!isemergency) trimstacks(L);
  luaW_start(L);
  if (!L->reclaiming && L->world_depth == 0 &&
      (luaC_retired(L) != NULL || L->reclaim_pages))
    luaC_reclaim(L);
  luaC_leavegc(L, &frame);
  luaHS_reclaim(L, LUAHS_DRAIN);
}

/* }====================================================== */


