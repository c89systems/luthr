/*
** $Id: lstate.h $
** Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include <stdint.h>
#include "lua.h"
#include "lgate.h"
#include "lworld.h"
#include "lexec.h"
#include "lallocator.h"
#include "lpacer.h"


/* Some header files included here need this definition */
typedef struct CallInfo CallInfo;


#include "lobject.h"
#include "ltm.h"
#include "lzio.h"


/*
** Garbage-collected objects are discoverable until retirement:
**
** 'allgc': published non-table objects, including finalizable objects;
** table allocation membership is recorded in allocator pages.
** Each coroutine retains unpublished objects and a private table nursery.
** 'sweep_plan': the detached cohort, survivors, and retries of an active sweep;
** claimed batches remain owned until publication, with no safepoint between.
**
** Independent indices use 'next' in CommonHeader:
** 'finobj': objects marked for finalization;
** 'tobefnz': objects ready to be finalized;
** 'fixedgc': all objects that are not to be collected (currently
** only small strings, such as reserved words).
**
** For the generational collector, some of these lists have marks for
** generations between sweep jobs. Each mark points to the first element for
** that particular generation; that generation goes until the next mark.
**
** 'allgc' -> 'survival': new objects;
** 'survival' -> 'old1': objects that survived one collection;
** 'old1' -> NULL: objects old enough to skip in a young sweep.
**
** 'finobj' -> 'finobjsur': new objects marked for finalization;
** 'finobjsur' -> 'finobjold1': survived   """";
** 'finobjold1' -> NULL: old objects, skipped by minor discovery.
**
** Registration can put older objects at the head of the finalizer index.
** Allocation membership remains stable. Newly promoted objects and older
** objects written by mutators use the same remembered-object queue.
*/

/*
** Moreover, there is another set of lists that control gray objects.
** These lists are linked by fields 'gclist'. (All objects that
** can become gray have such a field. The field is not the same
** in all objects, but it always has this name.)  Any gray object
** must belong to one of these lists, and all objects in these lists
** must be gray (with two exceptions explained below):
**
** 'gray': regular gray objects, still waiting to be visited.
** 'grayagain': objects that must be revisited at the atomic phase.
**   That includes
**   - black objects got in a write barrier;
**   - all kinds of weak tables during propagation phase;
**   - all threads.
** 'weak': tables with weak values to be cleared;
** 'ephemeron': ephemeron tables with white->white entries;
** 'allweak': tables with weak keys and/or weak values to be cleared.
**
** The exceptions to that "gray rule" are:
** - TOUCHED2 objects in generational mode stay in a gray list (because
** they must be visited again at the end of the cycle), but they are
** marked black because assignments to them must activate barriers (to
** move them back to TOUCHED1).
** - Open upvalues are kept gray to avoid barriers, but they stay out
** of gray lists. (They don't even have a 'gclist' field.)
*/



/*
** About 'nCcalls':  This count has two parts: the lower 16 bits counts
** the number of recursive invocations in the C stack; the higher
** 16 bits counts the number of non-yieldable calls in the stack.
** (They are together so that we can change and save both with one
** instruction.)
*/


/* true if this thread does not have non-yieldable calls in the stack */
#define yieldable(L)		(((L)->nCcalls & 0xffff0000) == 0)

/* real number of C calls */
#define getCcalls(L)	((L)->nCcalls & 0xffff)


/* Increment the number of non-yieldable calls */
#define incnny(L)	((L)->nCcalls += 0x10000)

/* Decrement the number of non-yieldable calls */
#define decnny(L)	((L)->nCcalls -= 0x10000)

/* Non-yieldable call increment */
#define nyci	(0x10000 | 1)




struct lua_longjmp;  /* defined in ldo.c */


/*
** Type for VM trap flags. Hook configuration uses explicit lock-free atomics;
** asynchronous setters no longer walk or modify active CallInfo records.
*/
#if !defined(l_signalT)
#include <signal.h>
#define l_signalT	sig_atomic_t
#endif


/*
** Extra stack space to handle TM calls and some other extras. This
** space is not included in 'stack_last'. It is used only to avoid stack
** checks, either because the element will be promptly popped or because
** there will be a stack check soon after the push. Function frames
** never use this extra space, so it does not need to be kept clean.
*/
#define EXTRA_STACK   5


/*
** Size of cache for strings in the API. 'N' is the number of
** sets (better be a prime) and "M" is the size of each set.
** (M == 1 makes a direct cache.)
*/
#if !defined(STRCACHE_N)
#define STRCACHE_N              53
#define STRCACHE_M              2
#endif


#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)

#define stacksize(th)	cast_int((th)->stack_last.p - (th)->stack.p)


/* kinds of Garbage Collection */
#define KGC_INC		0	/* incremental gc */
#define KGC_GENMINOR	1	/* generational gc in minor (regular) mode */
#define KGC_GENMAJOR	2	/* generational in major mode */


typedef struct stringtable {
  LuaObjectLock access;
  TString **hash;  /* array of buckets (linked lists of strings) */
  int nuse;  /* number of elements */
  int size;  /* number of buckets */
} stringtable;

#define LUA_STRING_SHARDS 32
#define LUA_STRING_INITIAL_BUCKETS 4


/*
** Information about a call.
** About union 'u':
** - field 'l' is used only for Lua functions;
** - field 'c' is used only for C functions.
** About union 'u2':
** - field 'funcidx' is used only by C functions while doing a
** protected call;
** - field 'nyield' is used only while a function is "doing" an
** yield (from the yield until the next resume);
** - field 'nres' is used only while closing tbc variables when
** returning from a function;
*/
struct CallInfo {
  StkIdRel func;  /* function index in the stack */
  StkIdRel top;  /* top for this function */
  struct CallInfo *previous, *next;  /* dynamic call link */
  union {
    struct {  /* only for Lua functions */
      const Instruction *savedpc;
      volatile l_signalT trap;  /* function is tracing lines/counts */
      int nextraargs;  /* # of extra arguments in vararg functions */
      uint64_t captures;  /* shared bindings, conservatively indexed modulo 64 */
      const Instruction *rootpc;  /* operation whose shadows may be in use */
    } l;
    struct {  /* only for C functions */
      lua_KFunction k;  /* continuation in case of yields */
      ptrdiff_t old_errfunc;
      lua_KContext ctx;  /* context info. in case of yields */
    } c;
  } u;
  union {
    int funcidx;  /* called-function index */
    int nyield;  /* number of values yielded */
    int nres;  /* number of values returned */
  } u2;
  l_uint32 callstatus;
};


/*
** Maximum expected number of results from a function
** (must fit in CIST_NRESULTS).
*/
#define MAXRESULTS	250


/*
** Bits in CallInfo status
*/
/* bits 0-7 are the expected number of results from this function + 1 */
#define CIST_NRESULTS	0xffu

/* bits 8-11 count call metamethods (and their extra arguments) */
#define CIST_CCMT	8  /* the offset, not the mask */
#define MAX_CCMT	(0xfu << CIST_CCMT)

/* Bits 12-14 are used for CIST_RECST (see below) */
#define CIST_RECST	12  /* the offset, not the mask */

/* call is running a C function (still in first 16 bits) */
#define CIST_C		(1u << (CIST_RECST + 3))
/* call is on a fresh "luaV_execute" frame */
#define CIST_FRESH	(cast(l_uint32, CIST_C) << 1)
/* function is closing tbc variables */
#define CIST_CLSRET	(CIST_FRESH << 1)
/* function has tbc variables to close */
#define CIST_TBC	(CIST_CLSRET << 1)
/* original value of 'allowhook' */
#define CIST_OAH	(CIST_TBC << 1)
/* call is running a debug hook */
#define CIST_HOOKED	(CIST_OAH << 1)
/* doing a yieldable protected call */
#define CIST_YPCALL	(CIST_HOOKED << 1)
/* call was tail called */
#define CIST_TAIL	(CIST_YPCALL << 1)
/* last hook called yielded */
#define CIST_HOOKYIELD	(CIST_TAIL << 1)
/* function "called" a finalizer */
#define CIST_FIN	(CIST_HOOKYIELD << 1)


#define get_nresults(cs)  (cast_int((cs) & CIST_NRESULTS) - 1)

/*
** Field CIST_RECST stores the "recover status", used to keep the error
** status while closing to-be-closed variables in coroutines, so that
** Lua can correctly resume after an yield from a __close method called
** because of an error.  (Three bits are enough for error status.)
*/
#define getcistrecst(ci)     (((ci)->callstatus >> CIST_RECST) & 7)
#define setcistrecst(ci,st)  \
  check_exp(((st) & 7) == (st),   /* status must fit in three bits */  \
            ((ci)->callstatus = ((ci)->callstatus & ~(7u << CIST_RECST))  \
                                | (cast(l_uint32, st) << CIST_RECST)))


/* active function is a Lua function */
#define isLua(ci)	(!((ci)->callstatus & CIST_C))

/* call is running Lua code (not a hook) */
#define isLuacode(ci)	(!((ci)->callstatus & (CIST_C | CIST_HOOKED)))


#define setoah(ci,v)  \
  ((ci)->callstatus = ((v) ? (ci)->callstatus | CIST_OAH  \
                           : (ci)->callstatus & ~CIST_OAH))
#define getoah(ci)  (((ci)->callstatus & CIST_OAH) ? 1 : 0)


/* Error-unwind headroom belongs to the physical OS thread stack. */
typedef struct NativeStackContext {
  uintptr_t limit;
  lu_byte overflow;
  LuaExecution *execution;
  struct LuaCoroutineScope *coroutine_scopes;
  LuaApiRegion *api_region;
} NativeStackContext;

/* Carrier-local defaults and ambient host scopes share one TLS record. Managed
** fibers supply their own stack/execution context and never retain this record
** across a carrier handoff. */
typedef struct LuaThreadContext {
  NativeStackContext native;
  NativeStackContext *override;
  LuaPolicy *policy;
  struct LuaGCFrame *gc_frame;
} LuaThreadContext;

/* Temporary values owned by a native scope, independently of Lua stack slots.
** Nodes do not move and must be removed before their native storage expires. */
typedef struct LuaNativeRoot {
  struct LuaNativeRoot *previous;
  TValue value;
  const Table *table_view;  /* borrowed resize image, strongly traced until pop */
} LuaNativeRoot;

typedef struct LuaSharingRoots {
  struct LuaSharingRoots *previous;
  GCObject *pending;
  GCObject *current;
} LuaSharingRoots;


/*
** 'per thread' state
*/
struct lua_State {
  CommonHeader;
  lu_byte allowhook;
  TStatus status;
  StkIdRel top;  /* first free slot in the stack */
  struct global_State *l_G;
  CallInfo *ci;  /* call info for current function */
  StkIdRel stack_last;  /* end of stack (last element + 1) */
  StkIdRel stack;  /* stack base */
  UpVal *openupval;  /* list of open upvalues in this stack */
  StkIdRel tbclist;  /* list of to-be-closed variables */
  GCObject *gclist;
  struct lua_longjmp *errorJmp;  /* current error recover point */
  CallInfo base_ci;  /* CallInfo for first level (C host) */
  lua_Hook hook;
  ptrdiff_t errfunc;  /* current error handling function (stack index) */
  l_uint32 nCcalls;  /* number of nested non-yieldable or C calls */
  int oldpc;  /* last pc traced */
  int nci;  /* number of items in 'ci' list */
  int basehookcount;
  unsigned hookcount;
  l_signalT hookmask;
  struct {  /* info about transferred values (for call/return hooks) */
    int ftransfer;  /* offset of first value transferred */
    int ntransfer;  /* number of values transferred */
  } transferinfo;
  uintptr_t luthr_stack_limit;
  NativeStackContext *luthr_stack_context;
  LuaNativeRoot *native_roots;
  LuaSharingRoots *sharing_roots;  /* promotion can outlive a marking epoch */
  struct LuaCallback *callback_owner;
  unsigned callback_active;
  struct LuaCoroutineScope *operation;  /* world mutex protects operation metadata */
  struct LuaTaskRoot *task_owner;  /* persistent scheduled-task reservation */
  unsigned owned_operations;
  unsigned resume_active;
  unsigned close_active;  /* nesting depth, including recursive self-close */
  LuaStackGate stack_gate;
  LuaParticipation participation;
  LuaParticipation *admission;  /* shared by a paired stack transaction */
  struct lua_State *stack_peer;  /* other gate in a paired transaction */
  LuaPolicy *policy;
  void (*state_poll)(lua_State *L, void *context);
  void *state_poll_context;
  unsigned poll_remaining;
  unsigned native_depth;
  unsigned world_depth;
  unsigned heap_waiting;
  unsigned object_waiting;
  unsigned root_waiting;  /* atomic hint; borrowing requires world.mutex */
  struct LuaObjectScope *object_scopes;
  uint64_t trace_snapshot;  /* release-published root snapshot epoch */
  uint64_t trace_trimmed;
  lu_mem trace_bytes;
  struct Proto *building_protos;  /* mutable compiler/undumper graphs */
  GCObject *retired;  /* atomic work hint; world mutex protects shared claims */
  GCObject *retired_tail;
  unsigned reclaiming;
  unsigned reclaim_pages;
  struct LuaTableRetirement *storage_retired, *storage_tail;
  size_t storage_bytes;  /* owner-local unpublished retirement batch */
  struct lua_State *reclaim_next;
  unsigned reclaim_workers;  /* world mutex protects work ownership */
  unsigned reclaim_registered;  /* world mutex; helpers may finish the queue */
  lu_mem assist_debt;
  lu_mem trace_debt;  /* unpaid marking work, independent of physical returns */
  uint64_t assist_epoch;
  GCObject *new_objects;
  GCObject *new_objects_tail;
  GCObject *new_tables;  /* scratch links in table allocation prefixes */
  uintptr_t nursery_epoch;
  size_t nursery_tables;
  l_mem allocation_debt;
  l_mem reclaim_credit;  /* freed bytes do not replenish allocation pacing */
  struct lua_State *publish_next;
  struct lua_State **publish_previous;  /* atomic membership, shard owns links */
  TString *strcache[STRCACHE_N][STRCACHE_M];
  LuaAllocationCache allocation;
  uint64_t strcache_epoch;  /* zero until an owner initializes the cache */
};


/*
** thread state + extra space
*/
typedef struct LX {
  lu_byte extra_[LUA_EXTRASPACE];
  lua_State l;
} LX;


/*
** 'global state', shared by all threads of this state
*/
typedef struct LuaTraceList {
  GCObject *head;
  GCObject **tail;
} LuaTraceList;

/* Collection cursors belong to the VM, never to a driver's suspended C stack.
** A helper can therefore finish a phase while its original caller is parked. */
typedef struct LuaTracePlan {
  GCObject *again;
  GCObject *next;
  GCObject *deferred;
  GCObject *tail;
  GCObject *pending;
  GCObject **finalizers;
  GCObject *finalizer_gray;
  GCObject **finalizer_tail;
  GCObject *finalizer_new;
  GCObject **finalizer_newtail;
  uint64_t round_changes;
  uint64_t clear_changes;
  l_mem base_marked;
  unsigned phase;
  unsigned after;
  unsigned changed;
  unsigned direction;
  unsigned weak_list;
  unsigned finish;
  int roots_pending;  /* native roots captured at the first closed boundary */
} LuaTracePlan;

/* A sweep owns its old allocation cohort until every claimed batch returns.
** Newly published allocations stay in allgc, before the append anchor. */
typedef struct LuaSweepPlan {
  GCObject *next, *middle, *limit;
  struct LuaAllocPage *pages[LUA_ALLOC_BINS];
  unsigned page_pool, page_limit;
  GCObject *live[2], **tail[2];
  GCObject *retry[2];
  GCObject *table_retry;
  GCObject *threads;
  GCObject *gray[4];
  LuaTraceList watch;  /* remembered work for the next trace */
  GCObject *finhead, *finsurvival;
  GCObject **append;
  l_mem addedold, base_marked;
  uint64_t epoch;
  unsigned active, busy, kind, region, phase, gray_index, finishing;
} LuaSweepPlan;

#define LUA_BARRIER_SHARDS 16
typedef struct LuaBarrierQueue {
  GCObject *forward, *back;  /* atomic MPSC heads; claims own intrusive links */
  char padding[64 - 2 * sizeof(GCObject *)];
} LuaBarrierQueue;

#define LUA_TABLE_WRITERS 512
typedef struct LuaTableWriter {
  Table *owner;
  uint64_t sequence;
  uintptr_t slot;  /* representation only; never followed as a GC root */
  unsigned waiting;
} LuaTableWriter;

#define LUA_PUBLISH_SHARDS 8
typedef struct LuaPublishShard {
  pthread_mutex_t mutex;
  lua_State *head;
} LuaPublishShard;

typedef struct global_State {
  LuaWorld world;
  LuaTableWriter table_writers[LUA_TABLE_WRITERS];
  struct LuaScheduler *scheduler;
  struct LuaAllocPage *allocation_pages[LUA_ALLOC_POOLS];  /* pool mutex */
  struct LuaAllocPage *allocation_dirty;  /* atomic changed-page queue */
  struct LuaAllocPage *allocation_pending;  /* mutex-claimed finite page job */
  struct LuaAllocPage *allocation_retired;  /* atomic privately empty pages */
  uint64_t allocation_epoch;  /* a finite queue snapshot; helpers may finish it */
  unsigned allocation_busy;
  struct LuaAllocPage *allocation_returning;  /* world mutex, finite return job */
  uint64_t allocation_return_epoch;
  pthread_cond_t allocation_changed;  /* slow cache leases; pool zero mutex */
  unsigned allocation_waiters;
  pthread_mutex_t allocation_mutex[LUA_ALLOC_POOLS];
  struct LuaAllocPage *allocation_available[LUA_ALLOC_POOLS];
  LuaAllocationCache *slot_caches;
  LuaAllocator allocator;
  lua_Hook managed_hook;  /* runtime-owned accounting dispatcher, if any */
  struct LuaCallback *callbacks;  /* registered native roots, metadata-owned */
  struct Table *pipe_meta;  /* immutable shared chunk-pipe metatable */
  LuaObjectLock metatable_access[LUA_NUMTYPES];
  LuaObjectLock registry_access;
  LuaObjectLock warning_access;
  LuaObjectLock panic_access;
  LuaObjectLock callbacks_access;
  struct LuaCoroutineScope *coroutine_scopes;  /* temporary operation roots */
  struct LuaTaskRoot *tasks;  /* persistent scheduled-task roots */
  int task_admission_closed;  /* sticky, world-mutex-owned shutdown gate */
  TString *operation_error;
  l_mem GCbytes;  /* atomic charged storage, including pending destruction */
  l_mem GCused;  /* atomic logical storage, excluding retired objects */
  l_mem GCstep;  /* atomic logical byte threshold for a collector step */
  l_mem GCretired;  /* atomic logically dead storage awaiting physical credits */
  l_mem GCmajorlive;  /* logical live storage at the last major collection */
  LuaGCPacer pacer;
  l_mem GCcyclelive;  /* initial tracing cohort, less its logical garbage */
  unsigned gcstep_pending;  /* atomic; one automatic request seeks metadata */
  unsigned mark_state;  /* atomic stamps, allocation age and trace flags */
  unsigned trace_weakcutoff;  /* atomic: values, then keys, become collectible */
  uint64_t trace_interrupt;  /* emergency takeover invalidates older requests */
  uint64_t trace_epoch;
  uint64_t major_completed;  /* atomic count at the end of major collection */
  uint64_t weak_stamp;  /* high bits of the tables' per-trace weak state */
  unsigned trace_busy;  /* atomic number of unpublished scanning batches */
  unsigned trace_ready;  /* atomic queued-work hint, independent of busy claims */
  unsigned barrier_ready;  /* atomic directory of nonempty barrier shards */
  LuaBarrierQueue barriers[LUA_BARRIER_SHARDS];
  unsigned trace_workers;  /* atomic: payloads excluding failed reservations */
  unsigned sweep_workers;  /* atomic: ordinary ranges in an admitted sweep */
  uint64_t trace_changes;  /* atomic monotonic shading progress */
  GCObject *trace_deferred;
  GCObject *trace_stacks;  /* blocked stack roots, owned by trace_mutex */
  LuaTracePlan trace_plan;
  LuaSweepPlan sweep_plan;
  l_mem GCmarked;  /* bytes marked in a GC cycle */
  l_mem GCmajorminor;  /* auxiliary counter to control major-minor shifts */
  stringtable strt[LUA_STRING_SHARDS];  /* independently owned intern partitions */
  unsigned shrink_strings;  /* atomic request from a retiring intern shard */
  TValue l_registry;
  TValue nilvalue;  /* a nil value */
  unsigned int seed;  /* randomized seed for hashes */
  lu_byte gcparams[LUA_GCPN];
  lu_byte gcstate;  /* state of garbage collector */
  lu_byte gckind;  /* kind of GC running */
  lu_byte gcstopem;  /* stops emergency collections */
  lu_byte gcstp;  /* control whether GC is running */
  lu_byte gcemergency;  /* true if this is an emergency collection */
  GCObject *allgc;  /* shared list of non-table allocations */
  GCObject *finobj;  /* list of collectable objects with finalizers */
  GCObject *gray;  /* list of gray objects */
  GCObject *grayagain;  /* list of objects to be traversed atomically */
  GCObject *weak;  /* list of tables with weak values */
  GCObject *ephemeron;  /* list of ephemeron tables (weak keys) */
  GCObject *allweak;  /* list of all-weak tables */
  GCObject *tobefnz;  /* list of userdata to be GC */
  GCObject **tobefnz_tail;  /* append without walking queued finalizers */
  GCObject *fixedgc;  /* list of objects not to be collected */
  /* fields for generational collector */
  GCObject *survival;  /* start of objects that survived one GC cycle */
  GCObject *old1;  /* start of the old allocation band */
  GCObject *finobjsur;  /* list of survival objects with finalizers */
  GCObject *finobjold1;  /* list of old1 objects with finalizers */
  lua_CFunction panic;  /* to be called in unprotected errors */
  TString *memerrmsg;  /* message for memory-allocation errors */
  TString *stoperrmsg;  /* fixed task stop delivery; never allocates */
  TString *limiterrmsg;
  LuaPolicy *(*policy_factory)(lua_State *, const void *, void *);
  void *policy_context;
  TString *tmname[TM_N];  /* array with tag-method names */
  struct Table *mt[LUA_NUMTYPES];  /* metatables for basic types */
  struct Table *string_index;  /* atomic projection of the installed __index */
  lua_WarnFunction warnf;  /* warning function */
  void *ud_warn;         /* auxiliary data to 'warnf' */
  LX mainth;  /* main thread of this state */
  LuaPublishShard publishers[LUA_PUBLISH_SHARDS];
  GCObject *new_finalizers, *new_finalizers_tail;
  uint64_t strcache_epoch;
  uint64_t metatable_sequence[LUA_NUMTYPES];  /* primitive metatable publication */
  TString empty_string;
  TString byte_strings[UCHAR_MAX + 1];  /* immutable storage owned by the VM */
  pthread_cond_t phase_changed;
  LuaWaiter *phase_waiters;  /* world.mutex, sweep readiness */
  uint64_t minor_request;  /* world.mutex; odd while an explicit driver owns it */
  uint64_t minor_completed;  /* world.mutex; logical minor completions */
  LuaWaiter *minor_waiters;  /* world.mutex, explicit minor requests */
  pthread_mutex_t trace_mutex;  /* trace batch claims and publication */
  pthread_cond_t trace_changed;
  LuaWaiter *trace_waiters;  /* trace_mutex */
} global_State;


#define G(L)	(L->l_G)
#define mainthread(G)	(&(G)->mainth.l)

/*
** 'g->nilvalue' being a nil value flags that the state was completely
** build.
*/
#define completestate(g)	ttisnil(&g->nilvalue)


/*
** Union of all collectable objects
** ISO C99, 6.5.2.3 p.5:
** "if a union contains several structures that share a common initial
** sequence [...], and if the union object currently contains one
** of these structures, it is permitted to inspect the common initial
** part of any of them anywhere that a declaration of the complete type
** of the union is visible."
*/
union GCUnion {
  GCObject gc;  /* common header */
  struct TString ts;
  struct Udata u;
  union Closure cl;
  struct Table h;
  struct Proto p;
  struct lua_State th;  /* thread */
  struct UpVal upv;
};


/* macros to convert a GCObject into a specific value
** ISO C99, 6.3.2.2 p.7:
** "A pointer to an object or incomplete type may be converted to a
** pointer to a different object or incomplete type. If the resulting
** pointer is not correctly aligned for the pointed-to type, the
** behavior is undefined. Otherwise, when converted back again, the
** result shall compare equal to the original pointer."
*/
#define gco2(v,T,o)	check_exp((o)->tt == v, cast(T*, o))
#define gco2nv(t,T,o)	check_exp(novariant((o)->tt) == t, cast(T*, o))
#define gco2ts(o)	gco2nv(LUA_TSTRING, TString, o)
#define gco2u(o)	gco2(LUA_VUSERDATA, Udata, o)
#define gco2lcl(o)	(&gco2(LUA_VLCL, Closure, o)->l)
#define gco2ccl(o)	(&gco2(LUA_VCCL, Closure, o)->c)
#define gco2cl(o)	gco2nv(LUA_TFUNCTION, Closure, o)
#define gco2t(o)	gco2(LUA_VTABLE, Table, o)
#define gco2p(o)	gco2(LUA_VPROTO, Proto, o)
#define gco2th(o)	gco2(LUA_VTHREAD, lua_State, o)
#define gco2upv(o)	gco2(LUA_VUPVAL, UpVal, o)


/*
** macro to convert a Lua object into a GCObject
*/
#define obj2gco(v)  \
	check_exp(novariant((v)->tt) >= LUA_TSTRING, cast(GCObject*, v))


/* actual number of total memory allocated */
#define gettotalbytes(g) __atomic_load_n(&(g)->GCbytes, __ATOMIC_RELAXED)
#define getgcbytes(g) __atomic_load_n(&(g)->GCused, __ATOMIC_RELAXED)

l_sinline l_mem luaE_getdebt (const global_State *g) {
  l_mem step = __atomic_load_n(&g->GCstep, __ATOMIC_RELAXED);
  l_mem used = getgcbytes(g);
  return step < used - MAX_LMEM ? -MAX_LMEM : step - used;
}

l_sinline l_mem luaP_debt (const global_State *g) {
  l_mem limit = __atomic_load_n(&g->pacer.hard, __ATOMIC_RELAXED);
  return limit - getgcbytes(g);
}


LUAI_FUNC void luaE_setdebt (global_State *g, l_mem debt);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);
LUAI_FUNC void luaE_retirethread (lua_State *L, lua_State *thread);
LUAI_FUNC lu_mem luaE_threadsize (lua_State *L);
LUAI_FUNC CallInfo *luaE_extendCI (lua_State *L, int err);
LUAI_FUNC void luaE_shrinkCI (lua_State *L);
/* Two spare frames allow a finalizer and its nested call. */
l_sinline CallInfo *luaE_cireserve (CallInfo *active) {
  CallInfo *spare = active->next;
  return spare != NULL ? spare->next : NULL;
}
LUAI_FUNC void luaE_checkcstack (lua_State *L);
LUAI_DDEC(__thread LuaThreadContext luaE_thread_context;)
LUAI_FUNC uintptr_t luaE_initnativestack (void);
LUAI_FUNC NativeStackContext *luaE_getnativestackcontext (void);
LUAI_FUNC NativeStackContext *luaE_setnativestackcontext (NativeStackContext *context);

/* Only an owner (including a collector reservation) may use bound context.
** Bootstrap construction has no reservation yet and resolves its ambient
** context. A binding ends before its gate or final participation is released. */
l_sinline NativeStackContext *luaX_context (lua_State *L) {
  return L->stack_gate.context != NULL ? L->stack_gate.context
                                      : luaE_getnativestackcontext();
}

l_sinline LuaExecution *luaX_execution (lua_State *L) {
  return luaX_context(L)->execution;
}

l_sinline LuaThreadContext *luaX_hostcontext (lua_State *L) {
  NativeStackContext *context = L->stack_gate.context;
  if (context != NULL && context->execution == NULL) {
    /* An unmanaged owner token names the host's first member, even when
    ** an override supplies the logical stack context. */
    uintptr_t word = __atomic_load_n(&L->stack_gate.word, __ATOMIC_RELAXED);
    return cast(LuaThreadContext *, luaX_gateowner(word));
  }
  return &luaE_thread_context;
}

l_sinline void *luaX_token (lua_State *L) {
  uintptr_t word = __atomic_load_n(&L->stack_gate.word, __ATOMIC_RELAXED);
  void *owner = luaX_gateowner(word);
  return owner != NULL ? owner : luaQ_ownertoken();
}

l_sinline int luaX_isowner (lua_State *L, const LuaOwner *owner) {
  return __atomic_load_n(&owner->token, __ATOMIC_ACQUIRE) == luaX_token(L);
}

l_sinline int luaW_ownsheap (lua_State *L) {
  void *owner = __atomic_load_n(&G(L)->world.heap_owner.token, __ATOMIC_ACQUIRE);
  return owner != NULL && owner == luaX_token(L);
}

LUAI_FUNC void luaE_nativestackerror (lua_State *L, uintptr_t position);
LUAI_FUNC void luaE_incCstack (lua_State *L);
LUAI_FUNC void luaE_warning (lua_State *L, const char *msg, int tocont);
LUAI_FUNC void luaE_warnerror (lua_State *L, const char *where);
LUAI_FUNC TStatus luaE_resetthread (lua_State *L, TStatus status);
/* State ownership and a protected allocation boundary are required. */
LUAI_FUNC lua_State *luaE_newthread (lua_State *L, int collector);


#endif

