/*
** $Id: ldo.h $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/

#ifndef ldo_h
#define ldo_h


#include "llimits.h"
#include "lobject.h"
#include "lstate.h"
#include "lzio.h"


/*
** Macro to check stack size and grow stack if needed.  Parameters
** 'pre'/'pos' allow the macro to preserve a pointer into the
** stack across reallocations, doing the work only when needed.
** It also allows the running of one GC step when the stack is
** reallocated.
** 'condmovestack' is used in heavy tests to force a stack reallocation
** at every check.
*/

#if !defined(HARDSTACKTESTS)
#define condmovestack(L,pre,pos)	((void)0)
#else
/* realloc stack keeping its size */
#define condmovestack(L,pre,pos)  \
  { int sz_ = stacksize(L); pre; luaD_reallocstack((L), sz_, 0); pos; }
#endif

#define luaD_checkstackaux(L,n,pre,pos)  \
	if (l_unlikely(L->stack_last.p - L->top.p <= (n))) \
	  { pre; luaD_growstack(L, n, 1); pos; } \
	else { condmovestack(L,pre,pos); }

/* In general, 'pre'/'pos' are empty (nothing to save) */
#define luaD_checkstack(L,n)	luaD_checkstackaux(L,n,(void)0,(void)0)



#define savestack(L,pt)		(cast_charp(pt) - cast_charp(L->stack.p))
#define restorestack(L,n)	cast(StkId, cast_charp(L->stack.p) + (n))


/* macro to check stack size, preserving 'p' */
#define checkstackp(L,n,p)  \
  luaD_checkstackaux(L, n, \
    ptrdiff_t t__ = savestack(L, p),  /* save 'p' */ \
    p = restorestack(L, t__))  /* 'pos' part: restore 'p' */


/*
** Maximum depth for nested C calls, syntactical nested non-terminals,
** and other features implemented through recursion in C. (Value must
** fit in a 16-bit unsigned integer. It must also be compatible with
** the size of the C stack.)
*/
#if !defined(LUAI_MAXCCALLS)
#define LUAI_MAXCCALLS		200
#endif

/* Supported native stacks grow downwards. Reserve 32 KiB for error handling,
** including unoptimized traceback frames, and stop before the final 8 KiB. */
l_sinline uintptr_t luaD_nativeposition (void) {
  uintptr_t position;
#if defined(__x86_64__)
  __asm__ volatile ("mov %%rsp, %0" : "=r" (position));
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ volatile ("mov %0, sp" : "=r" (position));
#else
#error Unsupported Luthrum Lua native stack architecture
#endif
  return position;
}

#define luaE_refreshnativestack(L) \
  { NativeStackContext *context = luaE_getnativestackcontext(); \
    __asm__ ("" : "+r" (context)); \
    uintptr_t limit = context->limit; \
    if (l_unlikely(limit == 0 || limit == UINTPTR_MAX)) \
      context->limit = limit = luaE_initnativestack(); \
    (L)->luthrum_stack_limit = limit; \
    (L)->luthrum_stack_context = context; }

#define luaD_checknativestack(L) \
  { uintptr_t position = luaD_nativeposition(); \
    if (l_unlikely(position <= (L)->luthrum_stack_limit)) \
      luaE_nativestackerror((L), position); }


/* type of protected functions, to be ran by 'runprotected' */
typedef void (*Pfunc) (lua_State *L, void *ud);

LUAI_FUNC l_noret luaD_errerr (lua_State *L);
LUAI_FUNC void luaD_seterrorobj (lua_State *L, TStatus errcode, StkId oldtop);
LUAI_FUNC TStatus luaD_protectedparser (lua_State *L, ZIO *z,
                                                  const char *name,
                                                  const char *mode);
LUAI_FUNC void luaD_hook (lua_State *L, int event, int line,
                                        int fTransfer, int nTransfer);
LUAI_FUNC void luaD_hookcall (lua_State *L, CallInfo *ci);
LUAI_FUNC int luaD_pretailcall (lua_State *L, CallInfo *ci, StkId func,
                                              int narg1, int delta);
LUAI_FUNC CallInfo *luaD_precall (lua_State *L, StkId func, int nResults);
LUAI_FUNC void luaD_call (lua_State *L, StkId func, int nResults);
LUAI_FUNC void luaD_callnoyield (lua_State *L, StkId func, int nResults);
LUAI_FUNC TStatus luaD_closeprotected (lua_State *L, ptrdiff_t level,
                                                     TStatus status);
LUAI_FUNC TStatus luaD_pcall (lua_State *L, Pfunc func, void *u,
                                        ptrdiff_t oldtop, ptrdiff_t ef);
LUAI_FUNC void luaD_poscall (lua_State *L, CallInfo *ci, int nres);
LUAI_FUNC int luaD_reallocstack (lua_State *L, int newsize, int raiseerror);
LUAI_FUNC void luaD_rebindexecution (LuaExecution *execution);
LUAI_FUNC void luaD_releaseoperations (lua_State *L);
LUAI_FUNC int luaD_canselfclose (lua_State *L);
LUAI_FUNC int luaD_growstack (lua_State *L, int n, int raiseerror);
LUAI_FUNC void luaD_shrinkstack (lua_State *L);
LUAI_FUNC int luaD_checkminstack (lua_State *L);
LUAI_FUNC void luaD_anchorobj (lua_State *L, Table *anchor, GCObject *obj);

/* Compiler nonlocal jumps must execute below, not inline into, their receiver. */
LUAI_FUNC l_noret luaD_throw (lua_State *L, TStatus errcode)
  __attribute__((noinline));
LUAI_FUNC l_noret luaD_throwbaselevel (lua_State *L, TStatus errcode);
LUAI_FUNC TStatus luaD_rawrunprotected (lua_State *L, Pfunc f, void *ud);

#endif

