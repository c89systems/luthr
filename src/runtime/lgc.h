/*
** $Id: lgc.h $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#ifndef lgc_h
#define lgc_h


#include <stddef.h>


#include "lobject.h"
#include "lstate.h"
#include "lmempage.h"

/* Allocation membership is independent of finalization and trace queues.
** The embedded main thread alone has no allocator prefix; it is always the
** last allocation and its header link is never used by other queues. */
static inline GCObject **luaC_alloclink (GCObject *o) {
  char *memory = cast_charp(o);
  if (o->tt == LUA_VTHREAD) {
    if (o == obj2gco(mainthread(G(gco2th(o))))) return &o->next;
    memory -= offsetof(LX, l);
  }
  return &(cast(LuaAllocBlock *, memory) - 1)->block.link.object;
}

#define luaC_allocnext(o) (*luaC_alloclink(o))

static inline GCObject **luaC_graylink (GCObject *o) {
  switch (o->tt) {
    case LUA_VTABLE: return &gco2t(o)->gclist;
    case LUA_VLCL: return &gco2lcl(o)->gclist;
    case LUA_VCCL: return &gco2ccl(o)->gclist;
    case LUA_VTHREAD: return &gco2th(o)->gclist;
    case LUA_VPROTO: return &gco2p(o)->gclist;
    case LUA_VUPVAL: return &gco2upv(o)->gclist;
    case LUA_VUSERDATA: return &gco2u(o)->gclist;
    default: lua_assert(0); return NULL;
  }
}

typedef struct LuaGCFrame {
  struct LuaGCFrame *previous;
  global_State *global;
  lua_State *reclaimer;  /* stable owner of this batch's destruction queue */
  GCObject *retired, *retired_tail;
  l_mem retired_bytes;
  int batch;
} LuaGCFrame;

enum {
  TRACE_INIT, TRACE_PENDING, TRACE_MARK, TRACE_EPHEMERONS, TRACE_VALUES, TRACE_FINALIZERS,
  TRACE_CLEAR, TRACE_VERIFY
};
enum { TRACE_MAJOR = 1, TRACE_YOUNG, TRACE_GENERATIONAL };
#define luaC_tracephase(g) \
  __atomic_load_n(&(g)->trace_plan.phase, __ATOMIC_ACQUIRE)
#define luaC_traceepoch(g) __atomic_load_n(&(g)->trace_epoch, __ATOMIC_ACQUIRE)
#define luaC_weakstamp(g) __atomic_load_n(&(g)->weak_stamp, __ATOMIC_RELAXED)
#define luaC_waitingstacks(g) \
  ((luaW_control(&(g)->world) & LUA_WORLD_STACK_ROOTS) != 0)
#define luaC_tracerunnable(g) \
  (__atomic_load_n(&(g)->trace_ready, __ATOMIC_ACQUIRE) || \
   __atomic_load_n(&(g)->barrier_ready, __ATOMIC_ACQUIRE))
#define luaC_tracepending(g) (luaC_tracerunnable(g) || luaC_waitingstacks(g))

l_sinline int luaC_traceblocked (global_State *g) {
  return __atomic_load_n(&g->trace_advance, __ATOMIC_ACQUIRE) ||
         __atomic_load_n(&g->trace_busy, __ATOMIC_ACQUIRE) ||
         (luaC_waitingstacks(g) && !luaC_tracerunnable(g));
}

/* Publish the allocation and mark stamps together with trace activation.
** Inactive states retain the last trace's interpretation for sweeping. */
enum {
  LUA_MARK_YOUNG = 1u << 8, LUA_MARK_ACTIVE = 1u << 9, LUA_MARK_ARMING = 1u << 10
};
#define luaC_markstate(g) __atomic_load_n(&(g)->mark_state, __ATOMIC_RELAXED)
#define luaC_tracingglobal(g) \
  ((__atomic_load_n(&(g)->mark_state, __ATOMIC_ACQUIRE) & LUA_MARK_ACTIVE) != 0)
l_sinline int luaC_scanready (global_State *g) {
  const unsigned bits = LUA_MARK_ACTIVE | LUA_MARK_ARMING;
  return (__atomic_load_n(&g->mark_state, __ATOMIC_ACQUIRE) & bits) ==
    LUA_MARK_ACTIVE;
}

/* Idle workers consume payloads only. Reservation retries and phase changes
** remain with ordinary collector drivers; neither is runnable idle work. */
#define luaC_markready(g) \
  (__atomic_load_n(&(g)->trace_workers, __ATOMIC_ACQUIRE) || \
   __atomic_load_n(&(g)->barrier_ready, __ATOMIC_ACQUIRE))
#define luaC_workerready(g) \
  ((luaC_scanready(g) && luaC_markready(g)) || \
   __atomic_load_n(&(g)->sweep_workers, __ATOMIC_ACQUIRE))

/* A batch owns its payloads until it publishes every list and counter. It
** must not yield or park while these private references are outstanding. */
#define LUA_TRACE_BATCH 64
#define LUA_TRACE_PAGE_SLICES 4
typedef struct LuaTracePageSlice {
  LuaAllocPage *page;
  uint64_t bits;
  unsigned word;
} LuaTracePageSlice;

typedef struct LuaTraceWork {
  global_State *global;
  LuaTraceList gray, grayagain, weak, allweak, ephemeron, deferred, stacks;
  GCObject *retry, *retry_tail;
  GCObject *objects[LUA_TRACE_BATCH];
  uint64_t claims;
  l_mem marked;
  lu_mem scanned;
  unsigned count, object_count, phase, direction, changed;
  GCObject *local;  /* discoveries that fit in this claim's remaining budget */
  unsigned local_remaining;
  LuaTracePageSlice pages[LUA_TRACE_PAGE_SLICES];
  unsigned page_count;
  LuaAllocPage *staged_page;
  uint64_t staged_bits;
  unsigned staged_word;
} LuaTraceWork;

l_sinline int luaC_pagetracepending (global_State *g) {
  return g->trace_pages != NULL ||
    __atomic_load_n(&g->trace_page_incoming, __ATOMIC_ACQUIRE) != NULL;
}

LUAI_FUNC void luaC_queuetracepage (global_State *g, GCObject *object);
LUAI_FUNC void luaC_stagetracepage (LuaTraceWork *work, GCObject *object);
LUAI_FUNC void luaC_flushtracepage (LuaTraceWork *work);
LUAI_FUNC void luaC_claimtracepage (LuaTraceWork *work, unsigned limit);

typedef struct LuaSweepWork {
  LuaAllocPage *page;
  GCObject *next, *live, **tail;
  GCObject *retry, **retry_tail, *threads, **threads_tail;
  LuaTraceList watch;
  l_mem addedold, freed;
  unsigned count, kind, phase, region, tables, minor_work;
  int full_page;
} LuaSweepWork;

LUAI_FUNC LuaGCFrame *luaC_gcframe (void);
LUAI_FUNC void luaC_restoregcframe (LuaGCFrame *frame);
LUAI_FUNC int luaC_ingc (lua_State *L);
LUAI_FUNC void luaC_entergc (lua_State *L, LuaGCFrame *frame);
LUAI_FUNC void luaC_leavegc (lua_State *L, LuaGCFrame *frame);

/*
** Collectable objects may have one of three colors: white, which means
** the object is not marked; gray, which means the object is marked, but
** its references may be not marked; and black, which means that the
** object and all its references are marked.  The main invariant of the
** garbage collector, while marking objects, is that a black object can
** never point to a white one. Moreover, any gray object must be in a
** "gray list" (gray, grayagain, weak, allweak, ephemeron), or claimed
** for a coroutine's collector publication queue, so that it
** can be visited again before finishing the collection cycle. (Open
** upvalues are an exception to this rule, as they are attached to
** a corresponding thread.)  These lists have no meaning when the
** invariant is not being enforced (e.g., sweep phase).
*/


/*
** Possible states of the Garbage Collector
*/
#define GCSpropagate	0
#define GCSenteratomic	1
#define GCSatomic	2
#define GCSswpallgc	3
#define GCScallfin	4
#define GCSpause	5


#define luaC_state(g) __atomic_load_n(&(g)->gcstate, __ATOMIC_RELAXED)
#define luaC_kind(g) __atomic_load_n(&(g)->gckind, __ATOMIC_RELAXED)
#define luaC_setstate(g,s) __atomic_store_n(&(g)->gcstate, (s), __ATOMIC_RELAXED)
#define luaC_setkind(g,k) __atomic_store_n(&(g)->gckind, (k), __ATOMIC_RELAXED)
#define issweepphase(g)  (luaC_state(g) == GCSswpallgc)


/*
** some useful bit tricks
*/
#define resetbits(x,m) __atomic_fetch_and(&(x), cast_byte(~(m)), __ATOMIC_RELAXED)
#define setbits(x,m) __atomic_fetch_or(&(x), (m), __ATOMIC_RELAXED)
#define testbits(x,m)		((x) & (m))
#define bitmask(b)		(1<<(b))
#define bit2mask(b1,b2)		(bitmask(b1) | bitmask(b2))
#define l_setbit(x,b)		setbits(x, bitmask(b))
#define resetbit(x,b)		resetbits(x, bitmask(b))
#define testbit(x,b)		testbits(x, bitmask(b))


/*
** The two major stamp bits rotate between three nonzero values. Zero belongs
** to fixed objects. Minor marking has its own parity, so old objects need not
** be touched by young collections. Queue ownership is independent of either
** stamp and of generational age. Clearing QUEUEDBIT releases the intrusive
** link to the next claimant. Claim that link with an acquire RMW.
*/
#define MAJOR0BIT	3
#define MAJOR1BIT	4
#define MINORBIT	5
#define FINALIZEDBIT	6  /* object has been marked for finalization */
#define QUEUEDBIT	7



#define MAJORBITS	bit2mask(MAJOR0BIT, MAJOR1BIT)
#define MARKSTAMPS	(MAJORBITS | bitmask(MINORBIT))


#define getmarked(x) __atomic_load_n(&(x)->marked, __ATOMIC_RELAXED)
/* Diagnostic reads can run outside metadata ownership. An exchange also
** makes these atomic writes visible to Helgrind's instruction-level model. */
#define setmarked(x,m) \
  ((void)__atomic_exchange_n(&(x)->marked, (m), __ATOMIC_RELAXED))
#define isgray(x) testbit(getmarked(x), QUEUEDBIT)

#define tofinalize(x)	testbit(getmarked(x), FINALIZEDBIT)

#define nw2black(x) \
  ((void)__atomic_fetch_and(&(x)->marked, cast_byte(~bitmask(QUEUEDBIT)), \
                            __ATOMIC_RELEASE))


/* object age in generational mode */
#define G_NEW		0	/* created in current cycle */
#define G_SURVIVAL	1	/* created in previous cycle */
#define G_SURVIVAL_DIRTY 2  /* survivor received a younger edge */
#define G_SURVIVAL_CLEAN 3  /* survivor scanned without young edges */
#define G_TOUCHED0      4  /* old owner written during concurrent marking */
#define G_OLD           5  /* old object (not to be visited) */
#define G_TOUCHED1      6  /* old object to scan in two minor cycles */
#define G_TOUCHED2      7  /* old object to scan in one minor cycle */

#define AGEBITS		7  /* all age bits (111) */
#define luaC_newmark(g) cast_byte(luaC_markstate(g) & (MARKSTAMPS | AGEBITS))

#define getage(o)	(getmarked(o) & AGEBITS)
static inline void luaC_setage (GCObject *o, lu_byte age) {
  lu_byte marked = getmarked(o), changed;
  do {
    changed = cast_byte((marked & ~AGEBITS) | age);
  } while (!__atomic_compare_exchange_n(&o->marked, &marked, changed, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}
#define setage(o,a) luaC_setage(obj2gco(o), (a))
#define isold(o)	(getage(o) > G_SURVIVAL_CLEAN)

static inline int luaC_unmarked (unsigned state, lu_byte marked) {
  if (state & LUA_MARK_YOUNG)
    return (marked & AGEBITS) <= G_SURVIVAL_CLEAN &&
           ((marked ^ state) & bitmask(MINORBIT)) != 0;
  return (marked & MAJORBITS) != 0 &&
         ((marked ^ state) & MAJORBITS) != 0;
}
#define iswhite(g,o) luaC_unmarked(luaC_markstate(g), getmarked(o))
#define isblack(g,o) (!iswhite((g),(o)) && !isgray(o))

static inline int luaC_deadmark (global_State *g, const lu_byte *marked) {
  unsigned state = __atomic_load_n(&g->mark_state, __ATOMIC_ACQUIRE);
  return !(state & LUA_MARK_ACTIVE) &&
         luaC_unmarked(state, __atomic_load_n(marked, __ATOMIC_RELAXED));
}

enum { NURSERY_TAG = 1, NURSERY_ROOT = 2, NURSERY_LIVE = 4, NURSERY_TASK = 8,
       NURSERY_STEP = 16, NURSERY_TASKOLD = 16, NURSERY_TASK_EDGES = 32 };

l_sinline int luaC_tasktablestate (uint64_t state) {
  return (state & (NURSERY_TAG | NURSERY_TASK)) == (NURSERY_TAG | NURSERY_TASK);
}

l_sinline int luaC_tasktable (const Table *table) {
  return luaC_tasktablestate(__atomic_load_n(&table->gcstate, __ATOMIC_ACQUIRE));
}

l_sinline int luaC_taskstring (const GCObject *object) {
  return object->tt == LUA_VLNGSTR &&
         (__atomic_load_n(&gco2ts(object)->extra, __ATOMIC_ACQUIRE) & LSTR_TASK);
}

l_sinline int luaC_taskobject (const GCObject *object) {
  return object->tt == LUA_VTABLE ? luaC_tasktable(gco2t(object))
                                 : luaC_taskstring(object);
}

l_sinline int luaC_objectdead (global_State *g, const GCObject *object) {
  return luaC_deadmark(g, &object->marked) &&
         !luaC_taskobject(object);
}
#define isdead(g,o) luaC_objectdead((g), cast(const GCObject *, (o)))

static inline lu_byte luaC_stamp (unsigned state, lu_byte marked) {
  unsigned bits = state & LUA_MARK_YOUNG ? bitmask(MINORBIT)
                                       : MARKSTAMPS | AGEBITS;
  return cast_byte((marked & ~bits) | (state & bits));
}

/* Intern resurrection owns the shard, while a sweeper rechecks under the
** same ownership. A leaf is immediately marked and owns no trace queue. */
static inline void luaC_revive (global_State *g, GCObject *o) {
  unsigned state = luaC_markstate(g);
  lu_byte marked = getmarked(o), changed;
  do {
    changed = cast_byte(luaC_stamp(state, marked) & ~bitmask(QUEUEDBIT));
  } while (!__atomic_compare_exchange_n(&o->marked, &marked, changed, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}


/*
** New and survival objects are young. Promotion remembers objects with
** outgoing edges for two subsequent young collections unless the mark found
** only old edges. An inserted young edge vetoes that proof until sweep. A child
** born during the parent's sweep lies outside that frozen cohort and needs
** both scans before it matures. A major selected for generational execution
** stamps its objects and allocations old during marking. The quiescent switch
** then enables remembered barriers before any young allocation or write, so
** conversion needs no later whole-heap promotion scan.
**
** An old-to-young write remembers its owner, independently of marking its
** referent. TOUCHED1 and TOUCHED2 cover two subsequent minor scans. Writes
** during marking start at TOUCHED0: a scan in the current cycle cannot count
** as one of those two future scans when the child was allocated black.
** Gray weak tables and watched threads already own their collection link.
*/


/*
** {======================================================
** Default Values for GC parameters
** =======================================================
*/

/*
** Minor collections will shift to major ones after LUAI_MINORMAJOR%
** bytes become old.
*/
#define LUAI_MINORMAJOR         70

/*
** Major collections will shift to minor ones after a collection
** collects at least LUAI_MAJORMINOR% of the new bytes.
*/
#define LUAI_MAJORMINOR         50

/*
** A young (minor) collection will run after creating LUAI_GENMINORMUL%
** new bytes.
*/
#define LUAI_GENMINORMUL         20


/* incremental */

/* Number of bytes must be LUAI_GCPAUSE% before starting new cycle */
#define LUAI_GCPAUSE    250

/*
** Step multiplier: The collector handles LUAI_GCMUL% work units for
** each new allocated word. (Each "work unit" corresponds roughly to
** sweeping one object or traversing one slot.)
*/
#define LUAI_GCMUL      200

/* How many bytes to allocate before next GC step */
#define LUAI_GCSTEPSIZE	(200 * sizeof(Table))


#define setgcparam(g,p,v)  (g->gcparams[LUA_GCP##p] = luaO_codeparam(v))
#define applygcparam(g,p,x)  luaO_applyparam(g->gcparams[LUA_GCP##p], x)

/* }====================================================== */


/*
** Control when GC is running:
*/
#define GCSTPUSR	1  /* bit true when GC stopped by user */
#define GCSTPGC		2  /* bit true when GC stopped by itself */
#define GCSTPCLS	4  /* bit true when closing Lua state */
#define gcrunning(g)	((g)->gcstp == 0)


/*
** Does one step of collection when debt becomes zero. 'pre'/'pos'
** allows some adjustments to be done only when needed. macro
** 'condchangemem' is used only for heavy tests (forcing a full
** GC cycle on every opportunity)
*/

#if !defined(HARDMEMTESTS)
#define condchangemem(L,pre,pos,emg)	((void)0)
#else
#define condchangemem(L,pre,pos,emg)  \
	{ if (gcrunning(G(L))) { pre; luaC_fullgc(L, emg); pos; } }
#endif

#define luaC_needstep(L) \
    (luaE_getdebt(G(L)) <= -luaM_shareddebt(L))

#define LUA_GC_ASSIST_BYTES 65536
#define luaC_retired(L) __atomic_load_n(&(L)->retired, __ATOMIC_ACQUIRE)
#define luaC_hasreclaimers(L) \
    (__atomic_load_n(&G(L)->world.reclaimers, __ATOMIC_ACQUIRE) != NULL)
#define luaC_needassist(L) \
    ((L)->assist_debt >= LUA_GC_ASSIST_BYTES && luaC_hasreclaimers(L))
#define luaC_cyclepressure(L) \
    (luaP_debt(G(L)) <= -luaM_shareddebt(L))
#define luaC_needtraceassist(L) \
    (luaC_tracing(L) && gcrunning(G(L)) && \
     !(L)->reclaiming && \
     ((L)->trace_debt >= LUA_GC_ASSIST_BYTES || luaC_cyclepressure(L)))

#define luaC_condGC(L,pre,pos) \
    { if (!luaC_ingc(L)) { \
      if (luaC_sweeping(L) && gcrunning(G(L)) && \
          ((L)->trace_debt >= LUA_GC_ASSIST_BYTES || luaC_cyclepressure(L))) \
        { pre; luaC_assistsweep(L); pos; } \
      else if (luaC_needtraceassist(L)) { pre; luaC_assisttrace(L); pos; } \
      else { \
        if (gcrunning(G(L)) && luaC_needassist(L)) \
          { pre; luaC_assistreclaim(L); pos; } \
        if (!luaC_tracing(L) && !luaC_sweeping(L) && \
          gcrunning(G(L)) && (luaC_needstep(L) || luaC_cyclepressure(L)) && \
          ((L)->world_depth != 0 || \
           !__atomic_load_n(&G(L)->gcstep_pending, __ATOMIC_RELAXED) || \
           luaC_cyclepressure(L))) \
          { pre; luaC_step(L); pos; } } \
	  condchangemem(L,pre,pos,0); } }

/* more often than not, 'pre'/'pos' are empty */
#define luaC_checkGC(L)		luaC_condGC(L,(void)0,(void)0)

enum {
  LUA_WEAK_MODE_SHIFT = 1,
  LUA_WEAK_CLEARED_SHIFT = 3,
  LUA_WEAK_VALUES_LATE = 32,
  LUA_WEAK_STAMP_STEP = 64
};

#define luaC_tablestate(t) __atomic_load_n(&(t)->gcstate, __ATOMIC_ACQUIRE)
#define luaC_settablestate(t,v) __atomic_store_n(&(t)->gcstate, (v), __ATOMIC_RELEASE)

LUAI_FUNC int luaC_taskedge (lua_State *L, GCObject *owner, GCObject *value);
LUAI_FUNC int luaC_taskallocate (lua_State *L, GCObject *object, size_t bytes);
LUAI_FUNC void luaC_sealtask (lua_State *L);
LUAI_FUNC void luaC_sealtasks (lua_State *L);
LUAI_FUNC void luaC_preparefull (lua_State *L);
LUAI_FUNC void luaC_pollseal (lua_State *L);
/* world.mutex, after the task's nursery has been drained */
LUAI_FUNC void luaC_retireseal (lua_State *L);
LUAI_FUNC void luaC_endtask (lua_State *L);
LUAI_FUNC void luaC_taskbacking (lua_State *L, Table *table, size_t bytes);
LUAI_FUNC void luaC_taskresize (lua_State *L, Table *table, l_mem change);
LUAI_FUNC void luaC_taskstep (lua_State *L);
LUAI_FUNC void luaC_freetaskroots (lua_State *L);
LUAI_FUNC void luaC_collectnursery (lua_State *L);

l_sinline size_t luaC_taskrootsize (unsigned capacity) {
  return capacity * (sizeof(uintptr_t) + 2 * sizeof(uint16_t));
}

l_sinline size_t luaC_taskrootbytes (const lua_State *L) {
  return L->task_root_capacity > LUA_TASK_INLINE_ROOTS
    ? luaC_taskrootsize(L->task_root_capacity) : 0;
}

l_sinline int luaC_taskrootpressure (const lua_State *L) {
  return L->task_root_count >= L->task_root_capacity - 1;
}

l_sinline int luaC_weakvalid (global_State *g, uint64_t state) {
  return (state & (~cast(uint64_t, LUA_WEAK_STAMP_STEP - 1) | NURSERY_TAG)) ==
         luaC_weakstamp(g);
}

l_sinline unsigned luaC_weakmode (uint64_t state) {
  return (state & NURSERY_TAG) ? 0 : cast_uint(state >> LUA_WEAK_MODE_SHIFT) & 3;
}

l_sinline int luaC_strongtable (global_State *g, Table *table) {
  uint64_t state = luaC_tablestate(table);
  if (luaC_weakvalid(g, state)) return luaC_weakmode(state) == 0;
  return __atomic_load_n(&table->metatable, __ATOMIC_ACQUIRE) == NULL;
}

l_sinline int luaC_nurserytable (lua_State *L, Table *t) {
  return __atomic_load_n(&t->sharing, __ATOMIC_ACQUIRE) == cast(uintptr_t, L) &&
         (luaC_tablestate(t) & ~cast(uint64_t, NURSERY_ROOT | NURSERY_LIVE)) ==
           L->nursery_epoch;
}

/* Remember incoming edges from objects that survive independently of this
** unpublished cohort. Edges inside the cohort will be traced locally. */
l_sinline void luaC_nurseryedge (lua_State *L, GCObject *owner, GCObject *value) {
  if (value->tt == LUA_VTABLE && luaC_nurserytable(L, gco2t(value)) &&
      (owner->tt != LUA_VTABLE || !luaC_nurserytable(L, gco2t(owner)))) {
    Table *t = gco2t(value);
    luaC_settablestate(t, luaC_tablestate(t) | NURSERY_ROOT);
  }
}

static inline int luaC_barrierneeded (lua_State *L, GCObject *owner,
                                     GCObject *value) {
  lu_byte marked, referent;
  if (L->task_objects != NULL && luaC_taskedge(L, owner, value)) return 0;
  /* The shared trace publishes all newborns before local work can resume. */
  if (luaC_tracingglobal(G(L))) return 1;
  luaC_nurseryedge(L, owner, value);
  if (luaC_kind(G(L)) != KGC_GENMINOR) return 0;
  marked = getmarked(owner);
  if ((marked & AGEBITS) <= G_SURVIVAL) return 0;
  referent = getmarked(value);
  return (referent & AGEBITS) <= G_SURVIVAL_CLEAN;
}

#define luaC_objbarrier(L,p,o) \
        (luaC_barrierneeded(L, obj2gco(p), obj2gco(o)) ? \
         luaC_syncbarrier(L, obj2gco(p), obj2gco(o)) : cast_void(0))

#define luaC_barrier(L,p,v) (  \
	iscollectable(v) ? luaC_objbarrier(L,p,gcvalue(v)) : cast_void(0))

#define luaC_objbarrierback(L,p,o) luaC_objbarrier(L,p,o)

#define luaC_barrierback(L,p,v) (  \
	iscollectable(v) ? luaC_objbarrierback(L, p, gcvalue(v)) : cast_void(0))

LUAI_FUNC void luaC_fix (lua_State *L, GCObject *o);
LUAI_FUNC void luaC_freeallobjects (lua_State *L);
LUAI_FUNC void luaC_step (lua_State *L);
LUAI_FUNC int luaC_userstep (lua_State *L, l_mem bytes);
LUAI_FUNC void luaC_publishminor (global_State *g);

enum { LUA_YOUNG_WORK = 1, LUA_YOUNG_SURVIVOR = 2 };

/* Clear pending work only after the stopped sweep takes responsibility for
** the new objects, remembered edges, finalizers, and policy changes. */
l_sinline void luaC_noteyoung (global_State *g) {
  if (!(__atomic_load_n(&g->young_work, __ATOMIC_RELAXED) & LUA_YOUNG_WORK))
    __atomic_fetch_or(&g->young_work, LUA_YOUNG_WORK, __ATOMIC_RELEASE);
}
LUAI_FUNC void luaC_runtilstate (lua_State *L, int state, int fast);
LUAI_FUNC void luaC_fullgc (lua_State *L, int isemergency);
enum { SWEEP_MAJOR, SWEEP_YOUNG, SWEEP_OLD };
enum { SWEEP_OBJECTS, SWEEP_THREADS, SWEEP_GRAY, SWEEP_DONE };
enum { SWEEP_RETRY, SWEEP_LIVE, SWEEP_RETIRED, SWEEP_WATCH };
#define luaC_sweeping(L) \
  __atomic_load_n(&G(L)->sweep_plan.active, __ATOMIC_ACQUIRE)
LUAI_FUNC void luaC_beginsweep (lua_State *L, unsigned kind, l_mem marked);
LUAI_FUNC int luaC_sweepready (global_State *g);
LUAI_FUNC lu_mem luaC_sweepbatch (lua_State *L, unsigned limit, int complete);
LUAI_FUNC lu_mem luaC_sweeppayload (lua_State *L, unsigned limit);
LUAI_FUNC void luaC_scansweep (lua_State *L, LuaSweepWork *work);
LUAI_FUNC void luaC_waitsweep (lua_State *L);
LUAI_FUNC void luaC_pollsweep (lua_State *L);
LUAI_FUNC void luaC_assistsweep (lua_State *L);
LUAI_FUNC int luaC_sweepobject (lua_State *L, GCObject *object, unsigned kind,
                              LuaSweepWork *work);
LUAI_FUNC void luaC_endsweep (lua_State *L);
LUAI_FUNC void luaC_sweepmaintenance (lua_State *L);
LUAI_FUNC int luaC_tryretire (lua_State *L, GCObject *object);
/* Publish a complete private batch while holding world.mutex. */
LUAI_FUNC void luaC_publishretired (lua_State *L, LuaGCFrame *frame);
LUAI_FUNC int luaC_sweepgray (GCObject *object);
LUAI_FUNC GCObject *luaC_newobj (lua_State *L, lu_byte tt, size_t sz);
LUAI_FUNC GCObject *luaC_newobjdt (lua_State *L, lu_byte tt, size_t sz,
                                                 size_t offset);
LUAI_FUNC GCObject *luaC_newresultstring (lua_State *L, size_t size);
LUAI_FUNC void luaC_publishobjects (lua_State *L, GCObject *head, GCObject *tail);
LUAI_FUNC void luaC_syncbarrier (lua_State *L, GCObject *o, GCObject *v);
/* Collector-only: participants have finished all claimed publications. */
LUAI_FUNC void luaC_drainbarriers (lua_State *L);
LUAI_FUNC void luaC_publishbarrier (lua_State *, GCObject *, int back);
/* Shared list publication owns the active collector's queue or quiescence. */
LUAI_FUNC void luaC_linktrace (global_State *, GCObject *);
/* Private work-list inspection; caller owns quiescent collection. */
LUAI_FUNC GCObject *luaC_barriernext (GCObject *object);
/* A hybrid deletion/insertion barrier shades before mutation. No allocator,
** object ownership or collection request is permitted on these paths. */
#define luaC_tracing(L) luaC_tracingglobal(G(L))
#define luaC_tracevalue(L,v) \
  { const TValue *trace_v_ = (v); \
    if (luaC_tracing(L) && iscollectable(trace_v_)) \
      luaC_traceobject((L), gcvalue(trace_v_)); }
#define luaC_prewrite(L,old,newv) \
  { if (luaC_tracing(L)) { luaC_tracevalue((L),(old)); \
      luaC_tracevalue((L),(newv)); } }
#define luaC_tracepointer(L,p) \
  { if (luaC_tracing(L)) { GCObject *trace_p_ = cast(GCObject *, (p)); \
      if (trace_p_ != NULL) luaC_traceobject((L), trace_p_); } }
#define luaC_prewriteptr(L,old,newp) \
  { luaC_tracepointer((L),(old)); luaC_tracepointer((L),(newp)); }
LUAI_FUNC void luaC_traceobject (lua_State *L, GCObject *object);
LUAI_FUNC void luaC_snapshot (lua_State *L, lua_State *thread);
LUAI_FUNC int luaC_snapshotparked (lua_State *L, lua_State *thread);
LUAI_FUNC void luaC_snapshotproto (lua_State *L, Proto *proto);
LUAI_FUNC void luaC_traceview (lua_State *L, const Table *view);
LUAI_FUNC void luaC_checksnapshot (lua_State *L);
LUAI_FUNC void luaC_trimstack (lua_State *L);
LUAI_FUNC lu_mem luaC_tracebatch (lua_State *L, unsigned limit);
LUAI_FUNC void luaC_readytrace (global_State *g);
/* Zero rechecks all pending roots when the world reopens. */
LUAI_FUNC int luaC_readystack (global_State *g, uintptr_t stack);
LUAI_FUNC unsigned luaC_skiplivefinalizers (global_State *g, unsigned limit);
/* Strong tables use bounded, validated reads under the admission storage epoch.
** Return -1 for weak/borrowed ownership, 0 to defer, or 1 on completion. */
LUAI_FUNC int luaC_scanstrong (lua_State *, LuaTraceWork *, Table *);
LUAI_FUNC int luaC_tablesize (Table *, l_mem *);
LUAI_FUNC void luaC_traceedges (LuaTraceWork *, GCObject **, unsigned);
LUAI_FUNC void luaC_finishstrong (LuaTraceWork *, GCObject *, int old_edges);
LUAI_FUNC int luaC_scanvalues (lua_State *, LuaTraceWork *, GCObject *);
LUAI_FUNC unsigned luaC_objectchunk (GCObject *, unsigned, unsigned,
                                     GCObject **);
/* Claims/publication hold world.mutex; scanning owns only the local batch. */
LUAI_FUNC void luaC_claimtrace (lua_State *L, LuaTraceWork *work, unsigned limit);
LUAI_FUNC void luaC_scantrace (lua_State *L, LuaTraceWork *work, unsigned limit);
LUAI_FUNC void luaC_publishtrace (lua_State *L, LuaTraceWork *work);
LUAI_FUNC void luaC_advancetrace (lua_State *L);
/* Publish the mark plan and accounting after the barrier handshake.
** The caller owns world.mutex, or has stopped the world. */
LUAI_FUNC void luaC_publishtraceplan (global_State *g);
LUAI_FUNC void luaC_initialtrace (lua_State *L);
LUAI_FUNC void luaC_finishroots (lua_State *L);
LUAI_FUNC void luaC_finishtrace (lua_State *L);
LUAI_FUNC void luaC_assisttrace (lua_State *L);
LUAI_FUNC void luaC_polltrace (lua_State *L);
/* Caller owns the table. Resolve the current weak cutoff before publishing
** a result or deciding whether an entry exists. New stores shade both edges. */
LUAI_FUNC void luaC_prepareweak (lua_State *L, Table *table);
LUAI_FUNC void luaC_waittrace (lua_State *L);
/* Destroy privately retired garbage. Normal collection calls this after
** resuming mutators; exclusive VM teardown also uses the same path. */
LUAI_FUNC void luaC_reclaim (lua_State *L);
LUAI_FUNC void luaC_assistreclaim (lua_State *L);
LUAI_FUNC l_mem luaC_objsize (GCObject *object);
LUAI_FUNC void luaC_retireobject (lua_State *L, GCObject *object);
LUAI_FUNC void luaC_finishreclaim (lua_State *L);
/* Collector or exclusive incomplete-VM cleanup only. Publish complete local
** construction lists, exact allocation totals and finalizer registrations. */
LUAI_FUNC void luaC_publish (lua_State *L);
LUAI_FUNC void luaC_checkfinalizer (lua_State *L, GCObject *o, Table *mt);
LUAI_FUNC void luaC_changemode (lua_State *L, int newmode);


#endif
