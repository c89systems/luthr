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

typedef struct LuaGCFrame {
  struct LuaGCFrame *previous;
  global_State *global;
} LuaGCFrame;

enum {
  TRACE_INIT, TRACE_MARK, TRACE_EPHEMERONS, TRACE_VALUES, TRACE_FINALIZERS,
  TRACE_CLEAR, TRACE_VERIFY
};
enum { TRACE_MAJOR = 1, TRACE_YOUNG, TRACE_GENERATIONAL };

/* A batch owns its payloads until it publishes every list and counter. It
** must not yield or park while these private references are outstanding. */
#define LUA_TRACE_BATCH 64
typedef struct LuaTraceList {
  GCObject *head;
  GCObject **tail;
} LuaTraceList;

typedef struct LuaTraceWork {
  global_State *global;
  LuaTraceList gray, grayagain, weak, allweak, ephemeron, deferred;
  GCObject *retry, *retry_tail;
  GCObject *objects[LUA_TRACE_BATCH];
  uint64_t claims;
  l_mem marked;
  lu_mem scanned;
  unsigned count, phase, direction, changed;
} LuaTraceWork;

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
#define GCSswpfinobj	4
#define GCSswptobefnz	5
#define GCSswpend	6
#define GCScallfin	7
#define GCSpause	8


#define issweepphase(g)  \
	(GCSswpallgc <= (g)->gcstate && (g)->gcstate <= GCSswpend)


/*
** macro to tell when main invariant (white objects cannot point to black
** ones) must be kept. During a collection, the sweep phase may break
** the invariant, as objects turned white may point to still-black
** objects. The invariant is restored when sweep ends and all objects
** are white again.
*/

#define keepinvariant(g)	((g)->gcstate <= GCSatomic)


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
** Layout for bit use in 'marked' field. First three bits are
** used for object "age" in generational mode. Last bit is used
** by tests.
*/
#define WHITE0BIT	3  /* object is white (type 0) */
#define WHITE1BIT	4  /* object is white (type 1) */
#define BLACKBIT	5  /* object is black */
#define FINALIZEDBIT	6  /* object has been marked for finalization */

#define TESTBIT		7



#define WHITEBITS	bit2mask(WHITE0BIT, WHITE1BIT)


#define getmarked(x) __atomic_load_n(&(x)->marked, __ATOMIC_RELAXED)
/* Diagnostic reads can run outside metadata ownership. An exchange also
** makes these atomic writes visible to Helgrind's instruction-level model. */
#define setmarked(x,m) \
  ((void)__atomic_exchange_n(&(x)->marked, (m), __ATOMIC_RELAXED))
#define iswhite(x)      testbits(getmarked(x), WHITEBITS)
#define isblack(x)      testbit(getmarked(x), BLACKBIT)
#define isgray(x)  /* neither white nor black */  \
	(!testbits(getmarked(x), WHITEBITS | bitmask(BLACKBIT)))

#define tofinalize(x)	testbit(getmarked(x), FINALIZEDBIT)

#define otherwhite(g)	((g)->currentwhite ^ WHITEBITS)
#define isdeadm(ow,m)	((m) & (ow))
#define isdead(g,v)	isdeadm(otherwhite(g), getmarked(v))

#define changewhite(x) __atomic_fetch_xor(&(x)->marked, WHITEBITS, __ATOMIC_RELAXED)
#define nw2black(x)  \
	check_exp(!iswhite(x), l_setbit((x)->marked, BLACKBIT))

#define luaC_white(g)	cast_byte((g)->currentwhite & WHITEBITS)


/* object age in generational mode */
#define G_NEW		0	/* created in current cycle */
#define G_SURVIVAL	1	/* created in previous cycle */
#define G_OLD0		2	/* marked old by frw. barrier in this cycle */
#define G_OLD1		3	/* first full cycle as old */
#define G_OLD		4	/* really old object (not to be visited) */
#define G_TOUCHED1	5	/* old object touched this cycle */
#define G_TOUCHED2	6	/* old object touched in previous cycle */

#define AGEBITS		7  /* all age bits (111) */

#define getage(o)	(getmarked(o) & AGEBITS)
#define setage(o,a) setmarked(o, cast_byte((getmarked(o) & (~AGEBITS)) | a))
#define isold(o)	(getage(o) > G_SURVIVAL)


/*
** In generational mode, objects are created 'new'. After surviving one
** cycle, they become 'survival'. Both 'new' and 'survival' can point
** to any other object, as they are traversed at the end of the cycle.
** We call them both 'young' objects.
** If a survival object survives another cycle, it becomes 'old1'.
** 'old1' objects can still point to survival objects (but not to
** new objects), so they still must be traversed. After another cycle
** (that, being old, 'old1' objects will "survive" no matter what)
** finally the 'old1' object becomes really 'old', and then they
** are no more traversed.
**
** To keep its invariants, the generational mode uses the same barriers
** also used by the incremental mode. If a young object is caught in a
** forward barrier, it cannot become old immediately, because it can
** still point to other young objects. Instead, it becomes 'old0',
** which in the next cycle becomes 'old1'. So, 'old0' objects is
** old but can point to new and survival objects; 'old1' is old
** but cannot point to new objects; and 'old' cannot point to any
** young object.
**
** If any old object ('old0', 'old1', 'old') is caught in a back
** barrier, it becomes 'touched1' and goes into a gray list, to be
** visited at the end of the cycle.  There it evolves to 'touched2',
** which can point to survivals but not to new objects. In yet another
** cycle then it becomes 'old' again.
**
** The generational mode must also control the colors of objects,
** because of the barriers.  While the mutator is running, young objects
** are kept white. 'old', 'old1', and 'touched2' objects are kept black,
** as they cannot point to new objects; exceptions are threads and open
** upvalues, which age to 'old1' and 'old' but are kept gray. 'old0'
** objects may be gray or black, as in the incremental mode. 'touched1'
** objects are kept gray, as they must be visited again at the end of
** the cycle.
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
    (__atomic_load_n(&G(L)->GCdebt, __ATOMIC_RELAXED) <= -(L)->allocation_debt)

#define LUA_GC_ASSIST_BYTES 65536
#define luaC_retired(L) __atomic_load_n(&(L)->retired, __ATOMIC_ACQUIRE)
#define luaC_hasreclaimers(L) \
    (__atomic_load_n(&G(L)->world.reclaimers, __ATOMIC_ACQUIRE) != NULL)
#define luaC_needassist(L) \
    ((L)->assist_debt >= LUA_GC_ASSIST_BYTES && luaC_hasreclaimers(L))
#define luaC_markpressure(L) \
    (__atomic_load_n(&G(L)->GCmarkdebt, __ATOMIC_RELAXED) <= \
     -(L)->allocation_debt)
#define luaC_needtraceassist(L) \
    (luaC_tracing(L) && gcrunning(G(L)) && \
     !(L)->reclaiming && \
     ((L)->trace_debt >= LUA_GC_ASSIST_BYTES || luaC_markpressure(L)))

#define luaC_condGC(L,pre,pos) \
    { if (!luaC_ingc(L)) { \
      if (luaC_needtraceassist(L)) { pre; luaC_assisttrace(L); pos; } \
      else { \
        if (gcrunning(G(L)) && luaC_needassist(L)) \
          { pre; luaC_assistreclaim(L); pos; } \
        if (!luaC_tracing(L) && gcrunning(G(L)) && luaC_needstep(L) && \
          ((L)->world_depth != 0 || \
           !__atomic_load_n(&G(L)->gcstep_pending, __ATOMIC_RELAXED))) \
          { pre; luaC_step(L); pos; } } \
	  condchangemem(L,pre,pos,0); } }

/* more often than not, 'pre'/'pos' are empty */
#define luaC_checkGC(L)		luaC_condGC(L,(void)0,(void)0)


#define luaC_objbarrier(L,p,o) \
        ((luaC_tracing(L) || (isblack(p) && iswhite(o))) ? \
         luaC_syncbarrier(L, obj2gco(p), obj2gco(o), 0) : cast_void(0))

#define luaC_barrier(L,p,v) (  \
	iscollectable(v) ? luaC_objbarrier(L,p,gcvalue(v)) : cast_void(0))

#define luaC_objbarrierback(L,p,o) \
        ((luaC_tracing(L) || (isblack(p) && iswhite(o))) ? \
         luaC_syncbarrier(L, obj2gco(p), obj2gco(o), 1) : cast_void(0))

#define luaC_barrierback(L,p,v) (  \
	iscollectable(v) ? luaC_objbarrierback(L, p, gcvalue(v)) : cast_void(0))

LUAI_FUNC void luaC_fix (lua_State *L, GCObject *o);
LUAI_FUNC void luaC_freeallobjects (lua_State *L);
LUAI_FUNC void luaC_step (lua_State *L);
LUAI_FUNC void luaC_runtilstate (lua_State *L, int state, int fast);
LUAI_FUNC void luaC_fullgc (lua_State *L, int isemergency);
LUAI_FUNC GCObject *luaC_newobj (lua_State *L, lu_byte tt, size_t sz);
LUAI_FUNC GCObject *luaC_newobjdt (lua_State *L, lu_byte tt, size_t sz,
                                                 size_t offset);
LUAI_FUNC void luaC_barrier_ (lua_State *L, GCObject *o, GCObject *v);
LUAI_FUNC void luaC_barrierback_ (lua_State *L, GCObject *o);
LUAI_FUNC void luaC_syncbarrier (lua_State *L, GCObject *o, GCObject *v, int back);
/* Collector-only: participants have finished all claimed publications. */
LUAI_FUNC void luaC_drainbarriers (lua_State *L);
/* Private work-list inspection; caller owns quiescent collection. */
LUAI_FUNC GCObject *luaC_barriernext (GCObject *object);
/* A hybrid deletion/insertion barrier shades before mutation. No allocator,
** object ownership or collection request is permitted on these paths. */
#define luaC_tracing(L) \
  (__atomic_load_n(&G(L)->trace_active, __ATOMIC_ACQUIRE) != 0)
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
/* Claims/publication hold world.mutex; scanning owns only the local batch. */
LUAI_FUNC void luaC_claimtrace (lua_State *L, LuaTraceWork *work, unsigned limit);
LUAI_FUNC void luaC_scantrace (lua_State *L, LuaTraceWork *work, unsigned limit);
LUAI_FUNC void luaC_publishtrace (lua_State *L, LuaTraceWork *work);
LUAI_FUNC void luaC_advancetrace (lua_State *L);
LUAI_FUNC void luaC_initialtrace (lua_State *L);
LUAI_FUNC void luaC_finalizetrace (lua_State *L);
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
