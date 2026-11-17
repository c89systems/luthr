/*
** $Id: lstate.c $
** Global State
** See Copyright Notice in lua.h
*/

#define lstate_c
#define LUA_CORE

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "lprefix.h"


#include <stddef.h>
#include <string.h>
#include <pthread.h>
#if defined(__linux__) && !defined(__GLIBC__)
#include <inttypes.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "lua.h"

#include "lapi.h"
#include "laccess.h"
#include "lcallback.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"



#define fromstate(L)	(cast(LX *, cast(lu_byte *, (L)) - offsetof(LX, l)))


/*
** these macros allow user-specific actions when a thread is
** created/deleted
*/
#if !defined(luai_userstateopen)
#define luai_userstateopen(L)		((void)L)
#endif

#if !defined(luai_userstateclose)
#define luai_userstateclose(L)		((void)L)
#endif

#if !defined(luai_userstatethread)
#define luai_userstatethread(L,L1)	((void)L)
#endif

#if !defined(luai_userstatefree)
#define luai_userstatefree(L,L1)	((void)L)
#endif


/*
** set GCdebt to a new value keeping the real number of allocated
** objects (GCtotalobjs - GCdebt) invariant and avoiding overflows in
** 'GCtotalobjs'.
*/
void luaE_setdebt (global_State *g, l_mem debt) {
  l_mem tb = gettotalbytes(g);
  lua_assert(tb > 0);
  if (debt > MAX_LMEM - tb)
    debt = MAX_LMEM - tb;  /* will make GCtotalbytes == MAX_LMEM */
  g->GCtotalbytes = tb + debt;
  __atomic_store_n(&g->GCdebt, debt, __ATOMIC_RELAXED);
  __atomic_store_n(&g->GCreclaimed, 0, __ATOMIC_RELAXED);
}


CallInfo *luaE_extendCI (lua_State *L, int err) {
  CallInfo *ci;
  ci = luaM_newframe(L);
  if (l_unlikely(ci == NULL)) {  /* allocation failed? */
    if (err)
      luaM_error(L);  /* raise the error */
    return NULL;  /* else only report it */
  }
  ci->next = L->ci->next;
  ci->previous = L->ci;
  L->ci->next = ci;
  if (ci->next)
    ci->next->previous = ci;
  ci->u.l.trap = 0;
  L->nci++;
  return ci;
}


/*
** free all CallInfo structures not in use by a thread
*/
static void freeCI (lua_State *owner, lua_State *L) {
  CallInfo *ci = L->ci;
  CallInfo *next = ci->next;
  ci->next = NULL;
  while ((ci = next) != NULL) {
    next = ci->next;
    luaM_free(owner, ci);
    L->nci--;
  }
}


/*
** free half of the CallInfo structures not in use by a thread,
** keeping the first one.
*/
void luaE_shrinkCI (lua_State *L) {
  CallInfo *ci = L->ci->next;  /* first free CallInfo */
  CallInfo *next;
  if (ci == NULL)
    return;  /* no extra elements */
  while ((next = ci->next) != NULL) {  /* two extra elements? */
    CallInfo *next2 = next->next;  /* next's next */
    ci->next = next2;  /* remove next from the list */
    L->nci--;
    luaM_free(L, next);  /* free next */
    if (next2 == NULL)
      break;  /* no more elements */
    else {
      next2->previous = ci;
      ci = next2;  /* continue */
    }
  }
}


/* Cache native bounds per physical thread; refresh on outer VM entry. */
LUAI_DDEF __thread NativeStackContext luaE_native_stack_context;
static __thread NativeStackContext *native_stack_override;

NativeStackContext *luaE_getnativestackcontext (void) {
  return native_stack_override != NULL ? native_stack_override
                                      : &luaE_native_stack_context;
}

NativeStackContext *luaE_setnativestackcontext (NativeStackContext *context) {
  NativeStackContext *previous = native_stack_override;
  native_stack_override = context;
  return previous;
}

#if defined(__linux__) && !defined(__GLIBC__)
/* musl reports only the initial thread's mapped pages. Linux can grow that
** stack to its resource limit, bounded by the preceding mapping. Do not
** extend a pthread stack inherited by a child forked from a worker. */
static uintptr_t mainstacklow (uintptr_t mappedlow) {
  struct rlimit limit;
  FILE *maps;
  char line[128];
  uintptr_t previous = 0, result = mappedlow;
  uintptr_t position = luaD_nativeposition();
  if (syscall(SYS_gettid) != getpid() || getrlimit(RLIMIT_STACK, &limit) != 0)
    return result;
  maps = fopen("/proc/self/maps", "re");
  if (maps == NULL)
    return result;
  while (fgets(line, sizeof(line), maps) != NULL) {
    uintptr_t from, to;
    int valid = sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &from, &to) == 2;
    int mainstack = strstr(line, "[stack]") != NULL;
    if (strchr(line, '\n') == NULL) {
      int c;
      do { c = fgetc(maps); } while (c != '\n' && c != EOF);
    }
    if (!valid)
      continue;
    if (from <= position && position < to) {
      if (mainstack) {
        uintptr_t low = limit.rlim_cur >= to ? 0 : to - limit.rlim_cur;
        if (low < previous) low = previous;
        if (low < result) result = low;
      }
      break;
    }
    previous = to;
  }
  fclose(maps);
  return result;
}
#endif

uintptr_t luaE_initnativestack (void) {
  uintptr_t low = 0;
  size_t size = 0;
#if defined(__linux__)
  pthread_attr_t attributes;
  void *base = NULL;
  if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
    int status = pthread_attr_getstack(&attributes, &base, &size);
    pthread_attr_destroy(&attributes);
    if (status == 0)
      low = cast(uintptr_t, base);
    else
      size = 0;
  }
#elif defined(__APPLE__)
  uintptr_t high = cast(uintptr_t, pthread_get_stackaddr_np(pthread_self()));
  size = pthread_get_stacksize_np(pthread_self());
  if (high >= size)
    low = high - size;
  else
    size = 0;
#else
#error Unsupported Luthr Lua native stack platform
#endif
  if (low <= UINTPTR_MAX - size) {
#if defined(__linux__) && !defined(__GLIBC__)
    if (size != 0) {
      uintptr_t high = low + size;
      low = mainstacklow(low);
      size = high - low;
    }
#endif
    if (size > 32768)
      return low + 32768;
  }
  return UINTPTR_MAX;  /* retry failed discovery on the next outer entry */
}

void luaE_nativestackerror (lua_State *L, uintptr_t position) {
  if (L->luthr_stack_limit == UINTPTR_MAX) {
    if (!completestate(G(L)))
      luaD_throw(L, LUA_ERRMEM);
    luaD_checkstack(L, LUA_MINSTACK);
    setsvalue2s(L, L->top.p, luaS_newliteral(L, "native stack bounds unavailable"));
    L->top.p++;
    luaD_throw(L, LUA_ERRRUN);
  }
  if (position <= L->luthr_stack_limit - 24576) {
    luaD_checkstack(L, LUA_MINSTACK);
    luaD_errerr(L);
  }
  if (!L->luthr_stack_context->overflow) {
    L->luthr_stack_context->overflow = 1;
    luaD_checkstack(L, LUA_MINSTACK);
    luaG_runerror(L, "C stack overflow");
  }
}

/*
** Called when 'getCcalls(L)' larger or equal to LUAI_MAXCCALLS.
** If equal, raises an overflow error. If value is larger than
** LUAI_MAXCCALLS (which means it is handling an overflow) but
** not much larger, does not report an error (to allow overflow
** handling to work).
*/
void luaE_checkcstack (lua_State *L) {
  if (getCcalls(L) == LUAI_MAXCCALLS)
    luaG_runerror(L, "C stack overflow");
  else if (getCcalls(L) >= (LUAI_MAXCCALLS / 10 * 11))
    luaD_errerr(L);  /* error while handling stack error */
}


LUAI_FUNC void luaE_incCstack (lua_State *L) {
  L->nCcalls++;
  luaD_checknativestack(L);
  if (l_unlikely(getCcalls(L) >= LUAI_MAXCCALLS))
    luaE_checkcstack(L);
}


static void resetCI (lua_State *L) {
  CallInfo *ci = L->ci = &L->base_ci;
  ci->func.p = L->stack.p;
  setnilvalue2s(ci->func.p);  /* 'function' entry for basic 'ci' */
  ci->top.p = ci->func.p + 1 + LUA_MINSTACK;  /* +1 for 'function' entry */
  ci->u.c.k = NULL;
  ci->callstatus = CIST_C;
  L->status = LUA_OK;
  L->errfunc = 0;  /* stack unwind can "throw away" the error function */
}


static void stack_init (lua_State *L1, lua_State *L) {
  int i;
  /* initialize stack array */
  L1->stack.p = luaM_newvector(L, BASIC_STACK_SIZE + EXTRA_STACK, StackValue);
  L1->tbclist.p = L1->stack.p;
  for (i = 0; i < BASIC_STACK_SIZE + EXTRA_STACK; i++)
    setnilvalue2s(L1->stack.p + i);  /* erase new stack */
  L1->stack_last.p = L1->stack.p + BASIC_STACK_SIZE;
  /* initialize first ci */
  resetCI(L1);
  L1->top.p = L1->stack.p + 1;  /* +1 for 'function' entry */
}


static void freestack (lua_State *owner, lua_State *L) {
  if (L->stack.p == NULL)
    return;  /* stack not completely built yet */
  L->ci = &L->base_ci;  /* free the entire 'ci' list */
  freeCI(owner, L);
  lua_assert(L->nci == 0);
  /* free stack */
  luaM_freearray(owner, L->stack.p, cast_sizet(stacksize(L) + EXTRA_STACK));
}


/*
** Create registry table and its predefined values
*/
static void init_registry (lua_State *L, global_State *g) {
  /* create registry */
  TValue aux;
  Table *registry = luaH_new(L);
  sethvalue(L, &g->l_registry, registry);
  luaH_resize(L, registry, LUA_RIDX_LAST, 0);
  /* registry[1] = false */
  setbfvalue(&aux);
  luaH_setint(L, registry, 1, &aux);
  /* registry[LUA_RIDX_MAINTHREAD] = L */
  setthvalue(L, &aux, L);
  luaH_setint(L, registry, LUA_RIDX_MAINTHREAD, &aux);
  /* registry[LUA_RIDX_GLOBALS] = new table (table of globals) */
  sethvalue(L, &aux, luaH_new(L));
  luaH_setint(L, registry, LUA_RIDX_GLOBALS, &aux);
}


/*
** open parts of the state that may cause memory-allocation errors.
*/
static void f_luaopen (lua_State *L, void *ud) {
  global_State *g = G(L);
  UNUSED(ud);
  stack_init(L, L);  /* init stack */
  init_registry(L, g);
  luaS_init(L);
  luaT_init(L);
  luaX_init(L);
  g->operation_error = luaS_newliteral(L, "cannot close a coroutine with active operations");
  luaC_fix(L, obj2gco(g->operation_error));
  g->gcstp = 0;  /* allow gc */
  setnilvalue(&g->nilvalue);  /* now state is complete */
  luai_userstateopen(L);
}


/*
** preinitialize a thread with consistent values without allocating
** any memory (to avoid errors)
*/
static int preinit_thread (lua_State *L, global_State *g) {
  int i, j;
  G(L) = g;
  L->stack.p = NULL;
  L->ci = NULL;
  L->nci = 0;
  L->nCcalls = 0;
  L->luthr_stack_limit = 0;
  L->luthr_stack_context = NULL;
  L->native_roots = NULL;
  L->policy = NULL;
  L->barrier_forward = L->barrier_back = NULL;
  L->trace_snapshot = 0;
  L->trace_trimmed = 0;
  L->trace_bytes = 0;
  L->building_protos = NULL;
  L->retired = L->retired_tail = NULL;
  L->reclaiming = L->reclaim_pages = 0;
  L->reclaim_next = NULL;
  L->reclaim_workers = 0;
  L->reclaim_registered = 0;
  L->assist_debt = 0;
  L->trace_debt = 0;
  L->assist_epoch = 0;
  L->new_objects = L->new_objects_tail = NULL;
  L->allocation_debt = 0;
  L->reclaim_credit = 0;
  L->retired_credit = 0;
  L->finalizer_count = 0;
  for (i = 0; i < LUA_ALLOC_POOLS; i++)
    L->allocation_cache[i] = NULL;
  for (i = 0; i < STRCACHE_N; i++)
    for (j = 0; j < STRCACHE_M; j++)
      L->strcache[i][j] = NULL;
  L->participation.next = NULL;
  L->participation.previous = NULL;
  L->participation.thread = L;
  L->participation.active = 0;
  L->admission = &L->participation;
  L->callback_owner = NULL;
  L->callback_active = 0;
  L->operation = NULL;
  L->task_owner = NULL;
  L->owned_operations = L->resume_active = L->close_active = 0;
  L->state_poll = NULL;
  L->state_poll_context = NULL;
  L->poll_remaining = 1024;
  L->native_depth = L->world_depth = 0;
  L->heap_waiting = 0;
  L->object_waiting = 0;
  L->root_waiting = 0;
  L->stack_peer = NULL;
  L->object_scopes = NULL;
  L->errorJmp = NULL;
  L->hook = NULL;
  L->hookmask = 0;
  L->basehookcount = 0;
  L->allowhook = 1;
  resethookcount(L);
  L->openupval = NULL;
  L->status = LUA_OK;
  L->errfunc = 0;
  L->oldpc = 0;
  L->base_ci.previous = L->base_ci.next = NULL;
  return luaX_initgate(&L->stack_gate);
}


lu_mem luaE_threadsize (lua_State *L) {
  lu_mem sz = cast(lu_mem, sizeof(LX))
            + cast_uint(L->nci) * sizeof(CallInfo);
  if (L->stack.p != NULL)
    sz += cast_uint(stacksize(L) + EXTRA_STACK) * sizeof(StackValue);
  return sz;
}


static void close_state (lua_State *L, int owns_gate) {
  global_State *g = G(L);
  int i;
  if (owns_gate)
    luaW_stop(L);
  else
    luaC_publish(L);  /* exclusive cleanup of a partially constructed VM */
  if (!completestate(g))  /* closing a partially built state? */
    luaC_freeallobjects(L);  /* just collect its objects */
  else {  /* closing a fully built state */
    LuaWorldScope depth;
    resetCI(L);
    depth = luaW_suspend(L);
    luaD_closeprotected(L, 1, LUA_OK);  /* close all upvalues */
    luaW_restore(L, depth, L->native_depth);
    L->top.p = L->stack.p + 1;  /* empty the stack to run finalizers */
    luaC_freeallobjects(L);  /* collect all objects */
    luai_userstateclose(L);
  }
  for (i = 0; i < LUA_STRING_SHARDS; i++)
    luaM_freearray(L, g->strt[i].hash, cast_sizet(g->strt[i].size));
  freestack(L, L);
  luaCP_bind(L, NULL);
  luaM_reclaimpages(L);
  lua_assert(gettotalbytes(g) == sizeof(global_State) && g->GCretired == 0);
  if (owns_gate) {
    luaW_start(L);
    lua_unlock(L);
  }
  luaX_destroygate(&L->stack_gate);
  luaW_unregister(L);
  luaW_destroy(&g->world);
  (*g->frealloc)(g->ud, g, sizeof(global_State), 0);  /* free main block */
}


static void buildthread (lua_State *L, void *context) {
  lua_State *thread = cast(lua_State *, context);
  luaW_register(thread);
  luaCP_bind(thread, L->policy);
  lua_sethook(thread, gethook(L), gethookmask(L), getbasehookcount(L));
  memcpy(lua_getextraspace(thread), lua_getextraspace(mainthread(G(L))),
         LUA_EXTRASPACE);
  luai_userstatethread(L, thread);
  stack_init(thread, L);
}

LUA_API lua_State *lua_newthread (lua_State *L) {
  global_State *g = G(L);
  GCObject *o;
  lua_State *L1;
  int reservation;
  TStatus status;
  lua_lock(L);
  luaC_checkGC(L);
  /* create new thread */
  o = luaC_newobjdt(L, LUA_TTHREAD, sizeof(LX), offsetof(LX, l));
  L1 = gco2th(o);
  /* anchor it on L stack */
  setthvalue2s(L, L->top.p, L1);
  api_incr_top(L);
  if (preinit_thread(L1, g) != 0)
    luaM_error(L);
  /* No safepoint has exposed this fresh gate. Retain it while stack allocation
  ** can park the parent, so a concurrent scanner cannot read partial bounds.
  ** The parent supplies participation; the child must not add an active entry
  ** which would prevent collection while its allocator is waiting. */
  reservation = luaX_tryreserve(L1);
  lua_assert(reservation == 1);
  status = luaD_rawrunprotected(L, buildthread, L1);
  luaX_endreserve(L1, reservation);
  if (status != LUA_OK) luaD_throw(L, status);
  lua_unlock(L);
  return L1;
}


void luaE_retirethread (lua_State *L, lua_State *thread) {
  CallInfo *ci;
  for (ci = thread->base_ci.next; ci != NULL; ci = ci->next)
    luaM_retire_(L, ci, sizeof(*ci));
  if (thread->stack.p != NULL)
    luaM_retirearray(L, thread->stack.p,
                     cast_sizet(stacksize(thread) + EXTRA_STACK));
  luaM_retire_(L, fromstate(thread), sizeof(LX));
}

void luaE_freethread (lua_State *L, lua_State *L1) {
  LX *l = fromstate(L1);
  lua_assert(L1->native_depth == 0 && L1->world_depth == 0);
  luaF_closeupval(L1, L1->stack.p);  /* close all upvalues */
  lua_assert(L1->openupval == NULL);
  luai_userstatefree(L, L1);
  freestack(L, L1);
  {
    LuaPolicy *policy = L1->policy;
    L1->policy = NULL;
    luaCP_release(L, policy);
  }
  luaX_destroygate(&L1->stack_gate);
  luaW_unregister(L1);
  luaM_free(L, l);
}


TStatus luaE_resetthread (lua_State *L, TStatus status) {
  resetCI(L);
  if (status == LUA_YIELD)
    status = LUA_OK;
  status = luaD_closeprotected(L, 1, status);
  if (status != LUA_OK)  /* errors? */
    luaD_seterrorobj(L, status, L->stack.p + 1);
  else
    L->top.p = L->stack.p + 1;
  luaD_reallocstack(L, cast_int(L->ci->top.p - L->stack.p), 0);
  return status;
}


LUA_API int lua_closethread (lua_State *L, lua_State *from) {
  TStatus status;
  int crossing;
  l_uint32 calls = luaX_parentcalls(from);
  lua_lock(L);
  if (L->callback_active != 0) {
    setsvalue2s(L, L->top.p++, L->callback_owner->close_error);
    if (L->ci->top.p < L->top.p)
      L->ci->top.p = L->top.p;
    lua_unlock(L);
    return LUA_ERRRUN;
  }
  crossing = L == from && !luaD_canselfclose(L);
  luaX_checksync(pthread_mutex_lock(&G(L)->world.mutex));
  if (L->owned_operations != 0 || (L->close_active != 0 && L != from) ||
      crossing) {
    luaX_checksync(pthread_mutex_unlock(&G(L)->world.mutex));
    setsvalue2s(L, L->top.p++, G(L)->operation_error);
    if (L->ci->top.p < L->top.p) L->ci->top.p = L->top.p;
    lua_unlock(L);
    return LUA_ERRRUN;
  }
  luaX_checksync(pthread_mutex_unlock(&G(L)->world.mutex));
  L->close_active++;
  L->nCcalls = calls;
  status = luaE_resetthread(L, L->status);
  L->close_active--;
  if (L == from)  /* closing itself? */
    luaD_throwbaselevel(L, status);
  lua_unlock(L);
  return APIstatus(status);
}


LUA_API lua_State *lua_newstate (lua_Alloc f, void *ud, unsigned seed) {
  int i;
  lua_State *L;
  global_State *g = cast(global_State*,
                       (*f)(ud, NULL, LUA_TTHREAD, sizeof(global_State)));
  if (g == NULL) return NULL;
  L = &g->mainth.l;
  L->tt = LUA_VTHREAD;
  g->currentwhite = bitmask(WHITE0BIT);
  L->marked = luaC_white(g);
  if (preinit_thread(L, g) != 0) {
    (*f)(ud, g, sizeof(global_State), 0);
    return NULL;
  }
  if (luaW_init(&g->world) != 0) {
    luaX_destroygate(&L->stack_gate);
    (*f)(ud, g, sizeof(global_State), 0);
    return NULL;
  }
  luaW_register(L);
  g->allgc = obj2gco(L);  /* by now, only object is the main thread */
  L->next = NULL;
  incnny(L);  /* main thread is always non yieldable */
  g->frealloc = f;
  g->allocation_pages = NULL;
  g->scheduler = NULL;
  g->policy_factory = NULL;
  g->policy_context = NULL;
  g->slot_caches = NULL;
  for (i = 0; i < LUA_ALLOC_POOLS; i++) {
    g->allocation_available[i] = NULL;
    g->allocation_reserving[i] = 0;
  }
  g->ud = ud;
  g->warnf = NULL;
  g->ud_warn = NULL;
  g->seed = seed;
  g->gcstp = GCSTPGC;  /* no GC while building state */
  for (i = 0; i < LUA_STRING_SHARDS; i++) {
    g->strt[i].size = g->strt[i].nuse = 0;
    g->strt[i].hash = NULL;
    luaA_initlock(&g->strt[i].access);
  }
  setnilvalue(&g->l_registry);
  g->panic = NULL;
  g->managed_hook = NULL;
  g->callbacks = NULL;
  g->pipe_meta = NULL;
  for (i = 0; i < LUA_MANAGED_SHARDS; i++) {
    g->managed_functions[i] = NULL;
    luaA_initlock(&g->managed_access[i]);
  }
  luaA_initlock(&g->registry_access);
  luaA_initlock(&g->warning_access);
  luaA_initlock(&g->panic_access);
  luaA_initlock(&g->callbacks_access);
  g->coroutine_scopes = NULL;
  g->tasks = NULL;
  g->task_admission_closed = 0;
  g->operation_error = NULL;
  g->gcstate = GCSpause;
  g->gckind = KGC_INC;
  g->gcstopem = 0;
  g->gcemergency = 0;
  g->finobj = g->tobefnz = g->fixedgc = NULL;
  g->firstold1 = g->survival = g->old1 = g->reallyold = NULL;
  g->finobjsur = g->finobjold1 = g->finobjrold = NULL;
  g->sweepgc = NULL;
  g->gray = g->grayagain = NULL;
  g->weak = g->ephemeron = g->allweak = NULL;
  g->GCtotalbytes = sizeof(global_State);
  g->GCmarked = 0;
  g->GCdebt = 0;
  g->GCreclaimed = 0;
  g->GCretired = 0;
  g->GCmajorlive = sizeof(global_State);
  g->GCgoal = sizeof(global_State);
  g->GCcyclelive = sizeof(global_State);
  g->GCmarkdebt = 0;
  g->gcstep_pending = 0;
  g->trace_active = g->trace_epoch = g->trace_busy = 0;
  g->trace_weakcutoff = 0;
  memset(&g->trace_plan, 0, sizeof(g->trace_plan));
  g->trace_interrupt = 0;
  g->trace_changes = 0;
  g->trace_ready = 0;
  g->trace_deferred = NULL;
  setivalue(&g->nilvalue, 0);  /* to signal that state is not yet built */
  setgcparam(g, PAUSE, LUAI_GCPAUSE);
  setgcparam(g, STEPMUL, LUAI_GCMUL);
  setgcparam(g, STEPSIZE, LUAI_GCSTEPSIZE);
  setgcparam(g, MINORMUL, LUAI_GENMINORMUL);
  setgcparam(g, MINORMAJOR, LUAI_MINORMAJOR);
  setgcparam(g, MAJORMINOR, LUAI_MAJORMINOR);
  for (i=0; i < LUA_NUMTYPES; i++) {
    g->mt[i] = NULL;
    luaA_initlock(&g->metatable_access[i]);
  }
  if (luaD_rawrunprotected(L, f_luaopen, NULL) != LUA_OK) {
    /* memory allocation error: free partial state */
    close_state(L, 0);
    L = NULL;
  }
  return L;
}


LUA_API void lua_close (lua_State *L) {
  L = mainthread(G(L));  /* only the main thread can be closed */
  lua_lock(L);
  lua_assert(G(L)->scheduler == NULL);  /* retire carriers before the VM */
  close_state(L, 1);
}


void luaE_warning (lua_State *L, const char *msg, int tocont) {
  lua_WarnFunction wf;
  void *context;
  LuaObjectScope access;
  luaA_beginlock(L, &G(L)->warning_access, &access);
  wf = G(L)->warnf;
  context = G(L)->ud_warn;
  luaA_end(L, &access);
  if (wf != NULL) {
    LuaWorldScope depth;
    luaQ_beginnative(L);
    depth = luaW_detach(L);
    wf(context, msg, tocont);
    luaW_attach(L, depth);
    luaQ_endnative(L);
  }
}


/*
** Generate a warning from an error message
*/
void luaE_warnerror (lua_State *L, const char *where) {
  TValue *errobj = s2v(L->top.p - 1);  /* error object */
  const char *msg = (ttisstring(errobj))
                  ? getstr(tsvalue(errobj))
                  : "error object is not a string";
  /* produce warning "error in %s (%s)" (where, msg) */
  luaE_warning(L, "error in ", 1);
  luaE_warning(L, where, 1);
  luaE_warning(L, " (", 1);
  luaE_warning(L, msg, 1);
  luaE_warning(L, ")", 0);
}

