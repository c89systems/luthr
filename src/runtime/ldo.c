/*
** $Id: ldo.c $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/

#define ldo_c
#define LUA_CORE

#include "lprefix.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "lbuiltin.h"
#include "lcall.h"
#include "laccess.h"
#include "lroots.h"
#include "lcoroscope.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstackapi.h"
#include "lstring.h"
#include "ltable.h"
#include "ltableaccess.h"
#include "ltm.h"
#include "ltask.h"
#include "lundump.h"
#include "lvm.h"
#include "lzio.h"



#define errorstatus(s)	((s) > LUA_YIELD)


/*
** these macros allow user-specific actions when a thread is
** resumed/yielded.
*/
#if !defined(luai_userstateresume)
#define luai_userstateresume(L,n)	((void)L)
#endif

#if !defined(luai_userstateyield)
#define luai_userstateyield(L,n)	((void)L)
#endif


/*
** {======================================================
** Error-recovery functions
** =======================================================
*/

/* chained list of long jump buffers */
typedef struct lua_longjmp {
  struct lua_longjmp *previous;
  struct lua_longjmp *execution_previous;
  NativeStackContext *context;
  LuaExecution *execution;
  unsigned execution_native_depth;
  LuaCallScope *execution_trusted;
  LuaGCFrame *gc_frame;
  volatile pthread_t carrier;
  intptr_t b[5];  /* compiler-owned stack context; no libc thread encoding */
  volatile TStatus status;  /* error code */
  lu_byte luthr_stack_overflow;
  LuaNativeRoot *native_roots;
  unsigned native_depth;
  unsigned reclaiming;
  LuaWorldScope world_scope;
  LuaObjectScope *object_scopes;
  LuaCoroutineScope *coroutine_scopes;
  LuaApiRegion *api_region;
} lua_longjmp;

/* Only this logical execution owns these retained native stack locations.
** A state encountered by a nested API call can belong to another execution. */
void luaD_rebindexecution (LuaExecution *execution) {
  lua_longjmp *boundary;
  pthread_t carrier = pthread_self();
  lua_assert(execution != NULL && execution->native_depth == 0 &&
             execution->managed_depth == 0);
  for (boundary = execution->protected; boundary != NULL;
       boundary = boundary->execution_previous)
    boundary->carrier = carrier;
}


/*
** LUAI_THROW/LUAI_TRY define how Lua does exception handling.
** C++ source builds use exceptions. The supported C build uses the pinned
** compiler's nonlocal-goto mechanism, keeping its recovery context on the
** retained runtime stack rather than using a libc thread-specific jump buffer.
*/
#if !defined(LUAI_THROW)				/* { */

#if defined(__cplusplus) && !defined(LUA_USE_LONGJMP)	/* { */

/* C++ exceptions */
#define LUAI_THROW(L,c)		throw(c)

static void LUAI_TRY (lua_State *L, lua_longjmp *c, Pfunc f, void *ud) {
  try {
    f(L, ud);  /* call function protected */
  }
  catch (lua_longjmp *c1) { /* Lua error */
    if (c1 != c)  /* not the correct level? */
      throw;  /* rethrow to upper level */
  }
  catch (...) {  /* non-Lua exception */
    c->status = -1;  /* create some error code */
  }
}


#else							/* }{ */

/* The throw lives in a function called below the matching protected entry. */
#define LUAI_THROW(L,c) __builtin_longjmp(cast_voidp((c)->b), 1)
#define LUAI_TRY(L,c,f,ud) \
  if (__builtin_setjmp(cast_voidp((c)->b)) == 0) ((f)(L, ud))

#endif							/* } */

#endif							/* } */


void luaD_seterrorobj (lua_State *L, TStatus errcode, StkId oldtop) {
  if (errcode == LUA_ERRMEM) {  /* memory error? */
    setsvalue2s(L, oldtop, G(L)->memerrmsg); /* reuse preregistered msg. */
  }
  else {
    lua_assert(errorstatus(errcode));  /* must be a real error */
    lua_assert(!ttisnil(s2v(L->top.p - 1)));  /* with a non-nil object */
    setobjs2s(L, oldtop, L->top.p - 1);  /* move it to 'oldtop' */
  }
  L->top.p = oldtop + 1;  /* top goes back to old top plus error object */
}


l_noret luaD_throw (lua_State *L, TStatus errcode) {
  if (L->errorJmp) {  /* thread has an error handler? */
    NativeStackContext *context = luaE_getnativestackcontext();
    if (L->errorJmp->context != context ||
        L->errorJmp->execution != context->execution ||
        !pthread_equal(L->errorJmp->carrier, pthread_self())) {
      fputs("Luthr error crossed a foreign carrier's protected boundary\n", stderr);
      abort();
    }
    luaO_unwind(L->errorJmp->coroutine_scopes);
    luaA_unwind(L, L->errorJmp->object_scopes);
    context->api_region = L->errorJmp->api_region;
    L->errorJmp->status = errcode;  /* set status */
    LUAI_THROW(L, L->errorJmp);  /* jump to it */
  }
  else {  /* thread has no error handler */
    global_State *g = G(L);
    lua_State *mainth = mainthread(g);
    lua_CFunction panicf;
    luaO_unwind(NULL);
    luaA_unwind(L, NULL);
    luaE_getnativestackcontext()->api_region = NULL;
    L->native_roots = NULL;  /* every native scope on this thread is abandoned */
    {
      LuaWorldScope empty = {0};
      luaW_restore(L, empty, 1);  /* retain this state while closing frames */
    }
    errcode = luaE_resetthread(L, errcode);  /* close all upvalues */
    L->status = errcode;
    if (L != mainth) {
      LuaNativeRoot error;
      LuaWorldScope source;
      luaR_pushroot(L, &error);
      luaR_setroot(L, &error, s2v(L->top.p - 1));
      source = luaW_detach(L);
      lua_lock(mainth);
      if (mainth->errorJmp != NULL &&
          mainth->errorJmp->context == luaX_boundcontext(mainth) &&
          mainth->errorJmp->execution == luaQ_current() &&
          pthread_equal(mainth->errorJmp->carrier, pthread_self())) {
        LuaWorldScope depth;
        setobj2s(mainth, mainth->top.p++, &error.value);
        depth = luaW_detach(mainth);
        lua_lock(L);
        lua_assert(L->native_depth == 2);
        luaR_poproot(L, &error);
        L->native_depth -= 2;  /* detach pin and error-cleanup pin */
        lua_unlock(L);  /* no more source-state accesses after this leave */
        luaW_attach(mainth, depth);
        luaD_throw(mainth, errcode);
      }
      lua_unlock(mainth);
      luaW_attach(L, source);
      luaR_poproot(L, &error);
    }
    {
      LuaObjectScope access;
      luaA_beginlock(L, &g->panic_access, &access);
      panicf = g->panic;
      luaA_end(L, &access);
    }
    lua_assert(L->native_depth == 1);
    L->native_depth--;  /* all abandoned runtime scopes retire before panic */
    if (panicf != NULL) {
      lua_unlock(L);
      panicf(L);  /* last chance to jump out on this carrier */
    }
    abort();
  }
}


/* A reentrant self-close must not jump over another state's live operation
** or return into a protected receiver owned by another execution/carrier. */
int luaD_canselfclose (lua_State *L) {
  NativeStackContext *context = luaE_getnativestackcontext();
  lua_longjmp *base = L->errorJmp;
  if (base == NULL) return context->coroutine_scopes == NULL;
  while (base->previous != NULL) base = base->previous;
  return base->context == context &&
         base->execution == context->execution &&
         pthread_equal(base->carrier, pthread_self()) &&
         base->coroutine_scopes == context->coroutine_scopes;
}


l_noret luaD_throwbaselevel (lua_State *L, TStatus errcode) {
  if (L->errorJmp) {
    /* unroll error entries up to the first level */
    while (L->errorJmp->previous != NULL)
      L->errorJmp = L->errorJmp->previous;
    luaO_unwind(L->errorJmp->coroutine_scopes);
    luaA_unwind(L, L->errorJmp->object_scopes);
    luaX_context(L)->overflow = L->errorJmp->luthr_stack_overflow;
    L->native_roots = L->errorJmp->native_roots;
    luaW_restore(L, L->errorJmp->world_scope, L->errorJmp->native_depth);
  }
  luaD_throw(L, errcode);
}


static TStatus runprotected (lua_State *L, Pfunc f, void *ud,
                              NativeStackContext *context) {
  l_uint32 oldnCcalls = L->nCcalls;
  lua_longjmp lj;
  TStatus status;
  lj.status = LUA_OK;
  lj.carrier = pthread_self();
  lj.context = context;
  lj.execution = context->execution;
  lj.execution_native_depth = lj.execution != NULL
                              ? lj.execution->native_depth : 0;
  lj.execution_trusted = lj.execution != NULL ? lj.execution->trusted : NULL;
  lj.execution_previous = lj.execution != NULL
                           ? lj.execution->protected : NULL;
  if (lj.execution != NULL)
    lj.execution->protected = &lj;
  lj.luthr_stack_overflow = context->overflow;
  lj.native_roots = L->native_roots;
  lj.native_depth = L->native_depth;
  lj.reclaiming = L->reclaiming;
  lj.gc_frame = luaC_gcframe();
  lj.world_scope = luaW_scope(L);
  lj.object_scopes = L->object_scopes;
  lj.coroutine_scopes = context->coroutine_scopes;
  lj.api_region = context->api_region;
  lj.previous = L->errorJmp;  /* chain new error handler */
  L->errorJmp = &lj;
  LUAI_TRY(L, &lj, f, ud);  /* call 'f' catching errors */
  luaC_restoregcframe(lj.gc_frame);
  context->api_region = lj.api_region;
  L->reclaiming = lj.reclaiming;
  /* Retire the jump buffer before any cleanup callback can park this stack. */
  if (lj.execution != NULL) {
    lj.execution->protected = lj.execution_previous;
    lj.execution->trusted = lj.execution_trusted;
  }
  /* A managed coroutine yield can keep running on this task. Release the
  ** cached permit on a scheduler park, an error, or an abandoned native frame.
  ** Self-close can abandon a native frame while reporting LUA_OK. */
  if (lj.execution != NULL &&
      (lj.execution->native_depth != lj.execution_native_depth ||
       (lj.status != LUA_OK &&
        (lj.status != LUA_YIELD || lj.execution->scheduler == NULL))))
    luaQ_restorenative(lj.execution, L, lj.execution_native_depth);
  if (lj.status != LUA_OK) {
    L->native_roots = lj.native_roots;  /* native scopes have unwound */
    luaW_restore(L, lj.world_scope, lj.native_depth);
  }
  L->errorJmp = lj.previous;  /* restore old error handler */
  L->nCcalls = oldnCcalls;
  status = lj.status;
  if (l_unlikely(status != LUA_OK))
    context->overflow = lj.luthr_stack_overflow;
  return status;
}

TStatus luaD_rawrunprotected (lua_State *L, Pfunc f, void *ud) {
  return runprotected(L, f, ud, luaX_context(L));
}

/* Retain outer operations when a nested receiver catches an error. */
void luaD_releaseoperations (lua_State *L) {
  luaO_release(L->errorJmp != NULL ? L->errorJmp->coroutine_scopes : NULL);
}

/* }====================================================== */


/*
** {==================================================================
** Stack reallocation
** ===================================================================
*/

/* some stack space for error handling */
#define STACKERRSPACE	200


/*
** LUAI_MAXSTACK limits the size of the Lua stack.
** It must fit into INT_MAX/2.
*/

#if !defined(LUAI_MAXSTACK)
#if 1000000 < (INT_MAX / 2)
#define LUAI_MAXSTACK           1000000
#else
#define LUAI_MAXSTACK           (INT_MAX / 2u)
#endif
#endif


/* maximum stack size that respects size_t */
#define MAXSTACK_BYSIZET  ((MAX_SIZET / sizeof(StackValue)) - STACKERRSPACE)

/*
** Minimum between LUAI_MAXSTACK and MAXSTACK_BYSIZET
** (Maximum size for the stack must respect size_t.)
*/
#define MAXSTACK	cast_int(LUAI_MAXSTACK < MAXSTACK_BYSIZET  \
			        ? LUAI_MAXSTACK : MAXSTACK_BYSIZET)


/* stack size with extra space for error handling */
#define ERRORSTACKSIZE	(MAXSTACK + STACKERRSPACE)


/* raise a stack error while running the message handler */
l_noret luaD_errerr (lua_State *L) {
  TString *msg = luaS_newliteral(L, "error in error handling");
  setsvalue2s(L, L->top.p, msg);
  L->top.p++;  /* assume EXTRA_STACK */
  luaD_throw(L, LUA_ERRERR);
}


/*
** Check whether stacks have enough space to run a simple function (such
** as a finalizer): At least BASIC_STACK_SIZE in the Lua stack, two
** available CallInfos, and two "slots" in the C stack.
*/
int luaD_checkminstack (lua_State *L) {
  if (luaD_nativeposition() <= luaD_nativelimit(luaX_context(L)))
    return 0;
  if (getCcalls(L) >= LUAI_MAXCCALLS - 2)
    return 0;  /* not enough C-stack slots */
  if (luaE_cireserve(L->ci) == NULL && luaE_extendCI(L, 0) == NULL)
    return 0;  /* unable to reserve both frames */
  if (L->stack_last.p - L->top.p >= BASIC_STACK_SIZE)
    return 1;  /* enough (BASIC_STACK_SIZE) free slots in the Lua stack */
  else  /* try to grow stack to a size with enough free slots */
    return luaD_growstack(L, BASIC_STACK_SIZE, 0);
}


/* The owner can park while preparing the replacement. Until it resumes,
** both mutators and collectors still see the original stack and frames.
** Retarget only under the coroutine gate, with no safepoint until every
** interior pointer names the new stack. Freeing the old storage can park. */
static int resizestack (lua_State *L, lua_State *owner, int newsize,
                        int raiseerror) {
  StkId oldstack, newstack;
  CallInfo *ci;
  int oldsize, copied, i;
  lua_assert(G(L) == G(owner));
  lua_assert(newsize <= MAXSTACK || newsize == ERRORSTACKSIZE);
#if !defined(HARDSTACKTESTS)
  if (newsize == stacksize(L)) return 1;
#endif
  newstack = cast(StkId, luaM_allocstack(owner,
    (cast_sizet(newsize) + EXTRA_STACK) * sizeof(StackValue), raiseerror));
  if (l_unlikely(newstack == NULL)) {
    if (raiseerror) luaM_error(L);
    return 0;
  }
  /* A trace may have started while preparing the replacement. Publish the
  ** live stack before replacing its storage. */
  if (L == owner) luaC_checksnapshot(L);
  else if (luaC_scanready(G(owner))) luaC_snapshot(owner, L);
  oldstack = L->stack.p;
  oldsize = stacksize(L);
  copied = (oldsize < newsize ? oldsize : newsize) + EXTRA_STACK;
  memcpy(newstack, oldstack, cast_sizet(copied) * sizeof(StackValue));
  for (i = copied; i < newsize + EXTRA_STACK; i++)
    setnilvalue2s(newstack + i);
  L->stack.p = newstack;
  L->stack_last.p = newstack + newsize;
  L->top.p = newstack + (L->top.p - oldstack);
  L->tbclist.p = newstack + (L->tbclist.p - oldstack);
  for (ci = L->ci; ci != NULL; ci = ci->previous) {
    ci->top.p = newstack + (ci->top.p - oldstack);
    ci->func.p = newstack + (ci->func.p - oldstack);
    if (isLua(ci))
      ci->u.l.trap = 1;  /* refresh cached stack pointers in the VM */
  }
  if (newsize > oldsize) L->gc_resize = 1;
  luaM_freearray(owner, oldstack, cast_sizet(oldsize) + EXTRA_STACK);
  return 1;
}

int luaD_reallocstack (lua_State *L, int newsize, int raiseerror) {
  return resizestack(L, L, newsize, raiseerror);
}


/*
** Try to grow the stack by at least 'n' elements. When 'raiseerror'
** is true, raises any error; otherwise, return 0 in case of errors.
*/
int luaD_growstack (lua_State *L, int n, int raiseerror) {
  int size = stacksize(L);
  if (l_unlikely(size > MAXSTACK)) {
    /* if stack is larger than maximum, thread is already using the
       extra space reserved for errors, that is, thread is handling
       a stack error; cannot grow further than that. */
    lua_assert(stacksize(L) == ERRORSTACKSIZE);
    if (raiseerror)
      luaD_errerr(L);  /* stack error inside message handler */
    return 0;  /* if not 'raiseerror', just signal it */
  }
  else if (n < MAXSTACK) {  /* avoids arithmetic overflows */
    int newsize = size + (size >> 1);  /* tentative new size (size * 1.5) */
    int needed = cast_int(L->top.p - L->stack.p) + n;
    if (newsize > MAXSTACK)  /* cannot cross the limit */
      newsize = MAXSTACK;
    if (newsize < needed)  /* but must respect what was asked for */
      newsize = needed;
    if (l_likely(newsize <= MAXSTACK))
      return luaD_reallocstack(L, newsize, raiseerror);
  }
  /* else stack overflow */
  /* add extra size to be able to handle the error message */
  luaD_reallocstack(L, ERRORSTACKSIZE, raiseerror);
  if (raiseerror)
    luaG_runerror(L, "stack overflow");
  return 0;
}


/*
** Compute how much of the stack is being used, by computing the
** maximum top of all call frames in the stack and the current top.
*/
static int stackinuse (lua_State *L) {
  CallInfo *ci;
  int res;
  StkId lim = L->top.p;
  for (ci = L->ci; ci != NULL; ci = ci->previous) {
    if (lim < ci->top.p) lim = ci->top.p;
  }
  lua_assert(lim <= L->stack_last.p + EXTRA_STACK);
  res = cast_int(lim - L->stack.p) + 1;  /* part of stack in use */
  if (res < LUA_MINSTACK)
    res = LUA_MINSTACK;  /* ensure a minimum size */
  return res;
}


/*
** If stack size is more than 3 times the current use, reduce that size
** to twice the current use. (So, the final stack size is at most 2/3 the
** previous size, and half of its entries are empty.)
** As a particular case, if stack was handling a stack overflow and now
** it is not, 'max' (limited by MAXSTACK) will be smaller than
** stacksize (equal to ERRORSTACKSIZE in this case), and so the stack
** will be reduced to a "regular" size.
*/
static int stacktrimsize (lua_State *L) {
  int inuse, max;
  int capacity = stacksize(L);
  if (capacity <= (LUA_MINSTACK > MAXSTACK / 3 ? MAXSTACK : 3 * LUA_MINSTACK))
    return 0;
  inuse = stackinuse(L);
  max = (inuse > MAXSTACK / 3) ? MAXSTACK : inuse * 3;
  /* if thread is currently not handling a stack overflow and its
     size is larger than maximum "reasonable" size, shrink it */
  if (inuse <= MAXSTACK && capacity > max)
    return (inuse > MAXSTACK / 2) ? MAXSTACK : inuse * 2;
  return 0;
}

int luaD_cantrimstack (lua_State *L) {
#if defined(HARDSTACKTESTS)
  UNUSED(L);
  return 1;  /* retain forced relocations in stack stress builds */
#else
  return luaE_cantrimCI(L) || stacktrimsize(L) != 0;
#endif
}

void luaD_shrinkstack (lua_State *L, lua_State *owner) {
  int nsize = stacktrimsize(L);
#if defined(HARDSTACKTESTS)
  if (nsize == 0) nsize = stacksize(L);
#endif
  if (nsize != 0)
    resizestack(L, owner, nsize, 0);
  luaE_shrinkCI(L, owner);
}

/* }================================================================== */


/*
** Call a hook for the given event. Make sure there is a hook to be
** called. (Both the hook pointer and mask, which trigger this
** function, can be changed asynchronously by signals.)
*/
void luaD_hook (lua_State *L, int event, int line,
                              int ftransfer, int ntransfer) {
  lua_Hook hook = gethook(L);
  if (hook && L->allowhook) {  /* make sure there is a hook */
    int foreign = !luaQ_ismanagedhook(L, hook);
    LuaCallScope call;
    CallInfo *ci = L->ci;
    ptrdiff_t top = savestack(L, L->top.p);  /* preserve original 'top' */
    ptrdiff_t ci_top = savestack(L, ci->top.p);  /* idem for 'ci->top' */
    lua_Debug ar;
    luaQ_begincall(L, !foreign, &call);
    ar.event = event;
    ar.currentline = line;
    ar.i_ci = ci;
    L->transferinfo.ftransfer = ftransfer;
    L->transferinfo.ntransfer = ntransfer;
    if (isLua(ci) && L->top.p < ci->top.p)
      L->top.p = ci->top.p;  /* protect entire activation register */
    luaD_checkstack(L, LUA_MINSTACK);  /* ensure minimum stack size */
    if (ci->top.p < L->top.p + LUA_MINSTACK)
      ci->top.p = L->top.p + LUA_MINSTACK;
    L->allowhook = 0;  /* cannot call hooks inside a hook */
    ci->callstatus |= CIST_HOOKED;
    luaQ_entercall(L, &call);
    (*hook)(L, &ar);
    luaQ_leavecall(L, &call);
    lua_assert(!L->allowhook);
    L->allowhook = 1;
    ci->top.p = restorestack(L, ci_top);
    L->top.p = restorestack(L, top);
    ci->callstatus &= ~CIST_HOOKED;
  }
}


/*
** Executes a call hook for Lua functions. This function is called
** whenever 'hookmask' is not zero, so it checks whether call hooks are
** active.
*/
void luaD_hookcall (lua_State *L, CallInfo *ci) {
  L->oldpc = 0;  /* set 'oldpc' for new function */
  if (gethookmask(L) & LUA_MASKCALL) {  /* is call hook on? */
    int event = (ci->callstatus & CIST_TAIL) ? LUA_HOOKTAILCALL
                                             : LUA_HOOKCALL;
    Proto *p = ci_func(ci)->p;
    ci->u.l.savedpc++;  /* hooks assume 'pc' is already incremented */
    luaD_hook(L, event, -1, 1, p->numparams);
    ci->u.l.savedpc--;  /* correct 'pc' */
  }
}


/*
** Executes a return hook for Lua and C functions and sets/corrects
** 'oldpc'. (Note that this correction is needed by the line hook, so it
** is done even when return hooks are off.)
*/
static void rethook (lua_State *L, CallInfo *ci, int nres) {
  if (gethookmask(L) & LUA_MASKRET) {  /* is return hook on? */
    StkId firstres = L->top.p - nres;  /* index of first result */
    int delta = 0;  /* correction for vararg functions */
    int ftransfer;
    if (isLua(ci)) {
      Proto *p = ci_func(ci)->p;
      if (p->flag & PF_VAHID)
        delta = ci->u.l.nextraargs + p->numparams + 1;
    }
    ci->func.p += delta;  /* if vararg, back to virtual 'func' */
    ftransfer = cast_int(firstres - ci->func.p);
    luaD_hook(L, LUA_HOOKRET, -1, ftransfer, nres);  /* call it */
    ci->func.p -= delta;
  }
  if (isLua(ci = ci->previous))
    L->oldpc = pcRel(ci->u.l.savedpc, ci_func(ci)->p);  /* set 'oldpc' */
}


/*
** Check whether 'func' has a '__call' metafield. If so, put it in the
** stack, below original 'func', so that 'luaD_precall' can call it.
** Raise an error if there is no '__call' metafield.
** Bits CIST_CCMT in status count how many _call metamethods were
** invoked and how many corresponding extra arguments were pushed.
** (This count will be saved in the 'callstatus' of the call).
**  Raise an error if this counter overflows.
*/
static unsigned tryfuncTM (lua_State *L, StkId func, unsigned status) {
  LuaNativeRoot method;
  StkId p;
  luaR_pushroot(L, &method);
  luaT_gettmbyobj(L, s2v(func), TM_CALL, &method);
  if (l_unlikely(ttisnil(&method.value)))  /* no metamethod? */
    luaG_callerror(L, s2v(func));
  for (p = L->top.p; p > func; p--)  /* open space for metamethod */
    setobjs2s(L, p, p-1);
  L->top.p++;  /* stack space pre-allocated by the caller */
  setobj2s(L, func, &method.value);  /* snapshot is the new function */
  luaR_poproot(L, &method);
  if ((status & MAX_CCMT) == MAX_CCMT)  /* is counter full? */
    luaG_runerror(L, "'__call' chain too long");
  return status + (1u << CIST_CCMT);  /* increment counter */
}


/* Generic case for 'moveresult' */
l_sinline void genmoveresults (lua_State *L, StkId res, int nres,
                                             int wanted) {
  StkId firstresult = L->top.p - nres;  /* index of first result */
  int i;
  if (nres > wanted)  /* extra results? */
    nres = wanted;  /* don't need them */
  for (i = 0; i < nres; i++)  /* move all results to correct place */
    setobjs2s(L, res + i, firstresult + i);
  for (; i < wanted; i++)  /* complete wanted number of results */
    setnilvalue2s(res + i);
  L->top.p = res + wanted;  /* top points after the last result */
}


/*
** Given 'nres' results at 'firstResult', move 'fwanted-1' of them
** to 'res'.  Handle most typical cases (zero results for commands,
** one result for expressions, multiple results for tail calls/single
** parameters) separated. The flag CIST_TBC in 'fwanted', if set,
** forces the switch to go to the default case.
*/
l_sinline void moveresults (lua_State *L, StkId res, int nres,
                                          l_uint32 fwanted) {
  switch (fwanted) {  /* handle typical cases separately */
    case 0 + 1:  /* no values needed */
      L->top.p = res;
      return;
    case 1 + 1:  /* one value needed */
      if (nres == 0)   /* no results? */
        setnilvalue2s(res);  /* adjust with nil */
      else  /* at least one result */
        setobjs2s(L, res, L->top.p - nres);  /* move it to proper place */
      L->top.p = res + 1;
      return;
    case LUA_MULTRET + 1:
      genmoveresults(L, res, nres, nres);  /* we want all results */
      break;
    default: {  /* two/more results and/or to-be-closed variables */
      int wanted = get_nresults(fwanted);
      if (fwanted & CIST_TBC) {  /* to-be-closed variables? */
        L->ci->u2.nres = nres;
        L->ci->callstatus |= CIST_CLSRET;  /* in case of yields */
        res = luaF_close(L, res, CLOSEKTOP, 1);
        L->ci->callstatus &= ~CIST_CLSRET;
        if (gethookmask(L)) {  /* if needed, call hook after '__close's */
          ptrdiff_t savedres = savestack(L, res);
          rethook(L, L->ci, nres);
          res = restorestack(L, savedres);  /* hook can move stack */
        }
        if (wanted == LUA_MULTRET)
          wanted = nres;  /* we want all results */
      }
      genmoveresults(L, res, nres, wanted);
      break;
    }
  }
}


l_sinline void checkreturn (lua_State *L) {
  if (l_unlikely(luaTK_hasstops(L))) luaTK_deliverstop(L);
  if (l_unlikely(L->policy != NULL)) luaCP_checklimit(L);
}

/*
** Finishes a function call: calls hook if necessary, moves current
** number of results to proper place, and returns to previous call
** info. If function has to close variables, hook must be called after
** that.
*/
void luaD_poscall (lua_State *L, CallInfo *ci, int nres) {
  l_uint32 fwanted = ci->callstatus & (CIST_TBC | CIST_NRESULTS);
  if (l_unlikely(gethookmask(L)) && !(fwanted & CIST_TBC))
    rethook(L, ci, nres);
  /* move results to proper place */
  moveresults(L, ci->func.p, nres, fwanted);
  if (L->openupval != NULL)
    luaF_storelocals(L, ci->func.p, cast_int(L->top.p - ci->func.p));
  /* function cannot be in any of these cases when returning */
  lua_assert(!(ci->callstatus &
        (CIST_HOOKED | CIST_YPCALL | CIST_FIN | CIST_CLSRET)));
  L->ci = ci->previous;  /* back to caller (after closing variables) */
  checkreturn(L);
}



#define next_ci(L)  (L->ci->next ? L->ci->next : luaE_extendCI(L, 1))


/*
** Allocate and initialize CallInfo structure. At this point, the
** only valid fields in the call status are number of results,
** CIST_C (if it's a C function), and number of extra arguments.
** (All these bit-fields fit in 16-bit values.)
*/
l_sinline CallInfo *prepCallInfo (lua_State *L, StkId func, unsigned status,
                                                StkId top) {
  return luaD_initframe(L, next_ci(L), func, status, top);
}


l_sinline int managedCfunction (const TValue *function) {
  return ttypetag(function) == LUA_VMCF ||
         (ttisCclosure(function) && clCvalue(function)->managed);
}

static int __attribute__((noinline)) callCwithscope (lua_State *L, StkId func,
                         unsigned status, lua_CFunction f, int managed) {
  int n;  /* number of returns */
  LuaCallScope call;
  CallInfo *ci;
  ptrdiff_t function;
  function = savestack(L, func);
  luaQ_begincall(L, managed, &call);
  func = restorestack(L, function);
  checkstackp(L, LUA_MINSTACK, func);  /* ensure minimum stack size */
  L->ci = ci = prepCallInfo(L, func, status | CIST_C,
                               L->top.p + LUA_MINSTACK);
  lua_assert(ci->top.p <= L->stack_last.p);
  if (l_unlikely(gethookmask(L) & LUA_MASKCALL)) {
    int narg = cast_int(L->top.p - func) - 1;
    luaD_hook(L, LUA_HOOKCALL, -1, 1, narg);
  }
  luaQ_entercall(L, &call);
  n = (*f)(L);  /* do the actual call */
  luaQ_leavecall(L, &call);
  lua_assert(L->ci == ci);
  UNUSED(ci);
  return n;
}

/* The builtin yield has no native body to unwind. Keep its CallInfo so
** resumed arguments, debug inspection and close see the usual Lua frame. */
#if !defined(HARDSTACKTESTS)
static int trysuspend (lua_State *L, StkId func, unsigned status) {
  CallInfo *ci = L->ci->next;
  LuaExecution *execution;
  if (ci == NULL || L->stack_last.p - L->top.p <= LUA_MINSTACK ||
      !yieldable(L) || !L->resume_active || gethookmask(L) || luaW_ownsheap(L))
    return 0;
  /* A host-driven execution can observe the protected unwind on yield. */
  execution = luaX_execution(L);
  if (execution != NULL && execution->native_restore != NULL) return 0;
  luaD_initframe(L, ci, func, status | CIST_C, L->top.p + LUA_MINSTACK);
  ci->u.c.k = NULL;
  ci->u2.nyield = cast_int(L->top.p - (func + 1));
  luai_userstateyield(L, ci->u2.nyield);
  L->status = LUA_YIELD;
  return 1;
}
#endif

/* Opaque and managed C frames share the same ABI and result completion. */
int luaD_precallC (lua_State *L, StkId func,
                     unsigned status, lua_CFunction f, int managed) {
  CallInfo *ci;
  int n;
#if !defined(HARDSTACKTESTS)
  NativeStackContext *context = managed ? NULL : luaW_trynativeframe(L, func, status);
  if (context != NULL) {
    n = (*f)(L);
    luaW_finishnativeframe(L, context);
  }
  else
#endif
    n = callCwithscope(L, func, status, f, managed);
  api_checknelems(L, n);
  ci = L->ci;
  /* The frame can have moved or acquired cleanup during the call. Use the
  ** live frame after reentry, with the full return path for hooks and captures. */
  if (ci->callstatus == (CIST_C | (1 + 1)) && L->openupval == NULL &&
      gethookmask(L) == 0) {
    moveresults(L, ci->func.p, n, 1 + 1);
    L->ci = ci->previous;
    checkreturn(L);
  }
  else luaD_poscall(L, ci, n);
  /* A callback can consume the poll that first requested nursery work. */
  if (l_unlikely(L->task_nursery_pending) && L->native_depth == 0 && isLua(L->ci))
    L->poll_remaining = 1;
  return n;
}

/* A successful probe returns up to two computed values, or forwards a
** suffix of the arguments. It never rewrites a live slot or parks. */
static int leafresult (lua_State *L, StkId func, unsigned builtin, int nargs,
                       TValue *value, int *forward) {
  const TValue *first = s2v(func + 1);
  switch (builtin) {
  case LUA_LEAF_ASSERT:
    if (l_isfalse(first)) return -1;
    *forward = 1;
    return nargs;
  case LUA_LEAF_TYPE:
    setsvalue(L, value, G(L)->type_names[ttype(first)]);
    return 1;
  case LUA_LEAF_STRBYTE: case LUA_LEAF_STRSUB:
    return luaB_leafrange(L, func, nargs, builtin == LUA_LEAF_STRBYTE, value);
  case LUA_LEAF_NEXT: {
    int more;
    if (!ttistable(first)) return -1;
    if (nargs < 2) setnilvalue(value);
    else { setobj(L, value, s2v(func + 2)); }
    more = luaH_trynext(L, hvalue(first), value, value + 1);
    if (more < 0) return -1;
    if (!more) setnilvalue(value);
    return more + 1;
  }
  case LUA_LEAF_SELECT: {
    lua_Integer index;
    if (ttisstring(first) && getstr(tsvalue(first))[0] == '#') {
      setivalue(value, nargs - 1);
      return 1;
    }
    if (!tointegerns(first, &index)) return -1;
    index = luaB_selectindex(index, nargs);
    if (index < 1) return -1;
    *forward = 1;
    return nargs - cast_int(index);
  }
  case LUA_LEAF_RAWGET: {
    lu_byte tag;
    if (nargs < 2 || !ttistable(first) ||
        !luaH_tryget(L, hvalue(first), s2v(func + 2), value, &tag)) return -1;
    if (tagisempty(tag)) setnilvalue(value);
    return 1;
  }
  case LUA_LEAF_RAWEQUAL:
    if (nargs < 2) return -1;
    if (luaV_rawequalobj(first, s2v(func + 2))) { setbtvalue(value); }
    else { setbfvalue(value); }
    return 1;
  case LUA_LEAF_TONUMBER:
    if (nargs >= 2 && !ttisnil(s2v(func + 2))) return -1;
    if (ttisnumber(first)) { setobj(L, value, first); }
    else if (ttisshrstring(first)) {
      TString *str = tsvalue(first);
      if (luaO_str2num(getshrstr(str), value) != cast_sizet(str->shrlen) + 1)
        setnilvalue(value);
    }
    else return -1;
    return 1;
  case LUA_LEAF_MATHABS:
    if (!ttisinteger(first)) return -1;
    setivalue(value, luaB_absinteger(ivalue(first)));
    return 1;
  case LUA_LEAF_UTF8LEN:
    return luaB_leafutf8len(func, nargs, value);
  case LUA_LEAF_STRLEN:
    if (!ttisstring(first)) return -1;
    setivalue(value, cast(lua_Integer, tsslen(tsvalue(first))));
    return 1;
  case LUA_LEAF_MATHTOINTEGER: {
    lua_Integer integer;
    if (ttisstring(first)) return -1;  /* coercion uses the original frame */
    if (tointegerns(first, &integer)) {
      setivalue(value, integer);
    }
    else {
#if defined(LUA_FAILISFALSE)
      setbfvalue(value);
#else
      setnilvalue(value);
#endif
    }
    return 1;
  }
  case LUA_LEAF_STRFIND: case LUA_LEAF_STRMATCH:
    return luaB_leafmatch(L, func, nargs, builtin == LUA_LEAF_STRFIND, value);
  case LUA_LEAF_YIELD:
    return -1;  /* only a VM frame can suspend without a native call */
  default:
    lua_assert(0);
    return -1;
  }
}

/* An eligible builtin finishes in its caller's frame. Its caller delivers
** pending errors after publishing that frame. */
int luaD_tryleaf (lua_State *L, StkId func, int nargs, int wanted) {
  int n, forward = 0;
  TValue values[LUA_LEAF_RESULTS];
  if (nargs < 1 || !isLua(L->ci) || gethookmask(L) ||
      (wanted > 1 && wanted > L->ci->top.p - func) ||
      (L->openupval != NULL && L->openupval->level >= savestack(L, func)))
    return -1;
  n = leafresult(L, func, bcfindex(s2v(func)), nargs, values, &forward);
  if (n < 0 || l_unlikely(gethookmask(L)))
    return -1;
  if (forward) {
    L->top.p = func + 1 + nargs;
    moveresults(L, func, n, cast_uint(wanted + 1));
  }
  else {
    int i;
    if (wanted == LUA_MULTRET) {
      if (n > L->ci->top.p - func) return -1;
      wanted = n;
    }
    for (i = 0; i < n && i < wanted; i++) setobj2s(L, func + i, values + i);
    for (; i < wanted; i++) setnilvalue2s(func + i);
    L->top.p = func + wanted;
  }
  return n;
}

int luaD_callleaffallback (lua_State *L, StkId func, int wanted) {
  unsigned status = cast_uint(wanted + 1);
  lua_assert(checktag(s2v(func), LUA_VBCF));
#if !defined(HARDSTACKTESTS)
  if (bcfindex(s2v(func)) == LUA_LEAF_YIELD && trysuspend(L, func, status))
    return 1;
#endif
  luaD_precallC(L, func, status, fvalue(s2v(func)), 1);
  return 0;
}

l_sinline int precallmanaged (lua_State *L, StkId func, unsigned status) {
  if (checktag(s2v(func), LUA_VBCF)) {
    int n = luaD_tryleaf(L, func, cast_int(L->top.p - func) - 1,
                        get_nresults(status));
    if (n >= 0) {
      luaTK_checkstop(L);
      luaCP_checklimit(L);
      return n;
    }
  }
  return luaD_precallC(L, func, status, fvalue(s2v(func)), 1);
}

/*
** Prepare a function for a tail call, building its call info on top
** of the current call info. 'narg1' is the number of arguments plus 1
** (so that it includes the function itself). Return the number of
** results, if it was a C function, or -1 for a Lua function.
*/
int luaD_pretailcall (lua_State *L, CallInfo *ci, StkId func,
                                    int narg1, int delta) {
  unsigned status = LUA_MULTRET + 1;
  lu_byte tag;
 retry:
  tag = ttypetag(s2v(func));
  switch (tag) {
    case LUA_VCCL: {  /* C closure */
      CClosure *closure = clCvalue(s2v(func));
      return luaD_precallC(L, func, status, closure->f, closure->managed);
    }
    case LUA_VLCF:
      return luaD_precallC(L, func, status, fvalue(s2v(func)), 0);
    case LUA_VMCF:
      return precallmanaged(L, func, status);
    case LUA_VLCL: {  /* Lua function */
      Proto *p = clLvalue(s2v(func))->p;
      int fsize = p->maxstacksize;  /* frame size */
      int nfixparams = p->numparams;
      int i;
      checkstackp(L, fsize - delta, func);
      ci->func.p -= delta;  /* restore 'func' (if vararg) */
      for (i = 0; i < narg1; i++)  /* move down function and arguments */
        setobjs2s(L, ci->func.p + i, func + i);
      func = ci->func.p;  /* moved-down function */
      for (; narg1 <= nfixparams; narg1++)
        setnilvalue2s(func + narg1);  /* complete missing arguments */
      ci->top.p = func + 1 + fsize;  /* top for new function */
      lua_assert(ci->top.p <= L->stack_last.p);
      ci->u.l.savedpc = p->code;  /* starting point */
      ci->u.l.captures = 0;
      ci->u.l.rootpc = NULL;
      ci->callstatus |= CIST_TAIL;
      L->top.p = func + narg1;  /* set top */
      return -1;
    }
    default: {  /* not a function */
      checkstackp(L, 1, func);  /* space for metamethod */
      status = tryfuncTM(L, func, status);  /* try '__call' metamethod */
      narg1++;
      goto retry;  /* try again */
    }
  }
}


/*
** Prepares the call to a function (C or Lua). For C functions, also do
** the call. The function to be called is at '*func'.  The arguments
** are on the stack, right after the function.  Returns the CallInfo
** to be executed, if it was a Lua function. Otherwise (a C function)
** returns NULL, with all the results on the stack, starting at the
** original function position.
*/
CallInfo *luaD_precall (lua_State *L, StkId func, int nresults) {
  unsigned status = cast_uint(nresults + 1);
  lu_byte tag;
  lua_assert(status <= MAXRESULTS + 1);
 retry:
  tag = ttypetag(s2v(func));
  switch (tag) {
    case LUA_VCCL: {  /* C closure */
      CClosure *closure = clCvalue(s2v(func));
      luaD_precallC(L, func, status, closure->f, closure->managed);
      return NULL;
    }
    case LUA_VLCF:
      luaD_precallC(L, func, status, fvalue(s2v(func)), 0);
      return NULL;
    case LUA_VMCF:
      precallmanaged(L, func, status);
      return NULL;
    case LUA_VLCL: {  /* Lua function */
      CallInfo *ci;
      Proto *p = clLvalue(s2v(func))->p;
      int narg = cast_int(L->top.p - func) - 1;  /* number of real arguments */
      int nfixparams = p->numparams;
      int fsize = p->maxstacksize;  /* frame size */
      checkstackp(L, fsize, func);
      L->ci = ci = prepCallInfo(L, func, status, func + 1 + fsize);
      luaD_initluaframe(ci, p->code);
      for (; narg < nfixparams; narg++)
        setnilvalue2s(L->top.p++);  /* complete missing arguments */
      lua_assert(ci->top.p <= L->stack_last.p);
      return ci;
    }
    default: {  /* not a function */
      checkstackp(L, 1, func);  /* space for metamethod */
      status = tryfuncTM(L, func, status);  /* try '__call' metamethod */
      goto retry;  /* try again with metamethod */
    }
  }
}


/*
** Call a function (C or Lua) through C. 'inc' can be 1 (increment
** number of recursive invocations in the C stack) or nyci (the same
** plus increment number of non-yieldable calls).
** This function can be called with some use of EXTRA_STACK, so it should
** check the stack before doing anything else. 'luaD_precall' already
** does that.
*/
l_sinline void ccall (lua_State *L, StkId func, int nResults, l_uint32 inc,
                       int returnyield) {
  CallInfo *ci;
  luaD_checknativestack(L);
  L->nCcalls += inc;
  if (l_unlikely(getCcalls(L) >= LUAI_MAXCCALLS)) {
    checkstackp(L, 0, func);  /* free any use of EXTRA_STACK */
    luaE_checkcstack(L);
  }
  if ((ci = luaD_precall(L, func, nResults)) != NULL) {  /* Lua function? */
    ci->callstatus |= CIST_FRESH;  /* mark that it is a "fresh" execute */
    if (luaV_execute(L, ci) && !returnyield)
      luaD_throw(L, LUA_YIELD);  /* unwind a caller using the C API */
  }
  L->nCcalls -= inc;
}


/*
** External interface for 'ccall'
*/
void luaD_call (lua_State *L, StkId func, int nResults) {
  /* A bounded builtin needs no fresh C activation. Keep the ordinary call
  ** path at the recursion limit so it reports the same overflow. */
  if (checktag(s2v(func), LUA_VBCF) && getCcalls(L) < LUAI_MAXCCALLS - 1 &&
      luaD_tryleaf(L, func, cast_int(L->top.p - func) - 1, nResults) >= 0) {
    luaTK_checkstop(L);
    luaCP_checklimit(L);
    return;
  }
  ccall(L, func, nResults, 1, 0);
}


/*
** Similar to 'luaD_call', but does not allow yields during the call.
*/
void luaD_callnoyield (lua_State *L, StkId func, int nResults) {
  ccall(L, func, nResults, nyci, 0);
}


/*
** Finish the job of 'lua_pcallk' after it was interrupted by an yield.
** (The caller, 'finishCcall', does the final call to 'adjustresults'.)
** The main job is to complete the 'luaD_pcall' called by 'lua_pcallk'.
** If a '__close' method yields here, eventually control will be back
** to 'finishCcall' (when that '__close' method finally returns) and
** 'finishpcallk' will run again and close any still pending '__close'
** methods. Similarly, if a '__close' method errs, 'precover' calls
** 'unroll' which calls ''finishCcall' and we are back here again, to
** close any pending '__close' methods.
** Note that, up to the call to 'luaF_close', the corresponding
** 'CallInfo' is not modified, so that this repeated run works like the
** first one (except that it has at least one less '__close' to do). In
** particular, field CIST_RECST preserves the error status across these
** multiple runs, changing only if there is a new error.
*/
static TStatus finishpcallk (lua_State *L,  CallInfo *ci) {
  TStatus status = getcistrecst(ci);  /* get original status */
  if (l_likely(status == LUA_OK))  /* no error? */
    status = LUA_YIELD;  /* was interrupted by an yield */
  else {  /* error */
    StkId func = restorestack(L, ci->u2.funcidx);
    L->allowhook = getoah(ci);  /* restore 'allowhook' */
    func = luaF_close(L, func, status, 1);  /* can yield or raise an error */
    luaD_seterrorobj(L, status, func);
    luaD_shrinkstack(L, L);   /* restore stack size in case of overflow */
    setcistrecst(ci, LUA_OK);  /* clear original status */
  }
  ci->callstatus &= ~CIST_YPCALL;
  L->errfunc = ci->u.c.old_errfunc;
  /* if it is here, there were errors or yields; unlike 'lua_pcallk',
     do not change status */
  return status;
}


/*
** Completes the execution of a C function interrupted by an yield.
** The interruption must have happened while the function was either
** closing its tbc variables in 'moveresults' or executing
** 'lua_callk'/'lua_pcallk'. In the first case, it just redoes
** 'luaD_poscall'. In the second case, the call to 'finishpcallk'
** finishes the interrupted execution of 'lua_pcallk'.  After that, it
** calls the continuation of the interrupted function and finally it
** completes the job of the 'luaD_call' that called the function.  In
** the call to 'adjustresults', we do not know the number of results
** of the function called by 'lua_callk'/'lua_pcallk', so we are
** conservative and use LUA_MULTRET (always adjust).
*/
static void finishCcall (lua_State *L, CallInfo *ci) {
  int n;  /* actual number of results from C function */
  if (ci->callstatus & CIST_CLSRET) {  /* was closing TBC variable? */
    lua_assert(ci->callstatus & CIST_TBC);
    n = ci->u2.nres;  /* just redo 'luaD_poscall' */
    /* don't need to reset CIST_CLSRET, as it will be set again anyway */
  }
  else {
    LuaCallScope call;
    TStatus status = LUA_YIELD;  /* default if there were no errors */
    int managed = managedCfunction(s2v(ci->func.p));
    lua_KFunction kf = ci->u.c.k;  /* continuation function */
    /* must have a continuation and must be able to call it */
    lua_assert(kf != NULL && yieldable(L));
    if (ci->callstatus & CIST_YPCALL)   /* was inside a 'lua_pcallk'? */
      status = finishpcallk(L, ci);  /* finish it */
    adjustresults(L, LUA_MULTRET);  /* finish 'lua_callk' */
    luaQ_begincall(L, managed, &call);
    luaQ_entercall(L, &call);
    n = (*kf)(L, APIstatus(status), ci->u.c.ctx);  /* call continuation */
    luaQ_leavecall(L, &call);
    api_checknelems(L, n);
  }
  luaD_poscall(L, ci, n);  /* finish 'luaD_call' */
}


/*
** Executes "full continuation" (everything in the stack) of a
** previously interrupted coroutine until the stack is empty (or another
** interruption long-jumps out of the loop).
*/
static void unroll (lua_State *L, void *ud) {
  CallInfo *ci;
  UNUSED(ud);
  while ((ci = L->ci) != &L->base_ci) {  /* something in the stack */
    if (!isLua(ci))  /* C function? */
      finishCcall(L, ci);  /* complete its execution */
    else {  /* Lua function */
      luaV_finishOp(L);  /* finish interrupted instruction */
      if (luaV_execute(L, ci)) return;
    }
  }
}


/*
** Try to find a suspended protected call (a "recover point") for the
** given thread.
*/
static CallInfo *findpcall (lua_State *L) {
  CallInfo *ci;
  for (ci = L->ci; ci != NULL; ci = ci->previous) {  /* search for a pcall */
    if (ci->callstatus & CIST_YPCALL)
      return ci;
  }
  return NULL;  /* no pending pcall */
}


/*
** Signal an error in the call to 'lua_resume', not in the execution
** of the coroutine itself. (Such errors should not be handled by any
** coroutine error handler and should not kill the coroutine.)
*/
static int resume_error (lua_State *L, const char *msg, int narg) {
  api_checkpop(L, narg);
  L->top.p -= narg;  /* remove args from the stack */
  setsvalue2s(L, L->top.p, luaS_new(L, msg));  /* push error message */
  api_incr_top(L);
  return LUA_ERRRUN;
}


/*
** Do the work for 'lua_resume' in protected mode. Most of the work
** depends on the status of the coroutine: initial state, suspended
** inside a hook, or regularly suspended (optionally with a continuation
** function), plus erroneous cases: non-suspended coroutine or dead
** coroutine.
*/
static void resume (lua_State *L, void *ud) {
  int n = *(cast(int*, ud));  /* number of arguments */
  StkId firstArg = L->top.p - n;  /* first argument */
  CallInfo *ci = L->ci;
  luaTK_checkstop(L);
  if (L->status == LUA_OK)  /* starting a coroutine? */
    ccall(L, firstArg - 1, LUA_MULTRET, 0, 1);  /* just call its body */
  else {  /* resuming from previous yield */
    lua_assert(L->status == LUA_YIELD);
    L->status = LUA_OK;  /* mark that it is running (again) */
    luaD_checknativestack(L);
    if (isLua(ci)) {  /* yielded inside a hook? */
      /* undo increment made by 'luaG_traceexec': instruction was not
         executed yet */
      lua_assert(ci->callstatus & CIST_HOOKYIELD);
      ci->u.l.savedpc--;
      L->top.p = firstArg;  /* discard arguments */
      if (luaV_execute(L, ci)) return;
    }
    else {  /* 'common' yield */
      if (ci->u.c.k != NULL) {  /* does it have a continuation function? */
        LuaCallScope call;
        int managed = managedCfunction(s2v(ci->func.p));
        luaQ_begincall(L, managed, &call);
        luaQ_entercall(L, &call);
        n = (*ci->u.c.k)(L, LUA_YIELD, ci->u.c.ctx); /* call continuation */
        luaQ_leavecall(L, &call);
        api_checknelems(L, n);
      }
      luaD_poscall(L, ci, n);  /* finish 'luaD_call' */
    }
    unroll(L, NULL);  /* run continuation */
  }
}


/* A builtin yield can return through the VM to the resume boundary. */
static TStatus resumewith (lua_State *L, Pfunc run, void *data) {
  TStatus status = luaD_rawrunprotected(L, run, data);
  return status == LUA_OK && L->status == LUA_YIELD ? LUA_YIELD : status;
}


/*
** Unrolls a coroutine in protected mode while there are recoverable
** errors, that is, errors inside a protected call. (Any error
** interrupts 'unroll', and this loop protects it again so it can
** continue.) Stops with a normal end (status == LUA_OK), an yield
** (status == LUA_YIELD), or an unprotected error ('findpcall' doesn't
** find a recover point).
*/
static TStatus precover (lua_State *L, TStatus status) {
  CallInfo *ci;
  while (errorstatus(status) && (ci = findpcall(L)) != NULL) {
    L->ci = ci;  /* go down to recovery functions */
    setcistrecst(ci, status);  /* status to finish 'pcall' */
    status = resumewith(L, unroll, NULL);
  }
  return status;
}


int luaD_resume (lua_State *L, l_uint32 calls, int nargs, int *nresults) {
  TStatus status;
  if (L->status == LUA_OK) {  /* may be starting a coroutine */
    if (L->ci != &L->base_ci)  /* not in base level? */
      return resume_error(L, "cannot resume non-suspended coroutine", nargs);
    else if (L->top.p - (L->ci->func.p + 1) == nargs)  /* no function? */
      return resume_error(L, "cannot resume dead coroutine", nargs);
  }
  else if (L->status != LUA_YIELD)  /* ended with errors? */
    return resume_error(L, "cannot resume dead coroutine", nargs);
  L->nCcalls = calls;
  if (getCcalls(L) >= LUAI_MAXCCALLS)
    return resume_error(L, "C stack overflow", nargs);
  L->nCcalls++;
  L->resume_active = 1;
  luai_userstateresume(L, nargs);
  api_checkpop(L, (L->status == LUA_OK) ? nargs + 1 : nargs);
  status = resumewith(L, resume, &nargs);
   /* continue running after recoverable errors */
  status = precover(L, status);
  if (status == LUA_OK && L->task_finalizable != 0)
    status = luaD_rawrunprotected(L, luaC_finishtasknursery, NULL);
  if (l_likely(!errorstatus(status)))
    lua_assert(status == L->status);  /* normal end or yield */
  else {  /* unrecoverable error */
    L->status = status;  /* mark thread as 'dead' */
    luaD_seterrorobj(L, status, L->top.p);  /* push error message */
    L->ci->top.p = L->top.p;
  }
  *nresults = (status == LUA_YIELD) ? L->ci->u2.nyield
                                    : cast_int(L->top.p - (L->ci->func.p + 1));
  L->resume_active = 0;
  return APIstatus(status);
}

LUA_API int lua_resume (lua_State *L, lua_State *from, int nargs,
                        int *nresults) {
  l_uint32 calls = luaX_parentcalls(from);
  int status;
  lua_lock(L);
  status = luaD_resume(L, calls, nargs, nresults);
  lua_unlock(L);
  return status;
}

LUA_API int lua_isyieldable (lua_State *L) {
  int result;
  uintptr_t owner = luaX_valueenter(L, 1);
  result = yieldable(L);
  luaX_valueleave(L, owner);
  return result;
}


LUA_API int lua_yieldk (lua_State *L, int nresults, lua_KContext ctx,
                        lua_KFunction k) {
  CallInfo *ci;
  luai_userstateyield(L, nresults);
  lua_lock(L);
  ci = L->ci;
  api_checkpop(L, nresults);
  if (l_unlikely(!yieldable(L))) {
    if (L != mainthread(G(L)))
      luaG_runerror(L, "attempt to yield across a C-call boundary");
    else
      luaG_runerror(L, "attempt to yield from outside a coroutine");
  }
  L->status = LUA_YIELD;
  ci->u2.nyield = nresults;  /* save number of results */
  if (isLua(ci)) {  /* inside a hook? */
    lua_assert(!isLuacode(ci));
    api_check(L, nresults == 0, "hooks cannot yield values");
    api_check(L, k == NULL, "hooks cannot continue after yielding");
  }
  else {
    if ((ci->u.c.k = k) != NULL)  /* is there a continuation? */
      ci->u.c.ctx = ctx;  /* save context */
    luaD_throw(L, LUA_YIELD);
  }
  lua_assert(ci->callstatus & CIST_HOOKED);  /* must be inside a hook */
  lua_unlock(L);
  return 0;  /* return to 'luaD_hook' */
}


/*
** Auxiliary structure to call 'luaF_close' in protected mode.
*/
struct CloseP {
  StkId level;
  TStatus status;
};


/*
** Auxiliary function to call 'luaF_close' in protected mode.
*/
static void closepaux (lua_State *L, void *ud) {
  struct CloseP *pcl = cast(struct CloseP *, ud);
  luaF_close(L, pcl->level, pcl->status, 0);
}


/*
** Calls 'luaF_close' in protected mode. Return the original status
** or, in case of errors, the new status.
*/
TStatus luaD_closeprotected (lua_State *L, ptrdiff_t level, TStatus status) {
  CallInfo *old_ci = L->ci;
  lu_byte old_allowhooks = L->allowhook;
  for (;;) {  /* keep closing upvalues until no more errors */
    struct CloseP pcl;
    pcl.level = restorestack(L, level); pcl.status = status;
    status = luaD_rawrunprotected(L, &closepaux, &pcl);
    if (l_likely(status == LUA_OK))  /* no more errors? */
      return pcl.status;
    else {  /* an error occurred; restore saved state and repeat */
      L->ci = old_ci;
      L->allowhook = old_allowhooks;
    }
  }
}


static TStatus recovercall (lua_State *L, CallInfo *ci, lu_byte allowhooks,
                             ptrdiff_t top, TStatus status) {
  L->ci = ci;
  L->allowhook = allowhooks;
  status = luaD_closeprotected(L, top, status);
  luaD_seterrorobj(L, status, restorestack(L, top));
  luaD_shrinkstack(L, L);
  return status;
}


/* Restore the caller's frame and stack on an error. */
TStatus luaD_pcall (lua_State *L, Pfunc func, void *u, ptrdiff_t old_top,
                                  ptrdiff_t ef) {
  TStatus status;
  CallInfo *old_ci = L->ci;
  lu_byte old_allowhooks = L->allowhook;
  ptrdiff_t old_errfunc = L->errfunc;
  L->errfunc = ef;
  status = luaD_rawrunprotected(L, func, u);
  if (l_unlikely(status != LUA_OK))
    status = recovercall(L, old_ci, old_allowhooks, old_top, status);
  L->errfunc = old_errfunc;
  return status;
}



#if !defined(HARDSTACKTESTS)
struct NativeProtectedCall {
  lua_CFunction function;
  int managed;
};


/* The enclosing opaque call already owns the carrier and any native permit.
** Publish the nested frame under a stack claim and let the callee enter
** managed execution through the usual APIs whenever it needs it. */
static void f_nativeprotected (lua_State *L, void *ud) {
  struct NativeProtectedCall *call = cast(struct NativeProtectedCall *, ud);
  int n;
  luaX_leavebounded(L);
  n = (*call->function)(L);
  if (luaX_tryboundedscope(L, 0)) {
    CallInfo *ci = L->ci;
    int wanted = get_nresults(ci->callstatus);
    if (l_likely(!gethookmask(L) && !(ci->callstatus & CIST_TBC) &&
                  L->openupval == NULL && L->policy == NULL &&
                  (wanted == LUA_MULTRET ? n : wanted) <= LUA_MINSTACK &&
                  !luaTK_hasstops(L))) {
      api_checknelems(L, n);
      moveresults(L, ci->func.p, n, ci->callstatus & CIST_NRESULTS);
      L->ci = ci->previous;
      return;  /* keep the stack claim while retiring the error handler */
    }
    luaX_leavebounded(L);
  }
  lua_lock(L);
  call->managed = 1;
  api_checknelems(L, n);
  luaD_poscall(L, L->ci, n);
}


int luaD_trynativepcall (lua_State *L, NativeStackContext *context,
                         int nargs, int nresults, TStatus *status) {
  LuaExecution *execution = context->execution;
  struct NativeProtectedCall call = {NULL, 0};
  CallInfo *caller;
  StkId func;
  lu_byte allowhooks;
  ptrdiff_t top, olderr;
  if ((execution != NULL && (execution->managed_depth != 0 ||
                             execution->scheduler == NULL ||
                             execution->trusted != NULL)) ||
      !luaX_tryobserve(L, 0))
    return 0;
  caller = L->ci;
  if (L->errorJmp == NULL || L->errorJmp->context != context ||
      L->errorJmp->execution != execution ||
      !pthread_equal(L->errorJmp->carrier, pthread_self()) ||
      L->native_depth == 0 || L->world_depth != 0 ||
      luaW_ownsheap(L) || L->object_scopes != NULL || isLua(caller) ||
      gethookmask(L) || L->policy != NULL ||
      luaX_contextpolicy(context) != NULL || luaTK_hasstops(L) ||
      caller->next == NULL || L->stack_last.p - L->top.p <= LUA_MINSTACK ||
      getCcalls(L) >= LUAI_MAXCCALLS - 1 ||
      context->limit == 0 || luaD_nativeposition() <= context->limit)
    goto unavailable;
  api_checkpop(L, nargs + 1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  api_checkresults(L, nargs, nresults);
  func = L->top.p - (nargs + 1);
  if (!checktag(s2v(func), LUA_VLCF)) goto unavailable;
  luaX_invalidatearguments(&L->stack_gate);
  call.function = fvalue(s2v(func));
  top = savestack(L, func);
  olderr = L->errfunc;
  allowhooks = L->allowhook;
  L->errfunc = 0;
  L->nCcalls += nyci;
  luaD_initframe(L, caller->next, func, cast_uint(nresults + 1) | CIST_C,
                    L->top.p + LUA_MINSTACK);
  *status = runprotected(L, f_nativeprotected, &call, context);
  L->nCcalls -= nyci;
  if (l_unlikely(*status != LUA_OK))
    *status = recovercall(L, caller, allowhooks, top, *status);
  L->errfunc = olderr;
  adjustresults(L, nresults);
  if (*status == LUA_OK && !call.managed) luaX_leavebounded(L);
  else lua_unlock(L);
  return 1;

 unavailable:
  luaX_leavebounded(L);
  return 0;
}
#endif


/*
** Execute a protected parser.
*/
struct SParser {  /* data to 'f_parser' */
  ZIO *z;
  Mbuffer buff;  /* dynamic structure used by the scanner */
  Dyndata dyd;  /* dynamic structures used by the parser */
  const char *mode;
  const char *name;
};


static void checkmode (lua_State *L, const char *mode, const char *x) {
  if (strchr(mode, x[0]) == NULL) {
    luaO_pushfstring(L,
       "attempt to load a %s chunk (mode is '%s')", x, mode);
    luaD_throw(L, LUA_ERRSYNTAX);
  }
}


/*
** Before the first call to the reader function, Lua reserves a slot
** with a table for anchoring stuff.
*/
static void f_parser (lua_State *L, void *ud) {
  LuaNativeRoot anchor_root;
  LClosure *cl;
  struct SParser *p = cast(struct SParser *, ud);
  const char *mode = p->mode ? p->mode : "bt";
  int c;
  Table *anchor;
  ptrdiff_t otop = savestack(L, L->top.p);  /* original top */
  /* A managed load can hand off before entering the parser. Native callers
  ** retain their existing cancellation boundaries. */
  if (!isLua(L->ci) && managedCfunction(s2v(L->ci->func.p)))
    luaTK_checkstop(L);
  luaD_checknativestack(L);
  luaD_checkstack(L, 2);
  luaR_pushroot(L, &anchor_root);
  anchor = luaH_new(L, 0, 0);
  sethvalue(L, &anchor_root.value, anchor);
  c = zgetc(p->z);  /* read first character */
  if (c == LUA_SIGNATURE[0]) {
    int fixed = 0;
    if (strchr(mode, 'B') != NULL)
      fixed = 1;
    else
      checkmode(L, mode, "binary");
    cl = luaU_undump(L, p->z, anchor, p->name, fixed);
  }
  else {
    checkmode(L, mode, "text");
    cl = luaY_parser(L, p->z, anchor, &p->buff, &p->dyd, p->name, c);
  }
  lua_assert(cl->nupvalues == cl->p->sizeupvalues);
  luaF_initupvals(L, cl);
  L->top.p = restorestack(L, otop);  /* restore stack */
  setclLvalue2s(L, L->top.p++, cl);  /* publish the initialized closure */
  luaR_poproot(L, &anchor_root);
}


/*
** Root an object privately while luaH_set may run an emergency collection,
** then add it to the parser's anchor table with itself as its key.
*/
void luaD_anchorobj (lua_State *L, Table *anchor, GCObject *obj) {
  LuaNativeRoot object;
  luaR_pushroot(L, &object);
  setgcovalue(L, &object.value, obj);
  luaH_set(L, anchor, &object.value, &object.value);
  /* Because this is a new key, luaH_set will call the GC barrier, so
     we don't need to call the barrier again here */
  luaR_poproot(L, &object);
}


TStatus luaD_protectedparser (lua_State *L, ZIO *z, const char *name,
                                            const char *mode) {
  struct SParser p;
  TStatus status;
  Proto *building = L->building_protos;
  incnny(L);  /* cannot yield during parsing */
  p.z = z; p.name = name; p.mode = mode;
  p.dyd.actvar.arr = NULL; p.dyd.actvar.size = 0;
  p.dyd.gt.arr = NULL; p.dyd.gt.size = 0;
  p.dyd.label.arr = NULL; p.dyd.label.size = 0;
  luaZ_initbuffer(L, &p.buff);
  status = luaD_pcall(L, f_parser, &p, savestack(L, L->top.p), L->errfunc);
  luaZ_freebuffer(L, &p.buff);
  luaM_freearray(L, p.dyd.actvar.arr, cast_sizet(p.dyd.actvar.size));
  luaM_freearray(L, p.dyd.gt.arr, cast_sizet(p.dyd.gt.size));
  luaM_freearray(L, p.dyd.label.arr, cast_sizet(p.dyd.label.size));
  /* Both successful and abandoned parser graphs now have stable, initialized
  ** bounds. A concurrent scanner may inspect them without borrowing this
  ** execution's stack gate after the release below. */
  while (L->building_protos != building) {
    Proto *proto = L->building_protos;
    L->building_protos = proto->build_next;
    proto->build_next = NULL;
    __atomic_store_n(&proto->builder, NULL, __ATOMIC_RELEASE);
  }
  decnny(L);
  return status;
}


/* These arithmetic operations cannot fail after their numeric type check. */
static int scalararith (lua_State *L, const Proto *p, const TValue *registers,
                        Instruction i, TValue *result) {
  OpCode op = GET_OPCODE(i);
  const TValue *left = registers + GETARG_B(i);
  const TValue *right;
  TValue immediate;
  int arithmetic;
  if (op == OP_ADDI) {
    setivalue(&immediate, GETARG_sC(i));
    right = &immediate;
    arithmetic = LUA_OPADD;
  }
  else if (op >= OP_ADDK && op <= OP_MULK) {
    right = p->k + GETARG_C(i);
    arithmetic = LUA_OPADD + (op - OP_ADDK);
  }
  else {
    right = registers + GETARG_C(i);
    arithmetic = LUA_OPADD + (op - OP_ADD);
  }
  if (ttisinteger(left) && ttisinteger(right)) {
    lua_Integer a = ivalue(left), b = ivalue(right), n;
    if (arithmetic == LUA_OPADD) n = intop(+, a, b);
    else if (arithmetic == LUA_OPMUL) n = intop(*, a, b);
    else n = intop(-, a, b);
    setivalue(result, n);
    return 1;
  }
  return luaO_rawarith(L, arithmetic, left, right, result);
}

/* The original arguments and closure keep all references live while this
** body runs. Allocate scratch registers only when interpretation is needed. */
static int __attribute__((noinline)) interpretscalar (lua_State *L, StkId args,
                                                       const Proto *p, TValue *result) {
  const Instruction *pc = p->code;
  TValue registers[LUA_SCALAR_REGISTERS];
  int i;
  for (i = 0; i < p->numparams; i++) {
    setobj(L, &registers[i], s2v(args + i));
  }
  for (;;) {
    Instruction ins = *pc++;
    OpCode op = GET_OPCODE(ins);
    TValue *ra;
    if (op == OP_RETURN0) return 0;
    ra = registers + GETARG_A(ins);
    if (op == OP_RETURN1) {
      setobj(L, result, ra);
      return 1;
    }
    lua_assert(pc <= p->code + p->sizecode);
    switch (op) {
      case OP_MOVE: setobj(L, ra, registers + GETARG_B(ins)); break;
      case OP_LOADI: setivalue(ra, GETARG_sBx(ins)); break;
      case OP_LOADF: setfltvalue(ra, cast_num(GETARG_sBx(ins))); break;
      case OP_LOADFALSE: setbfvalue(ra); break;
      case OP_LOADTRUE: setbtvalue(ra); break;
      case OP_LOADNIL: {
        int extra = GETARG_B(ins);
        do { setnilvalue(ra++); } while (extra-- > 0);
        break;
      }
      case OP_LOADK: setobj(L, ra, p->k + GETARG_Bx(ins)); break;
      case OP_UNM: {
        const TValue *rb = registers + GETARG_B(ins);
        if (!luaO_rawarith(L, LUA_OPUNM, rb, rb, ra)) return -1;
        break;
      }
      case OP_NOT:
        if (l_isfalse(registers + GETARG_B(ins))) { setbtvalue(ra); }
        else { setbfvalue(ra); }
        break;
      case OP_ADDI:
      case OP_ADDK: case OP_SUBK: case OP_MULK:
      case OP_ADD: case OP_SUB: case OP_MUL:
        if (!scalararith(L, p, registers, ins, ra)) return -1;
        pc++;  /* numeric operands made the metamethod branch unnecessary */
        break;
      default: return -1;
    }
  }
}

static int __attribute__((noinline)) scalarbody (lua_State *L, StkId args,
                                                 const Proto *p, TValue *result) {
  int scale = p->scalar_scale;
  if (scale != 0 && ttisinteger(s2v(args))) {
    lua_Integer scaled = intop(*, ivalue(s2v(args)), scale);
    setivalue(result, intop(+, scaled, p->scalar_bias));
    return 1;
  }
  return interpretscalar(L, args, p, result);
}

/* Until the result is committed, the ordinary call can restart with its
** stack untouched. */
int luaD_tryscalarcall (lua_State *L, StkId func, const Proto *p, int nargs,
                        int wanted, NativeStackContext *context) {
  const LuaPolicy *policy;
  TValue result;
  const TValue *value = &result;
  int i, count;
  lua_assert(p->scalar_ticks != 0);
  if (nargs < p->numparams || L->poll_remaining <= p->scalar_ticks ||
      gethookmask(L) ||
      L->errorJmp == NULL || L->errorJmp->context != context || isLua(L->ci) ||
      (L->openupval != NULL && L->openupval->level >= savestack(L, func)) ||
      L->ci->next == NULL || L->stack_last.p - L->top.p <= p->maxstacksize ||
      getCcalls(L) >= LUAI_MAXCCALLS - 1 ||
      context->limit == 0 || luaD_nativeposition() <= context->limit)
    return 0;
  /* Reentry with full admission would apply the invocation's policy. */
  policy = luaX_contextpolicy(context);
  if (policy != L->policy) {
    if (policy != NULL && policy->global == G(L)) return 0;
    if (policy == NULL && context->execution != NULL &&
        context->execution->empty_policy == G(L))
      return 0;
  }
  /* One instruction means the function returns without writing registers. */
  if (p->scalar_ticks == 1) {
    Instruction first = p->code[0];
    count = GET_OPCODE(first) == OP_RETURN1;
    if (count != 0) value = s2v(func + 1 + GETARG_A(first));
  }
  else {
    count = scalarbody(L, func + 1, p, &result);
    if (count < 0) return 0;
  }
  policy = L->policy;
  if (gethookmask(L) ||
      (policy != NULL &&
       (__atomic_load_n(&policy->hookmask, __ATOMIC_ACQUIRE) != 0 ||
        luaCP_exhausted(L))) ||
      (luaTK_hasstops(L) && luaTK_pendingon(L, context->execution) != NULL))
    return 0;
  if (wanted == LUA_MULTRET) wanted = count;
  if (wanted > 0 && count != 0) { setobj2s(L, func, value); }
  for (i = count; i < wanted; i++) setnilvalue2s(func + i);
  L->top.p = func + wanted;
  L->poll_remaining -= p->scalar_ticks;
  return 1;
}

