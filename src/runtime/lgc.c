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
#include "lcoroscope.h"
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
#include "luthr_hook_policy.h"
#include "lstring.h"
#include "ltable.h"
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


/* mask with all color bits */
#define maskcolors	(bitmask(BLACKBIT) | WHITEBITS)

/* mask with all GC bits */
#define maskgcbits      (maskcolors | AGEBITS)


/* macro to erase all color bits then set only the current white bit */
#define makewhite(g,x)	\
  setmarked(x, cast_byte((getmarked(x) & ~maskcolors) | luaC_white(g)))

/* make an object gray (neither white nor black) */
#define set2gray(x)	resetbits(x->marked, maskcolors)


#define valiswhite(x)   (iscollectable(x) && iswhite(gcvalue(x)))

#define keyiswhite(n)   (keyiscollectable(n) && iswhite(gckey(n)))


/*
** Protected access to objects in values
*/
#define gcvalueN(o)     (iscollectable(o) ? gcvalue(o) : NULL)


/*
** Access to collectable objects in array part of tables
*/
#define gcvalarr(t,i)  \
	((*getArrTag(t,i) & BIT_ISCOLLECTABLE) ? getArrVal(t,i)->gc : NULL)


#define markvalue(g,o) { checkliveness(mainthread(g),o); \
  if (valiswhite(o)) reallymarkobject(g,gcvalue(o)); }

#define markobject(g,t)	{ if (iswhite(t)) reallymarkobject(g, obj2gco(t)); }

/*
** mark an object that can be NULL (either because it is really optional,
** or it was stripped as debug info, or inside an uncompleted structure)
*/
#define markobjectN(g,t)	{ if (t) markobject(g,t); }


static void reallymarkobject (global_State *g, GCObject *o);
static int atomic (lua_State *L);
static lu_mem tracework (lua_State *L, lu_mem budget, int complete);
static void entersweep (lua_State *L);
static void setidledebt (global_State *g);
static l_mem livebytes (global_State *g);
static void linktrace (global_State *g, GCObject *object);
static int scantrace (lua_State *L, LuaTraceWork *work, GCObject *object);
static void starttrace (lua_State *L);
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
#define gnodelast(h)	gnode(h, cast_sizet(sizenode(h)))


l_mem luaC_objsize (GCObject *o) {
  lu_mem res;
  switch (o->tt) {
    case LUA_VTABLE: {
      res = luaH_size(gco2t(o));
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


static GCObject **getgclist (GCObject *o) {
  switch (o->tt) {
    case LUA_VTABLE: return &gco2t(o)->gclist;
    case LUA_VLCL: return &gco2lcl(o)->gclist;
    case LUA_VCCL: return &gco2ccl(o)->gclist;
    case LUA_VTHREAD: return &gco2th(o)->gclist;
    case LUA_VPROTO: return &gco2p(o)->gclist;
    case LUA_VUPVAL: return &gco2upv(o)->gclist;
    case LUA_VUSERDATA: {
      Udata *u = gco2u(o);
      return &u->gclist;
    }
    default: lua_assert(0); return 0;
  }
}


/*
** Link a collectable object 'o' with a known type into the list 'p'.
** (Must be a macro to access the 'gclist' field in different types.)
*/
#define linkgclist(o,p)	linkgclist_(obj2gco(o), &(o)->gclist, &(p))

static void linkgclist_ (GCObject *o, GCObject **pnext, GCObject **list) {
  lua_assert(!isgray(o));  /* cannot be in a gray list */
  *pnext = *list;
  *list = o;
  set2gray(o);  /* now it is */
}


/*
** Link a generic collectable object 'o' into the list 'p'.
*/
#define linkobjgclist(o,p) linkgclist_(obj2gco(o), getgclist(o), &(p))



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
  else return iswhite(o);
}


/*
** Barrier that moves collector forward, that is, marks the white object
** 'v' being pointed by the black object 'o'.  In the generational
** mode, 'v' must also become old, if 'o' is old; however, it cannot
** be changed directly to OLD, because it may still point to non-old
** objects. So, it is marked as OLD0. In the next cycle it will become
** OLD1, and in the next it will finally become OLD (regular old). By
** then, any object it points to will also be old.  If called in the
** incremental sweep phase, it clears the black object to white (sweep
** it) to avoid other barrier calls for this same object. (That cannot
** be done is generational mode, as its sweep does not distinguish
** white from dead.)
*/
void luaC_barrier_ (lua_State *L, GCObject *o, GCObject *v) {
  global_State *g = G(L);
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  if (keepinvariant(g)) {  /* must keep invariant? */
    reallymarkobject(g, v);  /* restore invariant */
    if (isold(o)) {
      lua_assert(!isold(v));  /* white object could not be old */
      setage(v, G_OLD0);  /* restore generational invariant */
    }
  }
  else {  /* sweep phase */
    lua_assert(issweepphase(g));
    if (g->gckind != KGC_GENMINOR)  /* incremental mode? */
      makewhite(g, o);  /* mark 'o' as white to avoid other barriers */
  }
}


/*
** barrier that moves collector backward, that is, mark the black object
** pointing to a white object as gray again.
*/
void luaC_barrierback_ (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  lua_assert(isblack(o) && !isdead(g, o));
  lua_assert((g->gckind != KGC_GENMINOR)
          || (isold(o) && getage(o) != G_TOUCHED1));
  if (getage(o) == G_TOUCHED2)  /* already in gray list? */
    set2gray(o);  /* make it gray to become touched1 */
  else  /* link it in 'grayagain' and paint it gray */
    linkobjgclist(o, g->grayagain);
  if (isold(o))  /* generational mode? */
    setage(o, G_TOUCHED1);  /* touched in current cycle */
}

static void applybarrier (lua_State *L, GCObject *o, GCObject *v, int back) {
  if (isblack(o)) {
    if (back)
      luaC_barrierback_(L, o);  /* dirty owner, regardless of first child's color */
    else if (iswhite(v))
      luaC_barrier_(L, o, v);
  }
}

/* A live object's allocator link is unused until reclamation. Reuse it for
** claimed work rather than allocating records or growing coroutine buffers.
** The main thread is embedded in global_State and is never a barrier target:
** restartcollection marks it before admitting any mutator. */
static LuaAllocBlock *barrierblock (GCObject *o) {
  char *memory = cast_charp(o);
  if (o->tt == LUA_VTHREAD)
    memory -= offsetof(LX, l);
  return cast(LuaAllocBlock *, memory) - 1;
}

GCObject *luaC_barriernext (GCObject *o) {
  return cast(GCObject *, barrierblock(o)->block.next);
}

static void publishbarrier (global_State *g, GCObject **queue, GCObject *o) {
  LuaAllocBlock *block;
  GCObject *head = __atomic_load_n(queue, __ATOMIC_RELAXED);
  lua_assert(o != obj2gco(mainthread(g)));
  UNUSED(g);
  block = barrierblock(o);
  do {
    block->block.next = cast(LuaAllocBlock *, head);
  } while (!__atomic_compare_exchange_n(queue, &head, o, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/* Mark work can be produced by any execution. Claiming a white object is
** monotonic during tracing; a producer never reads mutable object payloads.
** Its participation covers the claim-to-publication gap at termination. */
void luaC_traceobject (lua_State *L, GCObject *o) {
  lu_byte marked = getmarked(o);
  lu_byte claimed;
  if (!luaC_tracing(L)) return;
  do {
    if (!testbits(marked, WHITEBITS)) return;
    claimed = cast_byte(marked & ~maskcolors);
  } while (!__atomic_compare_exchange_n(&o->marked, &marked, claimed, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
  __atomic_fetch_add(&G(L)->trace_changes, 1, __ATOMIC_RELAXED);
  publishbarrier(G(L), &L->barrier_forward, o);
}

static void drainthreadbarriers (lua_State *L, lua_State *thread) {
  global_State *g = G(L);
  GCObject *o = __atomic_exchange_n(&thread->barrier_back, NULL, __ATOMIC_ACQUIRE);
  while (o != NULL) {
    GCObject *next = luaC_barriernext(o);
    lua_assert(isgray(o));
    *getgclist(o) = g->grayagain;
    g->grayagain = o;
    o = next;
  }
  o = __atomic_exchange_n(&thread->barrier_forward, NULL, __ATOMIC_ACQUIRE);
  while (o != NULL) {
    GCObject *next = luaC_barriernext(o);
    lua_assert(isgray(o));
    linktrace(g, o);
    o = next;
  }
}

void luaC_drainbarriers (lua_State *L) {
  LuaParticipation *participant;
  for (participant = G(L)->world.participants; participant != NULL;
       participant = participant->next)
    drainthreadbarriers(L, participant->thread);
}

/* The colour transition claims the object's one work-list link. Other writers
** observe gray and need no further record. Participation covers the interval
** from claiming to release-publication, so termination cannot miss a producer.
** No allocation, ownership wait or safepoint occurs on this path. */
void luaC_syncbarrier (lua_State *L, GCObject *o, GCObject *v, int back) {
  global_State *g = G(L);
  GCObject *target = back ? o : v;
  lu_byte marked, claimed;
  if (luaC_tracing(L)) {
    luaC_traceobject(L, v);
    /* A concurrent minor can install an already-black, newly allocated
    ** referent into an old owner after its scan. Keep the generational
    ** invariant as well as reachability: OLD0 is swept to OLD1 and scanned
    ** on the next minor, including all of this referent's younger children.
    ** An ordinary color-only barrier would miss this black-to-black write. */
    if (g->gckind == KGC_GENMINOR && isold(o)) {
      marked = getmarked(v);
      while ((marked & AGEBITS) < G_OLD0) {
        claimed = cast_byte((marked & ~AGEBITS) | G_OLD0);
        if (__atomic_compare_exchange_n(&v->marked, &marked, claimed, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
          break;
      }
    }
    return;
  }
  if (luaW_iscollector(L)) {
    applybarrier(L, o, v, back);
    return;
  }
  if (!back && !keepinvariant(g)) {
    makewhite(g, o);  /* sweeping needs no referent publication */
    return;
  }
  marked = getmarked(target);
  do {
    if (back ? !testbit(marked, BLACKBIT) : !testbits(marked, WHITEBITS))
      return;
    claimed = cast_byte(marked & ~maskcolors);
    if (back && (marked & AGEBITS) > G_SURVIVAL)
      claimed = cast_byte((claimed & ~AGEBITS) | G_TOUCHED1);
    else if (!back && isold(o))
      claimed = cast_byte((claimed & ~AGEBITS) | G_OLD0);
  } while (!__atomic_compare_exchange_n(&target->marked, &marked, claimed, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
  /* TOUCHED2 is already on grayagain. Its existing link remains authoritative. */
  if (!back || (marked & AGEBITS) != G_TOUCHED2)
    publishbarrier(g, back ? &L->barrier_back : &L->barrier_forward, target);
}


void luaC_fix (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  GCObject **link;
  GCObject *previous = NULL;
  luaW_heapenter(L);
  link = &L->new_objects;
  while (*link != NULL && *link != o) {
    previous = *link;
    link = &(*link)->next;
  }
  if (*link == o) {
    if (L->new_objects_tail == o) L->new_objects_tail = previous;
  }
  else {
    link = &g->allgc;
    lua_assert(*link == o);  /* initialization's already-published head */
  }
  set2gray(o);  /* they will be gray forever */
  setage(o, G_OLD);  /* and old forever */
  *link = o->next;
  o->next = g->fixedgc;  /* link it to 'fixedgc' list */
  g->fixedgc = o;
  luaW_heapleave(L);
}


/*
** create a new collectable object (with given type, size, and offset)
** and retain it in this coroutine's construction list. Collection publishes
** complete lists under quiescence, before any tracing or reclamation.
*/
GCObject *luaC_newobjdt (lua_State *L, lu_byte tt, size_t sz, size_t offset) {
  global_State *g = G(L);
  char *p;
  GCObject *o;
  p = cast_charp(luaM_newobject(L, novariant(tt), sz));
  o = cast(GCObject *, p + offset);
  setmarked(o, luaC_tracing(L) ? bitmask(BLACKBIT) : luaC_white(g));
  o->tt = tt;
  if (luaW_iscollector(L)) {
    o->next = g->allgc;
    g->allgc = o;
  }
  else {
    o->next = L->new_objects;
    if (L->new_objects == NULL) L->new_objects_tail = o;
    L->new_objects = o;
  }
  return o;
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
static int claimmark (GCObject *o) {
  lu_byte marked = getmarked(o);
  lu_byte claimed;
  do {
    if (!testbits(marked, WHITEBITS)) return 0;
    claimed = cast_byte(marked & ~maskcolors);
  } while (!__atomic_compare_exchange_n(&o->marked, &marked, claimed, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
  return 1;
}

static void reallymarkobject (global_State *g, GCObject *o) {
  if (claimmark(o)) {
    if (__atomic_load_n(&g->trace_active, __ATOMIC_RELAXED))
      __atomic_fetch_add(&g->trace_changes, 1, __ATOMIC_RELAXED);
    linktrace(g, o);
  }
}

/* Shared root/barrier publication owns world.mutex or quiescence. Strings
** are immutable leaves; all other payloads are queued for a scanner. */
static void linktrace (global_State *g, GCObject *o) {
  if (novariant(o->tt) == LUA_TSTRING) {
    g->GCmarked += luaC_objsize(o);
    nw2black(o);
  }
  else {
    *getgclist(o) = g->gray;
    g->gray = o;
  }
}

static void queuework (LuaTraceList *list, GCObject *object) {
  GCObject **next = getgclist(object);
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
  if (!claimmark(object)) return;
  work->claims++;
  if (novariant(object->tt) == LUA_TSTRING) {
    work->marked += luaC_objsize(object);
    nw2black(object);
  }
  else queuework(&work->gray, object);
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
  CClosure *function;
  LuaCoroutineScope *scope;
  LuaTaskRoot *task;
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  for (task = g->tasks; task != NULL; task = task->next)
    markobject(g, task->thread);
  for (scope = g->coroutine_scopes; scope != NULL; scope = scope->next) {
    markobject(g, scope->source);
    markobject(g, scope->target);
  }
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  for (i = 0; i < LUA_MANAGED_SHARDS; i++)
    for (function = g->managed_functions[i]; function != NULL;
         function = function->managed_next)
      markobject(g, function);
  markobjectN(g, g->pipe_meta);
  for (i=0; i < LUA_NUMTYPES; i++)
    markobjectN(g, g->mt[i]);
}


/*
** mark all objects in list of being-finalized
*/
static void markbeingfnz (global_State *g) {
  GCObject *o;
  for (o = g->tobefnz; o != NULL; o = o->next)
    markobject(g, o);
}


static void cleargraylists (global_State *g) {
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

/* Convert the previous live storage estimate to allocation runway at the
** configured marking work/byte ratio. Decode Lua's fixed-point parameter so
** this inverse cannot overflow or lose precision on large heaps. */
static l_mem markrunway (global_State *g) {
  unsigned p = g->gcparams[LUA_GCPSTEPMUL];
  unsigned m = p & 15u, e = p >> 4;
  l_mem live = g->GCcyclelive;
  if (e != 0) { m += 16; e--; }
  if (m == 0) return 0;  /* zero requests complete collections */
  if (e >= 7) return (live >> (e - 7)) / m;
  e = 7 - e;
  if (live / m > MAX_LMEM >> e) return MAX_LMEM;
  {
    l_mem high = (live / m) << e;
    l_mem low = ((live % m) << e) / m;
    return high > MAX_LMEM - low ? MAX_LMEM : high + low;
  }
}

static void starttrace (lua_State *L) {
  global_State *g = G(L);
  l_mem runway, deadline;
  if ((g->gcstp & GCSTPCLS) && !g->gcemergency) return;
  lua_assert(L->world_depth != 0 && !luaC_tracing(L));
  g->trace_epoch++;
  __atomic_fetch_add(&g->world.work_epoch, 1, __ATOMIC_RELEASE);
  g->trace_weakcutoff = 0;
  g->trace_changes = 0;
  g->trace_ready = 0;
  g->trace_deferred = NULL;
  g->trace_plan.again = g->trace_plan.next = NULL;
  g->trace_plan.deferred = g->trace_plan.tail = NULL;
  g->trace_plan.base_marked = g->GCmarked;
  g->trace_plan.phase = TRACE_INIT;
  g->trace_plan.finish = g->gckind == KGC_GENMINOR
                         ? TRACE_YOUNG : TRACE_MAJOR;
  runway = markrunway(g);
  deadline = runway > MAX_LMEM - g->GCgoal ? MAX_LMEM : g->GCgoal + runway;
  __atomic_store_n(&g->GCmarkdebt, deadline - livebytes(g), __ATOMIC_RELAXED);
  __atomic_store_n(&g->trace_active, 1, __ATOMIC_RELEASE);
}

static void endtrace (lua_State *L) {
  global_State *g = G(L);
  LuaWorld *world = &g->world;
  lua_assert(L->world_depth != 0 &&
             !__atomic_load_n(&g->trace_busy, __ATOMIC_ACQUIRE));
  __atomic_store_n(&g->trace_active, 0, __ATOMIC_RELEASE);
  g->trace_weakcutoff = 0;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
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

static void restartcollection (lua_State *L, global_State *g) {
  cleargraylists(g);
  g->GCmarked = 0;
  starttrace(L);
  markobject(g, mainthread(g));
  marknativeframes(g);
  markvalue(g, &g->l_registry);
  markmt(g);
  markbeingfnz(g);  /* mark any finalizing object left from previous cycle */
}

/* }====================================================== */


/*
** {======================================================
** Traverse functions
** =======================================================
*/


/*
** Check whether object 'o' should be kept in the 'grayagain' list for
** post-processing by 'correctgraylist'. (It could put all old objects
** in the list and leave all the work to 'correctgraylist', but it is
** more efficient to avoid adding elements that will be removed.) Only
** TOUCHED1 objects need to be in the list. TOUCHED2 doesn't need to go
** back to a gray list, but then it must become OLD. (That is what
** 'correctgraylist' does when it finds a TOUCHED2 object.)
** This function is a no-op in incremental mode, as objects cannot be
** marked as touched in that mode.
*/
static void genlink (LuaTraceWork *work, GCObject *o) {
  lua_assert(isblack(o));
  if (getage(o) == G_TOUCHED1) {  /* touched in this cycle? */
    linkwork(&work->grayagain, o);
  }  /* everything else do not need to be linked back */
  else if (getage(o) == G_TOUCHED2)
    setage(o, G_OLD);  /* advance age */
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
  Node *n, *limit = gnodelast(h);
  /* if there is array part, assume it may have white values (it is not
     worth traversing it now just to check) */
  int hasclears = (h->asize > 0);
  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    if (isempty(gval(n)))  /* entry is empty? */
      clearkey(n);  /* clear its key */
    else {
      lua_assert(!keyisnil(n));
      tracekey(work, n);
      if (!hasclears && iscleared(work, gcvalueN(gval(n))))
        hasclears = 1;  /* table will have to be cleared */
    }
  }
  if (g->gcstate == GCSpropagate)
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
  unsigned asize = h->asize;
  int marked = 0;  /* true if some object is marked in this traversal */
  unsigned i;
  for (i = 0; i < asize; i++) {
    GCObject *o = gcvalarr(h, i);
    if (o != NULL && iswhite(o)) {
      marked = 1;
      markwork(work, o);
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
  unsigned int i;
  unsigned int nsize = sizenode(h);
  int marked = traversearray(work, h);  /* traverse array part */
  /* Alternate direction between convergence rounds to shorten long chains. */
  for (i = 0; i < nsize; i++) {
    Node *n = inv ? gnode(h, nsize - 1 - i) : gnode(h, i);
    if (isempty(gval(n)))  /* entry is empty? */
      clearkey(n);  /* clear its key */
    else if (iscleared(work, gckeyN(n))) {  /* key is not marked (yet)? */
      hasclears = 1;  /* table must be cleared */
      if (valiswhite(gval(n)))  /* value not marked yet? */
        hasww = 1;  /* white-white entry */
    }
    else if (valiswhite(gval(n))) {  /* value not marked yet? */
      marked = 1;
      markwork(work, gcvalue(gval(n)));
    }
  }
  /* link table into proper list */
  if (g->gcstate == GCSpropagate)
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
  Node *n, *limit = gnodelast(h);
  traversearray(work, h);
  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    if (isempty(gval(n)))  /* entry is empty? */
      clearkey(n);  /* clear its key */
    else {
      lua_assert(!keyisnil(n));
      tracekey(work, n);
      tracevalue(work, gval(n));
    }
  }
  genlink(work, obj2gco(h));
}


/*
** (result & 1) iff weak values; (result & 2) iff weak keys.
*/
static int getmode (global_State *g, Table *h) {
  const TValue *mode;
  int result;
  if (__atomic_load_n(&g->trace_active, __ATOMIC_RELAXED) &&
      h->weak_epoch == g->trace_epoch)
    return h->gcweak;
  mode = gfasttm(g, h->metatable, TM_MODE);
  if (mode == NULL || !ttisstring(mode))
    result = 0;  /* ignore non-string modes */
  else {
    const char *smode = getstr(tsvalue(mode));
    const char *weakkey = strchr(smode, 'k');
    const char *weakvalue = strchr(smode, 'v');
    result = ((weakkey != NULL) << 1) | (weakvalue != NULL);
  }
  if (__atomic_load_n(&g->trace_active, __ATOMIC_RELAXED)) {
    h->gcweak = cast_byte(result);
    h->weak_cleared = 0;
    h->weak_initial_cutoff = cast_byte(__atomic_load_n(&g->trace_weakcutoff,
                                                       __ATOMIC_RELAXED));
    h->weak_epoch = g->trace_epoch;
  }
  return result;
}


static l_mem traversetable (LuaTraceWork *work, Table *h) {
  global_State *g = work->global;
  traceobjectN(work, h->metatable);
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
      if (g->gcstate == GCSpropagate)
        linkwork(&work->grayagain, obj2gco(h));
      else
        linkwork(&work->allweak, obj2gco(h));
      break;
  }
  return cast(l_mem, 1 + 2*sizenode(h) + h->asize);
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

static int scantrace (lua_State *L, LuaTraceWork *work, GCObject *o) {
  global_State *g = G(L);
  if (o->tt == LUA_VTHREAD) {
    lua_State *thread = gco2th(o);
    if (__atomic_load_n(&thread->trace_snapshot, __ATOMIC_ACQUIRE) !=
        g->trace_epoch) {
      int reservation = luaW_iscollector(L) ? 2 : luaX_tryreserve(thread);
      if (reservation == 0) {
        if (!luaC_snapshotparked(L, thread)) return 0;
      }
      else {
        luaC_snapshot(L, thread);
        luaX_endreserve(thread, reservation);
      }
    }
    work->marked += cast(l_mem, thread->trace_bytes);
    nw2black(o);
    if (isold(o)) linkwork(&work->grayagain, o);
    return 1;
  }
  if (o->tt == LUA_VPROTO) {
    Proto *proto = gco2p(o);
    if (__atomic_load_n(&proto->trace_snapshot, __ATOMIC_ACQUIRE) !=
        g->trace_epoch) {
      lua_State *builder = __atomic_load_n(&proto->builder, __ATOMIC_ACQUIRE);
      int reservation = builder != NULL && !luaW_iscollector(L)
        ? luaX_tryreserve(builder) : 2;
      if (reservation == 0) {
        if (!luaC_snapshotparked(L, builder)) return 0;
        lua_assert(proto->trace_snapshot == g->trace_epoch);
      }
      else {
        if (builder != NULL) luaC_traceobject(L, obj2gco(builder));
        luaC_snapshotproto(L, proto);
        if (builder != NULL) luaX_endreserve(builder, reservation);
      }
    }
    work->marked += cast(l_mem, proto->trace_bytes);
    nw2black(o);
    return 1;
  }
  if (o->tt == LUA_VUPVAL) {
    UpVal *upvalue = gco2upv(o);
    if (upvalue->mutex_ready && !luaW_iscollector(L)) {
      int status = pthread_mutex_trylock(&upvalue->mutex);
      if (status == EBUSY) return 0;
      luaX_checksync(status);
      tracevalue(work, &upvalue->value);
      luaX_checksync(pthread_mutex_unlock(&upvalue->mutex));
    }
    else tracevalue(work, &upvalue->value);
    work->marked += sizeof(UpVal);
    nw2black(o);
    return 1;
  }
  if (!luaW_iscollector(L) && !luaA_tryscan(o)) return 0;
  if (o->tt == LUA_VTABLE) {
    Table *table = gco2t(o);
    Table *metatable = table->metatable;
    int pair = metatable != NULL && metatable != table;
    if (pair && !luaW_iscollector(L) && !luaA_tryscan(obj2gco(metatable))) {
      luaA_endscan(L, o);
      return 0;
    }
    work->marked += luaC_objsize(o);
    nw2black(o);
    traversetable(work, table);
    if (pair && !luaW_iscollector(L)) luaA_endscan(L, obj2gco(metatable));
  }
  else {
    work->marked += luaC_objsize(o);
    nw2black(o);
    switch (o->tt) {
      case LUA_VUSERDATA: traverseudata(work, gco2u(o)); break;
      case LUA_VLCL: traverseLclosure(work, gco2lcl(o)); break;
      case LUA_VCCL: traverseCclosure(work, gco2ccl(o)); break;
      default: lua_assert(0); break;
    }
  }
  if (!luaW_iscollector(L)) luaA_endscan(L, o);
  return 1;
}

void luaC_assisttrace (lua_State *L) {
  global_State *g = G(L);
  LuaGCFrame frame;
  lu_mem budget = L->trace_debt;
  int pressure = luaC_markpressure(L);
  if (!gcrunning(g) || (budget < LUA_GC_ASSIST_BYTES && !pressure) ||
      luaC_ingc(L) || L->reclaiming ||
      L->world_depth != 0 || L->object_scopes != NULL ||
      luaW_scope(L).heap != 0 || !luaC_tracing(L))
    return;
  luaC_entergc(L, &frame);
  /* Earlier cycles can still have physical garbage. A stream of new marking
  ** jobs must not monopolize every allocation assist and starve that storage. */
  if (luaC_needassist(L)) luaC_assistreclaim(L);
  if (pressure || L->assist_epoch ==
      __atomic_load_n(&g->world.work_epoch, __ATOMIC_ACQUIRE))
    tracework(L, budget, pressure);
  else L->trace_debt = 0;
  if (luaC_retired(L) != NULL || L->reclaim_pages) luaC_reclaim(L);
  luaC_leavegc(L, &frame);
}

/* Supply marking capacity even when applications stop allocating. This uses
** the VM's existing safepoints, never another carrier or a retained driver.
** One whole-object batch is the minimum slice; it owns no unpublished work
** when the application resumes. */
void luaC_polltrace (lua_State *L) {
  LuaGCFrame frame;
  if (!luaC_tracing(L) || !gcrunning(G(L)) || luaC_ingc(L) || L->reclaiming ||
      L->world_depth != 0 || L->object_scopes != NULL ||
      luaW_scope(L).heap != 0)
    return;
  luaC_entergc(L, &frame);
  tracework(L, 1, 0);
  if (luaC_retired(L) != NULL || L->reclaim_pages) luaC_reclaim(L);
  luaC_leavegc(L, &frame);
}



/* Weak work already owns its live object's allocation scratch link: the
** white-to-gray publication which could use it has completed. Deferral does
** not disturb the weak-list links used by other phases. */
static void deferweak (GCObject **head, GCObject **tail, GCObject *object) {
  barrierblock(object)->block.next = NULL;
  if (*tail != NULL)
    barrierblock(*tail)->block.next = cast(LuaAllocBlock *, object);
  else *head = object;
  *tail = object;
}

static GCObject *takeweak (GCObject **head, GCObject **tail) {
  GCObject *object = *head;
  *head = luaC_barriernext(object);
  if (*head == NULL) *tail = NULL;
  return object;
}



/* Claims/publication serialize list ownership, never payload scanning. A batch
** stays participating until its discoveries are published; stopping the world
** therefore also joins all scanners before a phase can change. */
static void readytrace (lua_State *L) {
  global_State *g = G(L);
  LuaTracePlan *plan = &g->trace_plan;
  __atomic_store_n(&g->trace_ready,
    g->gray != NULL || g->trace_deferred != NULL ||
    plan->next != NULL || plan->deferred != NULL ||
    __atomic_load_n(&g->trace_busy, __ATOMIC_RELAXED) != 0 ||
    __atomic_load_n(&L->barrier_forward, __ATOMIC_ACQUIRE) != NULL,
    __ATOMIC_RELEASE);
}

void luaC_claimtrace (lua_State *L, LuaTraceWork *work, unsigned limit) {
  global_State *g = G(L);
  LuaTracePlan *plan = &g->trace_plan;
  l_mem marked = g->GCmarked;
  work->global = g;
  if (!luaC_tracing(L)) return;
  luaC_drainbarriers(L);
  work->scanned = cast(lu_mem, g->GCmarked - marked);
  if (g->gray == NULL && g->trace_deferred != NULL) {
    g->gray = g->trace_deferred;
    g->trace_deferred = NULL;
  }
  work->phase = TRACE_MARK;
  while (g->gray != NULL && work->count < limit) {
    GCObject *object = g->gray;
    g->gray = *getgclist(object);
    work->objects[work->count++] = object;
  }
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
        plan->next = *getgclist(object);
      }
      else object = takeweak(&plan->deferred, &plan->tail);
      work->objects[work->count++] = object;
    }
  }
  if (work->count != 0)
    __atomic_fetch_add(&g->trace_busy, 1, __ATOMIC_RELAXED);
  readytrace(L);
}

void luaC_scantrace (lua_State *L, LuaTraceWork *work, unsigned limit) {
  unsigned attempted = 0;
  unsigned index;
  int quiescent = luaW_iscollector(L);
  lua_assert(__atomic_load_n(&G(L)->trace_busy, __ATOMIC_RELAXED));
  for (index = 0; index < work->count; index++, attempted++) {
    GCObject *object = work->objects[index];
    if (work->phase == TRACE_MARK) {
      if (!scantrace(L, work, object)) queuework(&work->deferred, object);
    }
    else if (!quiescent && !luaA_tryscan(object))
      deferweak(&work->retry, &work->retry_tail, object);
    else {
      if (work->phase == TRACE_EPHEMERONS) {
        nw2black(object);
        if (traverseephemeron(work, gco2t(object), work->direction))
          work->changed = 1;
      }
      else luaC_prepareweak(L, gco2t(object));
      work->scanned += luaC_objsize(object);
      if (!quiescent) luaA_endscan(L, object);
    }
  }
  /* Consume locally produced edges before returning to the shared queue. */
  while (work->gray.head != NULL && attempted++ < limit) {
    GCObject *object = work->gray.head;
    work->gray.head = *getgclist(object);
    if (!scantrace(L, work, object)) queuework(&work->deferred, object);
  }
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
  if (work->retry != NULL) {
    barrierblock(work->retry_tail)->block.next =
      cast(LuaAllocBlock *, plan->deferred);
    if (plan->deferred == NULL) plan->tail = work->retry_tail;
    plan->deferred = work->retry;
  }
  plan->changed |= work->changed;
  g->GCmarked += work->marked;
  /* Phase decisions join every batch, so claim counts can be published once
  ** with the work rather than contending on one cache line per object. */
  __atomic_fetch_add(&g->trace_changes, work->claims, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&g->trace_busy, 1, __ATOMIC_RELEASE);
  readytrace(L);
}

/* }====================================================== */


/*
** {======================================================
** Sweep Functions
** =======================================================
*/



/* Idle stacks are borrowed under their own gate, then resized under metadata
** ownership. Pin the target before a possible allocation wait: an emergency
** may otherwise reclaim a stack whose last ordinary reference disappeared.
** Running owners perform the same trim at their next VM safepoint. */
static int trimstacks (lua_State *L) {
  global_State *g = G(L);
  LuaWorld *world = &g->world;
  LuaWorldScope scope;
  LuaParticipation *participant;
  uint64_t interrupted = g->trace_epoch;
  lu_byte stopem = g->gcstopem;
  if (g->gcemergency || L->object_scopes != NULL || luaW_scope(L).heap != 0)
    return 1;
  g->gcstopem = 0;
  scope = luaW_suspend(L);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  participant = world->participants;
  while (participant != NULL) {
    lua_State *thread = participant->thread;
    int reservation = 0;
    participant = participant->next;
    if (__atomic_load_n(&thread->trace_snapshot, __ATOMIC_ACQUIRE) ==
        g->trace_epoch)
      reservation = luaX_tryreserve(thread);
    if (reservation == 0) continue;
    thread->native_depth++;
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    luaW_heapenter(L);  /* before any target pointer can become an offset */
    if (g->trace_epoch == interrupted && !thread->heap_waiting &&
        !thread->object_waiting && !thread->reclaiming &&
        thread->trace_trimmed != thread->trace_snapshot) {
      thread->trace_trimmed = thread->trace_snapshot;
      thread->reclaiming = 1;
      luaD_shrinkstack(thread);  /* metadata entry is recursive for this owner */
      luaM_flushdebt(thread);
      thread->reclaiming = 0;
    }
    thread->native_depth--;
    luaX_endreserve(thread, reservation);
    luaW_heapleave(L);
    luaX_pollrequests(L);
    luaQ_poll(L);
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    if (g->trace_epoch != interrupted) break;  /* saved cursor may be gone */
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaW_restore(L, scope, L->native_depth);
  g->gcstopem = stopem;
  return g->trace_epoch == interrupted;
}




/*
** sweep at most 'countin' elements from a list of GCObjects erasing dead
** objects, where a dead object is one marked with the old (non current)
** white; change all non-dead objects back to white (and new), preparing
** for next collection cycle. Return where to continue the traversal or
** NULL if list is finished.
*/
static GCObject **sweeplist (lua_State *L, GCObject **p, l_mem countin) {
  global_State *g = G(L);
  int ow = otherwhite(g);
  int white = luaC_white(g);  /* current white */
  while (*p != NULL && countin-- > 0) {
    GCObject *curr = *p;
    int marked = curr->marked;
    if (isdeadm(ow, marked)) {  /* is 'curr' dead? */
      *p = curr->next;  /* remove 'curr' from list */
      luaC_retireobject(L, curr);
    }
    else {  /* change mark to 'white' and age to 'new' */
      curr->marked = cast_byte((marked & ~maskgcbits) | white | G_NEW);
      p = &curr->next;  /* go to next element */
    }
  }
  return (*p == NULL) ? NULL : p;
}


/*
** sweep a list until a live object (or end of list)
*/
static GCObject **sweeptolive (lua_State *L, GCObject **p) {
  GCObject **old = p;
  do {
    p = sweeplist(L, p, 1);
  } while (p == old);
  return p;
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
  if (!g->gcemergency) {
    int i;
    for (i = 0; i < LUA_STRING_SHARDS; i++) {
      stringtable *tb = &g->strt[i];
      /* An allocation waiter may retain the old bucket allocation. Removal
      ** is safe under quiescence; resizing that allocation must wait. */
      if (__atomic_load_n(&tb->access.owner, __ATOMIC_ACQUIRE) == NULL &&
          tb->size > LUA_STRING_INITIAL_BUCKETS && tb->nuse < tb->size / 4)
        luaS_resize(L, tb, tb->size / 2);
    }
  }
}


/*
** Get the next udata to be finalized from the 'tobefnz' list, and
** link it back into the 'allgc' list.
*/
static GCObject *udata2finalize (global_State *g) {
  GCObject *o = g->tobefnz;  /* get first element */
  lua_assert(tofinalize(o));
  g->tobefnz = o->next;  /* remove it from 'tobefnz' list */
  o->next = g->allgc;  /* return it to 'allgc' list */
  g->allgc = o;
  resetbit(o->marked, FINALIZEDBIT);  /* object is "normal" again */
  if (issweepphase(g))
    makewhite(g, o);  /* "sweep" object */
  else if (getage(o) == G_OLD1)
    g->firstold1 = o;  /* it is the first OLD1 object in the list */
  return o;
}


static void dothecall (lua_State *L, void *ud) {
  UNUSED(ud);
  luaD_callnoyield(L, L->top.p - 2, 0);
}


static void GCTM (lua_State *L) {
  global_State *g = G(L);
  LuaNativeRoot method;
  TValue v;
  lua_assert(!g->gcemergency);
  setgcovalue(L, &v, udata2finalize(g));
  luaR_pushroot(L, &method);
  luaT_gettmbyobj(L, &v, TM_GC, &method);
  if (!ttisnil(&method.value)) {
    TStatus status;
    lu_byte oldah = L->allowhook;
    LuaWorldScope depth;
    /* Embedding limits also account for Lua finalizer instructions. */
    L->allowhook = (gethookmask(L) & LUTHR_LUA_RUNTIME_MASK_FINALIZERS) ? 1 : 0;
    setobj2s(L, L->top.p++, &method.value);  /* push finalizer... */
    setobj2s(L, L->top.p++, &v);  /* ... and its argument */
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
}


/*
** call all pending finalizers
*/
static void callallpendingfinalizers (lua_State *L) {
  global_State *g = G(L);
  while (g->tobefnz)
    GCTM(L);
}


/*
** find last 'next' field in list 'p' list (to add elements in its end)
*/
static GCObject **findlast (GCObject **p) {
  while (*p != NULL)
    p = &(*p)->next;
  return p;
}


/*
** Move all unreachable objects (or 'all' objects) that need
** finalization from list 'finobj' to list 'tobefnz' (to be finalized).
** (Note that objects after 'finobjold1' cannot be white, so they
** don't need to be traversed. In incremental mode, 'finobjold1' is NULL,
** so the whole list is traversed.)
*/
static void separatetobefnz (global_State *g, int all) {
  GCObject *curr;
  GCObject **p = &g->finobj;
  GCObject **lastnext = findlast(&g->tobefnz);
  while ((curr = *p) != g->finobjold1) {  /* traverse all finalizable objects */
    lua_assert(tofinalize(curr));
    if (!(iswhite(curr) || all))  /* not being collected? */
      p = &curr->next;  /* don't bother with it */
    else {
      if (curr == g->finobjsur)  /* removing 'finobjsur'? */
        g->finobjsur = curr->next;  /* correct it */
      *p = curr->next;  /* remove 'curr' from 'finobj' list */
      curr->next = *lastnext;  /* link at the end of 'tobefnz' list */
      *lastnext = curr;
      lastnext = &curr->next;
    }
  }
}


/*
** If pointer 'p' points to 'o', move it to the next element.
*/
static void checkpointer (GCObject **p, GCObject *o) {
  if (o == *p)
    *p = o->next;
}


/*
** Correct pointers to objects inside 'allgc' list when
** object 'o' is being removed from the list.
*/
static void correctpointers (global_State *g, GCObject *o) {
  checkpointer(&g->survival, o);
  checkpointer(&g->old1, o);
  checkpointer(&g->reallyold, o);
  checkpointer(&g->firstold1, o);
}


/*
** if object 'o' has a finalizer, remove it from 'allgc' list (must
** search the list to find it) and link it in 'finobj' list.
*/
static void registerfinalizer (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  GCObject **p;
  if (tofinalize(o)) return;
  for (p = &g->allgc; *p != o; p = &(*p)->next)
    lua_assert(*p != NULL);
  if (issweepphase(g)) {
    makewhite(g, o);
    if (g->sweepgc == &o->next)
      g->sweepgc = p;  /* retain the next unswept link without freeing nodes */
  }
  else
    correctpointers(g, o);
  *p = o->next;
  o->next = g->finobj;
  g->finobj = o;
  l_setbit(o->marked, FINALIZEDBIT);
}

void luaC_publish (lua_State *L) {
  LuaParticipation *participant;
  global_State *g = G(L);
  for (participant = g->world.participants; participant != NULL;
       participant = participant->next) {
    lua_State *thread = participant->thread;
    luaM_flushdebt(thread);
    if (thread->new_objects != NULL) {
      thread->new_objects_tail->next = g->allgc;
      g->allgc = thread->new_objects;
      thread->new_objects = thread->new_objects_tail = NULL;
    }
  }
  for (participant = g->world.participants; participant != NULL;
       participant = participant->next) {
    lua_State *thread = participant->thread;
    unsigned i, count = thread->finalizer_count;
    thread->finalizer_count = 0;
    for (i = 0; i < count; i++)
      registerfinalizer(L, thread->finalizers[i]);
  }
}

void luaC_checkfinalizer (lua_State *L, GCObject *o, Table *mt) {
  global_State *g = G(L);
  LuaNativeRoot object_root, metatable_root, method;
  luaR_pushroot(L, &object_root);
  setgcovalue(L, &object_root.value, o);
  luaR_pushroot(L, &metatable_root);
  sethvalue(L, &metatable_root.value, mt);
  luaR_pushroot(L, &method);
  luaA_tabletm(L, mt, TM_GC, &method);
  if (tofinalize(o) ||                 /* obj. is already marked... */
      ttisnil(&method.value) ||         /* or has no finalizer... */
      (g->gcstp & GCSTPCLS))                   /* or closing state? */
    goto done;  /* nothing to be done */
  else if (luaW_iscollector(L))
    registerfinalizer(L, o);
  else {
    lua_assert(L->finalizer_count < LUA_FINALIZER_CAPACITY);
    L->finalizers[L->finalizer_count++] = o;
    if (L->finalizer_count == LUA_FINALIZER_CAPACITY) {
      luaW_stop(L);
      luaW_start(L);
    }
  }
 done:
  luaR_poproot(L, &method);
  luaR_poproot(L, &metatable_root);
  luaR_poproot(L, &object_root);
}

/* }====================================================== */


/*
** {======================================================
** Generational Collector
** =======================================================
*/

/*
** Trace and generational-mode heuristics use 'GCmarked' and 'GCmajorminor'.
** Allocation pacing instead uses logical live storage and a fixed GCgoal.
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
static l_mem livebytes (global_State *g) {
  l_mem live = gettotalbytes(g) -
    __atomic_load_n(&g->GCretired, __ATOMIC_RELAXED);
  lua_assert(live >= cast(l_mem, sizeof(global_State)));
  return live;
}

static void setidledebt (global_State *g) {
  l_mem debt = g->GCgoal - livebytes(g);
  if (debt < 0) debt = 0;
  luaE_setdebt(g, debt);
}


/*
** Sweep a list of objects to enter generational mode.  Deletes dead
** objects and turns the non dead to old. All non-dead threads---which
** are now old---must be in a gray list. Everything else is not in a
** gray list. Open upvalues are also kept gray.
*/
static void sweep2old (lua_State *L, GCObject **p) {
  GCObject *curr;
  global_State *g = G(L);
  while ((curr = *p) != NULL) {
    if (iswhite(curr)) {  /* is 'curr' dead? */
      lua_assert(isdead(g, curr));
      *p = curr->next;  /* remove 'curr' from list */
      luaC_retireobject(L, curr);
    }
    else {  /* all surviving objects become old */
      setage(curr, G_OLD);
      if (curr->tt == LUA_VTHREAD) {  /* threads must be watched */
        lua_State *th = gco2th(curr);
        linkgclist(th, g->grayagain);  /* insert into 'grayagain' list */
      }
      else  /* everything else is black */
        nw2black(curr);
      p = &curr->next;  /* go to next element */
    }
  }
}


/*
** Sweep for generational mode. Delete dead objects. (Because the
** collection is not incremental, there are no "new white" objects
** during the sweep. So, any white object must be dead.) For
** non-dead objects, advance their ages and clear the color of
** new objects. (Old objects keep their colors.)
** The ages of G_TOUCHED1 and G_TOUCHED2 objects cannot be advanced
** here, because these old-generation objects are usually not swept
** here.  They will all be advanced in 'correctgraylist'. That function
** will also remove objects turned white here from any gray list.
*/
static GCObject **sweepgen (lua_State *L, global_State *g, GCObject **p,
                            GCObject *limit, GCObject **pfirstold1,
                            l_mem *paddedold) {
  static const lu_byte nextage[] = {
    G_SURVIVAL,  /* from G_NEW */
    G_OLD1,      /* from G_SURVIVAL */
    G_OLD1,      /* from G_OLD0 */
    G_OLD,       /* from G_OLD1 */
    G_OLD,       /* from G_OLD (do not change) */
    G_TOUCHED1,  /* from G_TOUCHED1 (do not change) */
    G_TOUCHED2   /* from G_TOUCHED2 (do not change) */
  };
  l_mem addedold = 0;
  int white = luaC_white(g);
  GCObject *curr;
  while ((curr = *p) != limit) {
    if (iswhite(curr)) {  /* is 'curr' dead? */
      lua_assert(!isold(curr) && isdead(g, curr));
      *p = curr->next;  /* remove 'curr' from list */
      luaC_retireobject(L, curr);
    }
    else {  /* correct mark and age */
      int age = getage(curr);
      if (age == G_NEW) {  /* new objects go back to white */
        int marked = curr->marked & ~maskgcbits;  /* erase GC bits */
        curr->marked = cast_byte(marked | G_SURVIVAL | white);
      }
      else {  /* all other objects will be old, and so keep their color */
        lua_assert(age != G_OLD1);  /* advanced in 'markold' */
        setage(curr, nextage[age]);
        if (getage(curr) == G_OLD1) {
          addedold += luaC_objsize(curr);  /* bytes becoming old */
          if (*pfirstold1 == NULL)
            *pfirstold1 = curr;  /* first OLD1 object in the list */
        }
      }
      p = &curr->next;  /* go to next element */
    }
  }
  *paddedold += addedold;
  return p;
}


/*
** Correct a list of gray objects. Return a pointer to the last element
** left on the list, so that we can link another list to the end of
** this one.
** Because this correction is done after sweeping, young objects might
** be turned white and still be in the list. They are only removed.
** 'TOUCHED1' objects are advanced to 'TOUCHED2' and remain on the list;
** Non-white threads also remain on the list. 'TOUCHED2' objects and
** anything else become regular old, are marked black, and are removed
** from the list.
*/
static GCObject **correctgraylist (GCObject **p) {
  GCObject *curr;
  while ((curr = *p) != NULL) {
    GCObject **next = getgclist(curr);
    if (iswhite(curr))
      goto remove;  /* remove all white objects */
    else if (getage(curr) == G_TOUCHED1) {  /* touched in this cycle? */
      lua_assert(isgray(curr));
      nw2black(curr);  /* make it black, for next barrier */
      setage(curr, G_TOUCHED2);
      goto remain;  /* keep it in the list and go to next element */
    }
    else if (curr->tt == LUA_VTHREAD) {
      lua_assert(isgray(curr));
      goto remain;  /* keep non-white threads on the list */
    }
    else {  /* everything else is removed */
      lua_assert(isold(curr));  /* young objects should be white here */
      if (getage(curr) == G_TOUCHED2)  /* advance from TOUCHED2... */
        setage(curr, G_OLD);  /* ... to OLD */
      nw2black(curr);  /* make object black (to be removed) */
      goto remove;
    }
    remove: *p = *next; continue;
    remain: p = next; continue;
  }
  return p;
}


/*
** Correct all gray lists, coalescing them into 'grayagain'.
*/
static void correctgraylists (global_State *g) {
  GCObject **list = correctgraylist(&g->grayagain);
  *list = g->weak; g->weak = NULL;
  list = correctgraylist(list);
  *list = g->allweak; g->allweak = NULL;
  list = correctgraylist(list);
  *list = g->ephemeron; g->ephemeron = NULL;
  correctgraylist(list);
}


/*
** Mark black 'OLD1' objects when starting a new young collection.
** Gray objects are already in some gray list, and so will be visited in
** the atomic step.
*/
static void markold (global_State *g, GCObject *from, GCObject *to) {
  GCObject *p;
  for (p = from; p != to; p = p->next) {
    if (getage(p) == G_OLD1) {
      lua_assert(!iswhite(p));
      setage(p, G_OLD);  /* now they are old */
      if (isblack(p)) {
        if (__atomic_load_n(&g->trace_active, __ATOMIC_RELAXED)) {
          set2gray(p);
          linktrace(g, p);
        }
        else reallymarkobject(g, p);
      }
    }
  }
}


/*
** Finish a young-generation collection.
*/
static void finishgencycle (lua_State *L, global_State *g) {
  l_mem live = g->GCcyclelive;
  l_mem growth = applygcparam(g, MINORMUL, g->GCmajorlive);
  g->GCgoal = growth > MAX_LMEM - live ? MAX_LMEM : live + growth;
  correctgraylists(g);
  checkSizes(L, g);
  g->gcstate = GCSpropagate;  /* skip restart */
  if (g->tobefnz != NULL && !g->gcemergency && luaD_checkminstack(L))
    callallpendingfinalizers(L);
}


/*
** Shifts from a minor collection to major collections. It starts in
** the "sweep all" state to clear all objects, which are mostly black
** in generational mode.
*/
static void minor2inc (lua_State *L, global_State *g, lu_byte kind) {
  g->GCmajorminor = g->GCmarked;  /* number of live bytes */
  g->gckind = kind;
  g->reallyold = g->old1 = g->survival = NULL;
  g->finobjrold = g->finobjold1 = g->finobjsur = NULL;
  entersweep(L);  /* continue as an incremental cycle */
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

/*
** Start a young collection by publishing 'OLD1' work. Its payloads and
** subsequent phases use the same shared slices as a major collection.
*/
static void startyoungcollection (lua_State *L, global_State *g) {
  lua_assert(g->gcstate == GCSpropagate);
  starttrace(L);
  if (g->firstold1) {  /* are there regular OLD1 objects? */
    markold(g, g->firstold1, g->reallyold);  /* mark them */
    g->firstold1 = NULL;  /* no more OLD1 objects (for now) */
  }
  markold(g, g->finobj, g->finobjrold);
  markold(g, g->tobefnz, NULL);
}

static void finishyoung (lua_State *L, global_State *g, l_mem marked) {
  l_mem addedold1 = 0;
  GCObject **psurvival;
  GCObject *dummy;
  /* sweep nursery and get a pointer to its last live element */
  g->gcstate = GCSswpallgc;
  psurvival = sweepgen(L, g, &g->allgc, g->survival, &g->firstold1, &addedold1);
  /* sweep 'survival' */
  sweepgen(L, g, psurvival, g->old1, &g->firstold1, &addedold1);
  g->reallyold = g->old1;
  g->old1 = *psurvival;  /* 'survival' survivals are old now */
  g->survival = g->allgc;  /* all news are survivals */

  /* repeat for 'finobj' lists */
  dummy = NULL;  /* no 'firstold1' optimization for 'finobj' lists */
  psurvival = sweepgen(L, g, &g->finobj, g->finobjsur, &dummy, &addedold1);
  /* sweep 'survival' */
  sweepgen(L, g, psurvival, g->finobjold1, &dummy, &addedold1);
  g->finobjrold = g->finobjold1;
  g->finobjold1 = *psurvival;  /* 'survival' survivals are old now */
  g->finobjsur = g->finobj;  /* all news are survivals */

  sweepgen(L, g, &g->tobefnz, NULL, &dummy, &addedold1);

  /* keep total number of added old1 bytes */
  g->GCmarked = marked + addedold1;

  /* decide whether to shift to major mode */
  if (checkminormajor(g)) {
    minor2inc(L, g, KGC_GENMAJOR);  /* go to major mode */
    g->GCmarked = 0;  /* fresh accounting for the forthcoming major */
  }
  else
    finishgencycle(L, g);  /* still in minor mode; finish it */
}


/*
** Clears all gray lists, sweeps objects, and prepare sublists to enter
** generational mode. The sweeps remove dead objects and turn all
** surviving objects to old. Threads go back to 'grayagain'; everything
** else is turned black (not in any gray list).
*/
static void atomic2gen (lua_State *L, global_State *g) {
  cleargraylists(g);
  /* sweep all elements making them old */
  g->gcstate = GCSswpallgc;
  sweep2old(L, &g->allgc);
  /* everything alive now is old */
  g->reallyold = g->old1 = g->survival = g->allgc;
  g->firstold1 = NULL;  /* there are no OLD1 objects anywhere */

  /* repeat for 'finobj' lists */
  sweep2old(L, &g->finobj);
  g->finobjrold = g->finobjold1 = g->finobjsur = g->finobj;

  sweep2old(L, &g->tobefnz);

  g->gckind = KGC_GENMINOR;
  g->GCmajorlive = livebytes(g);
  g->GCmajorminor = g->GCmarked;  /* "base" for number of bytes */
  g->GCmarked = 0;  /* to count the number of added old1 bytes */
  finishgencycle(L, g);
}


/*
** Finish physical reclamation and refresh the current cycle's fixed budget.
*/
void luaC_finishreclaim (lua_State *L) {
  global_State *g = G(L);
  if (g->gcstp & GCSTPCLS) return;  /* teardown frees pages without admission */
  luaW_stop(L);
  luaM_reclaimpages(L);
  /* A competing collector may have advanced the cycle during destruction.
  ** Reset only the current idle policy, after reclamation credits are exact. */
  if (g->gckind == KGC_GENMINOR || g->gcstate == GCSpause)
    setidledebt(g);
  luaW_start(L);
}


/*
** Enter generational mode. Must go until the end of an atomic cycle
** to ensure that all objects are correctly marked and weak tables
** are cleared. Then, turn all objects into old and finishes the
** collection.
*/
static void entergen (lua_State *L, global_State *g) {
  luaC_runtilstate(L, GCSpause, 1);
  if (g->gckind == KGC_GENMINOR) return;
  luaC_runtilstate(L, GCSpropagate, 1);
  g->trace_plan.finish = TRACE_GENERATIONAL;
  atomic(L);
  L->reclaim_pages = 1;
}


/*
** Change collector mode to 'newmode'.
*/
void luaC_changemode (lua_State *L, int newmode) {
  global_State *g = G(L);
  if (g->gckind == KGC_GENMAJOR)  /* doing major collections? */
    g->gckind = KGC_INC;  /* already incremental but in name */
  if (newmode != g->gckind) {  /* does it need to change? */
    if (newmode == KGC_INC)  /* entering incremental mode? */
      minor2inc(L, g, KGC_INC);  /* entering incremental mode */
    else {
      lua_assert(newmode == KGC_GENMINOR);
      entergen(L, g);
    }
  }
}


/*
** Does a full collection in generational mode.
*/
static void fullgen (lua_State *L, global_State *g) {
  minor2inc(L, g, KGC_INC);
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
  if (g->gckind == KGC_GENMAJOR) {  /* generational mode? */
    l_mem numbytes = gettotalbytes(g);
    l_mem addedbytes = numbytes - g->GCmajorminor;
    l_mem limit = applygcparam(g, MAJORMINOR, addedbytes);
    l_mem tobecollected = numbytes - g->GCmarked;
    if (tobecollected > limit) {
      atomic2gen(L, g);  /* return to generational mode */
      setidledebt(g);
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
** Enter first sweep phase.
** The call to 'sweeptolive' makes the pointer point to an object
** inside the list (instead of to the header), so that the real sweep do
** not need to skip objects created between "now" and the start of the
** real sweep.
*/
static void entersweep (lua_State *L) {
  global_State *g = G(L);
  g->GCcyclelive = livebytes(g);
  g->gcstate = GCSswpallgc;
  lua_assert(g->sweepgc == NULL);
  g->sweepgc = sweeptolive(L, &g->allgc);
}


/*
** Delete all objects in list 'p' until (but not including) object
** 'limit'.
*/
static void deletelist (lua_State *L, GCObject *p, GCObject *limit) {
  while (p != limit) {
    GCObject *next = p->next;
    luaC_retireobject(L, p);
    p = next;
  }
}


/*
** Call all finalizers of the objects in the given Lua state, and
** then free all objects, except for the main thread.
*/
void luaC_freeallobjects (lua_State *L) {
  global_State *g = G(L);
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
  separatetobefnz(g, 1);  /* separate all objects with finalizers */
  lua_assert(g->finobj == NULL);
  callallpendingfinalizers(L);
  deletelist(L, g->allgc, obj2gco(mainthread(g)));
  lua_assert(g->finobj == NULL);  /* no new finalizers */
  luaC_reclaim(L);  /* external callbacks precede fixed DLL handle destruction */
  deletelist(L, g->fixedgc, NULL);  /* collect fixed objects */
  luaC_reclaim(L);  /* shutdown owns the complete VM until destruction ends */
  { int i; for (i = 0; i < LUA_STRING_SHARDS; i++)
      lua_assert(g->strt[i].nuse == 0); }
}


void luaC_initialtrace (lua_State *L) {
  global_State *g = G(L);
  lua_assert(L->world_depth != 0 && luaC_tracing(L));
  g->trace_plan.again = g->grayagain;
  g->grayagain = NULL;
  lua_assert(g->ephemeron == NULL && g->weak == NULL);
  lua_assert(!iswhite(mainthread(g)));
  g->gcstate = GCSatomic;
  g->trace_plan.phase = TRACE_MARK;
  markobject(g, L);  /* mark running thread */
  marknativeframes(g);
  /* registry and global metatables may be changed by API */
  markvalue(g, &g->l_registry);
  markmt(g);  /* mark global metatables */
}

void luaC_finalizetrace (lua_State *L) {
  global_State *g = G(L);
  lua_assert(L->world_depth != 0);
  separatetobefnz(g, 0);  /* separate objects to be finalized */
  markbeingfnz(g);  /* mark objects that will be finalized */
}

void luaC_finishtrace (lua_State *L) {
  global_State *g = G(L);
  unsigned finish = g->trace_plan.finish;
  l_mem marked = g->trace_plan.base_marked;
  lua_assert(L->world_depth != 0 && luaC_tracing(L));
  luaS_clearcache(g);
  g->GCcyclelive = livebytes(g);
  endtrace(L);
  g->trace_plan.finish = 0;
  g->currentwhite = cast_byte(otherwhite(g));  /* flip current white */
  lua_assert(g->gray == NULL);
  if (finish == TRACE_YOUNG) {
    finishyoung(L, g, marked);
    setidledebt(g);
  }
  else if (finish == TRACE_GENERATIONAL) {
    atomic2gen(L, g);
    setidledebt(g);
    L->reclaim_pages = 1;
  }
  else if (!checkmajorminor(L, g))
    entersweep(L);
}

/* Only scalar tickets and ownership scopes survive a scheduler poll. Every
** graph cursor is shared, so another execution can finish this very job. */
static lu_mem tracework (lua_State *L, lu_mem budget, int complete) {
  global_State *g = G(L);
  uint64_t epoch = g->trace_epoch;
  lu_mem worked = 0;
  if (g->gcemergency || L->object_scopes != NULL || luaW_scope(L).heap != 0) {
    /* These callers may borrow mutable storage which cannot cross admission.
    ** Emergency retries must finish before their allocator resumes. */
    while (luaC_tracing(L) && g->trace_epoch == epoch) {
      luaC_advancetrace(L);
      if (!luaC_tracing(L) || g->trace_epoch != epoch) break;
      luaC_tracebatch(L, LUA_TRACE_BATCH);
    }
  }
  else {
    LuaWorldScope scope;
    if (!__atomic_load_n(&g->trace_ready, __ATOMIC_ACQUIRE) &&
        g->trace_plan.finish != 0)
      luaC_advancetrace(L);
    if (L->world_depth != 0) g->gcstopem = 0;
    scope = luaW_suspend(L);
    while (luaC_tracing(L) && g->trace_epoch == epoch) {
      lu_mem done;
      if (!__atomic_load_n(&g->trace_ready, __ATOMIC_ACQUIRE) &&
          g->trace_plan.finish != 0)
        luaC_advancetrace(L);
      if (!luaC_tracing(L) || g->trace_epoch != epoch) break;
      done = luaC_tracebatch(L, LUA_TRACE_BATCH);
      worked = done > cast(lu_mem, MAX_LMEM) - worked
        ? cast(lu_mem, MAX_LMEM) : worked + done;
      if (!complete && (worked >= budget || done == 0)) {
        /* The last publisher also services termination. Deferring an empty
        ** job until another allocation slice would retain every black object
        ** allocated in that gap and inflate the following cycle's live base. */
        if (luaC_tracing(L) && g->trace_epoch == epoch &&
            g->trace_plan.finish != 0 &&
            !__atomic_load_n(&g->trace_ready, __ATOMIC_ACQUIRE))
          luaC_advancetrace(L);
        break;
      }
      luaX_pollrequests(L);
      luaQ_poll(L);
      if (done == 0) {
        luaX_checksync(pthread_mutex_lock(&g->world.mutex));
        if (luaC_tracing(L) &&
            __atomic_load_n(&g->trace_busy, __ATOMIC_ACQUIRE))
          luaW_waitcollector(L);
        luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
      }
    }
    luaW_restore(L, scope, L->native_depth);
  }
  if (g->trace_epoch == epoch && L->assist_epoch ==
      __atomic_load_n(&g->world.work_epoch, __ATOMIC_ACQUIRE))
    L->trace_debt = worked >= L->trace_debt ? 0 : L->trace_debt - worked;
  return worked;
}

static int atomic (lua_State *L) {
  global_State *g = G(L);
  uint64_t epoch = g->trace_epoch;
  tracework(L, cast(lu_mem, MAX_LMEM), 1);
  return g->trace_epoch == epoch;
}


/*
** Do a sweep step. The normal case (not fast) sweeps at most GCSWEEPMAX
** elements. The fast case sweeps the whole list.
*/
static void sweepstep (lua_State *L, global_State *g,
                       lu_byte nextstate, GCObject **nextlist, int fast) {
  if (g->sweepgc)
    g->sweepgc = sweeplist(L, g->sweepgc, fast ? MAX_LMEM : GCSWEEPMAX);
  else {  /* enter next state */
    g->gcstate = nextstate;
    g->sweepgc = nextlist;
  }
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


static l_mem singlestep (lua_State *L, int fast) {
  global_State *g = G(L);
  l_mem stepresult;
  lua_assert(!g->gcstopem);  /* collector is not reentrant */
  g->gcstopem = 1;  /* no emergency collections while collecting */
  switch (g->gcstate) {
    case GCSpause: {
      restartcollection(L, g);
      g->gcstate = GCSpropagate;
      stepresult = luaC_tracing(L) ? 0 : 1;
      break;
    }
    case GCSpropagate: {
      lua_assert(luaC_tracing(L));
      g->gcstate = GCSenteratomic;
      stepresult = 0;
      break;
    }
    case GCSenteratomic: case GCSatomic: {
      if (!atomic(L)) {
        stepresult = stepinterrupted;
        break;
      }
      if (g->gckind == KGC_GENMINOR)
        stepresult = step2minor;
      else
        stepresult = atomicstep;
      break;
    }
    case GCSswpallgc: {  /* sweep "regular" objects */
      sweepstep(L, g, GCSswpfinobj, &g->finobj, fast);
      stepresult = GCSWEEPMAX;
      break;
    }
    case GCSswpfinobj: {  /* sweep objects with finalizers */
      sweepstep(L, g, GCSswptobefnz, &g->tobefnz, fast);
      stepresult = GCSWEEPMAX;
      break;
    }
    case GCSswptobefnz: {  /* sweep objects to be finalized */
      sweepstep(L, g, GCSswpend, NULL, fast);
      stepresult = GCSWEEPMAX;
      break;
    }
    case GCSswpend: {  /* finish sweeps */
      checkSizes(L, g);
      g->gcstate = GCScallfin;
      stepresult = GCSWEEPMAX;
      break;
    }
    case GCScallfin: {  /* call finalizers */
      if (g->tobefnz && !g->gcemergency && luaD_checkminstack(L)) {
        g->gcstopem = 0;  /* ok collections during finalizers */
        GCTM(L);  /* call one finalizer */
        stepresult = CWUFIN;
      }
      else {  /* no more finalizers or emergency mode or not enough stack
                 to run finalizers */
        g->gcstate = GCSpause;  /* finish collection */
        /* Allocations made after marking already consume the next cycle's
        ** allowance. They must not enlarge its live base before being traced. */
        g->GCgoal = applygcparam(g, PAUSE, g->GCcyclelive);
        stepresult = step2pause;
      }
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
  lua_assert(g->gckind != KGC_GENMINOR);
  while (state != g->gcstate && g->gckind != KGC_GENMINOR) {
    l_mem result = singlestep(L, fast);
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
static void incstep (lua_State *L, global_State *g, l_mem debt) {
  l_mem stepsize = applygcparam(g, STEPSIZE, 100);
  l_mem work2do = applygcparam(g, STEPMUL, stepsize / cast_int(sizeof(void*)));
  l_mem stres;
  int fast = (work2do == 0);  /* special case: do a full collection */
  /* Admission and local accounting can accumulate several workers' bytes
  ** beyond the trigger. Pay for that overshoot before resetting the debt;
  ** otherwise a faster allocator can outrun a fixed tracing quantum forever.
  ** Explicit basic steps pass zero and retain the configured work size. */
  if (!fast && debt < 0) {
    l_mem allocated = stepsize > MAX_LMEM + debt ? MAX_LMEM : stepsize - debt;
    work2do = applygcparam(g, STEPMUL, allocated / cast_int(sizeof(void*)));
  }
  do {  /* repeat until enough work */
    if (!fast && luaC_tracing(L)) {
      uint64_t epoch = g->trace_epoch;
      lu_mem budget = work2do > MAX_LMEM / cast_int(sizeof(void*))
        ? cast(lu_mem, MAX_LMEM)
        : cast(lu_mem, work2do * cast_int(sizeof(void*)));
      tracework(L, budget, 0);
      if (g->trace_epoch != epoch || g->gckind == KGC_GENMINOR) return;
      break;
    }
    stres = singlestep(L, fast);  /* perform one single step */
    if (stres == stepinterrupted) return;
    if (stres == step2minor)  /* returned to minor collections? */
      return;  /* nothing else to be done here */
    else if (stres == step2pause || (stres == atomicstep && !fast))
      break;  /* end of cycle or atomic */
    else
      work2do -= stres;
  } while (fast || work2do > 0);
  if (g->gcstate == GCSpause)
    setidledebt(g);
  else
    luaE_setdebt(g, stepsize);
}


#if !defined(luai_tracegc)
#define luai_tracegc(L,f)		((void)0)
#endif

/*
** Performs a basic GC step while collection is enabled. Explicit API steps
** temporarily enable collection while already owning the collector.
*/
void luaC_step (lua_State *L) {
  global_State *g = G(L);
  LuaGCFrame frame;
  int reclaim = 0;
  int claimed = 0;
  l_mem debt;
  if (luaC_tracing(L)) { luaC_assisttrace(L); return; }
  if (L->world_depth == 0 &&
      (L->object_scopes != NULL || luaW_scope(L).heap != 0)) return;
  if ((!gcrunning(g) && !luaC_ingc(L)) || !luaC_needstep(L)) return;
  if (L->world_depth == 0 && luaC_needassist(L))
    luaC_assistreclaim(L);
  if (L->world_depth == 0) {
    unsigned expected = 0;
    /* One automatic request can wait for metadata while other mutators keep
    ** working. Explicit steps already own the collector and must still run. */
    if (__atomic_load_n(&g->gcstep_pending, __ATOMIC_RELAXED) ||
        !__atomic_compare_exchange_n(&g->gcstep_pending, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return;
    claimed = 1;
  }
  /* Another collector can service this request while metadata access waits.
  ** Revalidate under metadata ownership before requesting global quiescence.
  ** Include unpublished local debt, just like the allocation-side check.
  ** Explicit API steps set their debt while already owning the collector. */
  luaW_collectenter(L);
  if (luaC_tracing(L) || (!gcrunning(g) && !luaC_ingc(L)) ||
      !luaC_needstep(L)) {
    luaW_heapleave(L);
    goto done;
  }
  luaW_stop(L);
  luaW_heapleave(L);  /* retain exactly the ownership acquired by luaW_stop */
  debt = claimed ? __atomic_load_n(&g->GCdebt, __ATOMIC_RELAXED) : 0;
  if (claimed) {
    /* The admission request is complete. A parked driver must not suppress
    ** future cycles after another execution finishes the shared job. */
    __atomic_store_n(&g->gcstep_pending, 0, __ATOMIC_RELEASE);
    claimed = 0;
  }
  luaC_entergc(L, &frame);
  lua_assert(!g->gcemergency);
  luai_tracegc(L, 1);  /* for internal debugging */
  switch (g->gckind) {
    case KGC_INC: case KGC_GENMAJOR:
      incstep(L, g, debt);
      reclaim = !luaC_tracing(L) &&
                (g->gcstate == GCSpause || g->gckind == KGC_GENMINOR);
      break;
    case KGC_GENMINOR: {
      l_mem budget = applygcparam(g, STEPMUL,
                        applygcparam(g, STEPSIZE, 100));
      uint64_t epoch;
      startyoungcollection(L, g);
      epoch = g->trace_epoch;
      tracework(L, cast(lu_mem, budget), budget == 0);
      if (g->trace_epoch != epoch) break;
      if (!luaC_tracing(L)) setidledebt(g);
      reclaim = !luaC_tracing(L);
      break;
    }
  }
  luai_tracegc(L, 0);  /* for internal debugging */
  /* Retiring all allocation caches and scanning every page belongs to a
  ** completed collection, not each marking/partial-sweep step. Emergency and
  ** explicit full collections still reclaim before retrying an allocation. */
  if (reclaim) L->reclaim_pages = 1;
  luaW_start(L);
  if (!L->reclaiming && L->world_depth == 0 &&
      (luaC_retired(L) != NULL || L->reclaim_pages))
    luaC_reclaim(L);
  luaC_leavegc(L, &frame);
 done:
  if (claimed)
    __atomic_store_n(&g->gcstep_pending, 0, __ATOMIC_RELEASE);
}


/*
** Perform a full collection in incremental mode.
** Before running the collection, check 'keepinvariant'; if it is true,
** there may be some objects marked as black, so the collector has
** to sweep all objects to turn them back to white (as white has not
** changed, nothing will be collected).
*/
static void fullinc (lua_State *L, global_State *g) {
  uint64_t interrupted = g->trace_interrupt;
  if (keepinvariant(g) && !luaC_tracing(L))  /* black objects without a trace? */
    entersweep(L); /* sweep everything to turn them back to white */
  /* finish any pending sweep phase to start a new cycle */
  luaC_runtilstate(L, GCSpause, 1);
  if (g->trace_interrupt != interrupted || g->gckind == KGC_GENMINOR) return;
  luaC_runtilstate(L, GCScallfin, 1);  /* run up to finalizers */
  if (g->trace_interrupt != interrupted || g->gckind == KGC_GENMINOR) return;
  luaC_runtilstate(L, GCSpause, 1);  /* finish collection */
  setidledebt(g);
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
  luaW_stop(L);
  luaC_entergc(L, &frame);
  lua_assert(!g->gcemergency);
  interrupted = g->trace_interrupt;
  g->gcemergency = cast_byte(isemergency);  /* set flag */
  if (luaC_tracing(L)) {
    if (isemergency) {
      g->trace_interrupt++;
      L->trace_snapshot = 0;
      luaC_snapshot(L, L);
    }
    atomic(L);
  }
  kind = g->gckind;
  switch (kind) {
    case KGC_GENMINOR: fullgen(L, g); break;
    case KGC_INC: case KGC_GENMAJOR: fullinc(L, g); break;
  }
  L->reclaim_pages = 1;
  /* An allocation retry may still borrow its caller's stack/object storage.
  ** Finish emergency destruction before relinquishing that quiescent scope. */
  if (isemergency) luaC_reclaim(L);
  g->gcemergency = 0;
  if (!isemergency && g->trace_interrupt != interrupted && g->tobefnz != NULL)
    callallpendingfinalizers(L);
  if (!isemergency) trimstacks(L);
  luaW_start(L);
  if (!L->reclaiming && L->world_depth == 0 &&
      (luaC_retired(L) != NULL || L->reclaim_pages))
    luaC_reclaim(L);
  luaC_leavegc(L, &frame);
}

/* }====================================================== */


