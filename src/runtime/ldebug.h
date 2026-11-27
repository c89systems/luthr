/*
** $Id: ldebug.h $
** Auxiliary functions from Debug Interface module
** See Copyright Notice in lua.h
*/

#ifndef ldebug_h
#define ldebug_h


#include "lstate.h"


#define pcRel(pc, p)	(cast_int((pc) - (p)->code) - 1)


/* Active Lua function (given call info) */
#define ci_func(ci)		(clLvalue(s2v((ci)->func.p)))


#define gethook(L) __atomic_load_n(&(L)->hook, __ATOMIC_SEQ_CST)
#define gethookmask(L) __atomic_load_n(&(L)->hookmask, __ATOMIC_SEQ_CST)
#define getbasehookcount(L) \
  __atomic_load_n(&(L)->basehookcount, __ATOMIC_SEQ_CST)
#define resethookcount(L) \
  __atomic_store_n(&(L)->hookcount, cast_uint(getbasehookcount(L)), \
                   __ATOMIC_SEQ_CST)

/*
** mark for entries in 'lineinfo' array that has absolute information in
** 'abslineinfo' array
*/
#define ABSLINEINFO	(-0x80)


/*
** MAXimum number of successive Instructions WiTHout ABSolute line
** information. (A power of two allows fast divisions.)
*/
#if !defined(MAXIWTHABS)
#define MAXIWTHABS	128
#endif


LUAI_FUNC int luaG_getfuncline (const Proto *f, int pc);
enum {
  INFO_SOURCE = 1, INFO_LINE = 2, INFO_UPVALUES = 4, INFO_NAME = 8,
  INFO_TRANSFER = 16, INFO_TAIL = 32, INFO_FUNCTION = 64, INFO_LINES = 128,
  INFO_INVALID = 256
};

LUAI_FUNC unsigned luaG_infofields (const char *options);
LUAI_FUNC CallInfo *luaG_getstack (lua_State *L, int level);
LUAI_FUNC void luaG_info (lua_State *L, unsigned fields, lua_Debug *ar,
                         CallInfo *ci, const TValue *func);
LUAI_FUNC const char *luaG_findlocal (lua_State *L, CallInfo *ci, int n,
                                                    StkId *pos);
LUAI_FUNC l_noret luaG_typeerror (lua_State *L, const TValue *o,
                                                const char *opname);
LUAI_FUNC l_noret luaG_regtypeerror (lua_State *L, const TValue *o,
                                    int reg, const char *opname);
LUAI_FUNC l_noret luaG_upvaltypeerror (lua_State *L, const TValue *o,
                                      int upvalue, const char *opname);
LUAI_FUNC l_noret luaG_callerror (lua_State *L, const TValue *o);
LUAI_FUNC l_noret luaG_forerror (lua_State *L, const TValue *o,
                                               const char *what);
LUAI_FUNC l_noret luaG_concaterror (lua_State *L, const TValue *p1,
                                                  const TValue *p2);
LUAI_FUNC l_noret luaG_opinterror (lua_State *L, const TValue *p1,
                                                 const TValue *p2,
                                                 const char *msg);
LUAI_FUNC l_noret luaG_tointerror (lua_State *L, const TValue *p1,
                                                 const TValue *p2);
LUAI_FUNC l_noret luaG_ordererror (lua_State *L, const TValue *p1,
                                                 const TValue *p2);
LUAI_FUNC l_noret luaG_errnnil (lua_State *L, LClosure *cl, int k);
LUAI_FUNC l_noret luaG_runerror (lua_State *L, const char *fmt, ...);
LUAI_FUNC const char *luaG_addinfo (lua_State *L, const char *msg,
                                                  TString *src, int line);
LUAI_FUNC l_noret luaG_errormsg (lua_State *L);
LUAI_FUNC int luaG_traceexec (lua_State *L, const Instruction *pc);
LUAI_FUNC int luaG_tracecall (lua_State *L);


#endif
