/*
** $Id: ldebug.c $
** Debug Interface
** See Copyright Notice in lua.h
*/

#define ldebug_c
#define LUA_CORE

#include "lprefix.h"


#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "laccess.h"
#include "lroots.h"
#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"



#define LuaClosure(f)		((f) != NULL && (f)->c.tt == LUA_VLCL)

static const char strlocal[] = "local";
static const char strupval[] = "upvalue";

static const char *funcnamefromcall (lua_State *L, CallInfo *ci,
                                                   const char **name);


static int currentpc (CallInfo *ci) {
  lua_assert(isLua(ci));
  return pcRel(ci->u.l.savedpc, ci_func(ci)->p);
}


/*
** Get a "base line" to find the line corresponding to an instruction.
** Base lines are regularly placed at MAXIWTHABS intervals, so usually
** an integer division gets the right place. When the source file has
** large sequences of empty/comment lines, it may need extra entries,
** so the original estimate needs a correction.
** If the original estimate is -1, the initial 'if' ensures that the
** 'while' will run at least once.
** The assertion that the estimate is a lower bound for the correct base
** is valid as long as the debug info has been generated with the same
** value for MAXIWTHABS or smaller. (Previous releases use a little
** smaller value.)
*/
static int getbaseline (const Proto *f, int pc, int *basepc) {
  if (f->sizeabslineinfo == 0 || pc < f->abslineinfo[0].pc) {
    *basepc = -1;  /* start from the beginning */
    return f->linedefined;
  }
  else {
    int i = pc / MAXIWTHABS - 1;  /* get an estimate */
    /* estimate must be a lower bound of the correct base */
    lua_assert(i < 0 ||
              (i < f->sizeabslineinfo && f->abslineinfo[i].pc <= pc));
    while (i + 1 < f->sizeabslineinfo && pc >= f->abslineinfo[i + 1].pc)
      i++;  /* low estimate; adjust it */
    *basepc = f->abslineinfo[i].pc;
    return f->abslineinfo[i].line;
  }
}


/*
** Get the line corresponding to instruction 'pc' in function 'f';
** first gets a base line and from there does the increments until
** the desired instruction.
*/
int luaG_getfuncline (const Proto *f, int pc) {
  if (f->lineinfo == NULL)  /* no debug information? */
    return -1;
  else {
    int basepc;
    int baseline = getbaseline(f, pc, &basepc);
    while (basepc++ < pc) {  /* walk until given instruction */
      lua_assert(f->lineinfo[basepc] != ABSLINEINFO);
      baseline += f->lineinfo[basepc];  /* correct line */
    }
    return baseline;
  }
}


static int getcurrentline (CallInfo *ci) {
  return luaG_getfuncline(ci_func(ci)->p, currentpc(ci));
}


/* Signal handlers must not wait for a gate, allocate, or traverse live frames.
** x86_64 targets must provide lock-free accesses for every published field. */
_Static_assert(__atomic_always_lock_free(sizeof(lua_Hook), 0),
               "hook pointers must be lock-free");
_Static_assert(__atomic_always_lock_free(sizeof(int), 0) &&
               __atomic_always_lock_free(sizeof(unsigned), 0) &&
               __atomic_always_lock_free(sizeof(l_signalT), 0),
               "hook counters and masks must be lock-free");

/* Each configuration component is atomic. As with signal-time changes in
** upstream Lua, an event already being selected may use earlier settings.
** The VM observes new masks at its own branch/call/poll boundaries. */
LUA_API void lua_sethook (lua_State *L, lua_Hook func, int mask, int count) {
  if (func == NULL || mask == 0) {  /* turn off hooks? */
    mask = 0;
    func = NULL;
  }
  __atomic_store_n(&L->hook, func, __ATOMIC_SEQ_CST);
  __atomic_store_n(&L->basehookcount, count, __ATOMIC_SEQ_CST);
  resethookcount(L);
  __atomic_store_n(&L->hookmask, cast_byte(mask), __ATOMIC_SEQ_CST);
}


LUA_API lua_Hook lua_gethook (lua_State *L) {
  return gethook(L);
}


LUA_API int lua_gethookmask (lua_State *L) {
  return gethookmask(L);
}


LUA_API int lua_gethookcount (lua_State *L) {
  return getbasehookcount(L);
}


CallInfo *luaG_getstack (lua_State *L, int level) {
  CallInfo *ci;
  for (ci = L->ci; level > 0 && ci != &L->base_ci; ci = ci->previous)
    level--;
  return (level == 0 && ci != &L->base_ci) ? ci : NULL;
}


LUA_API int lua_getstack (lua_State *L, int level, lua_Debug *ar) {
  CallInfo *ci;
  if (level < 0) return 0;
  lua_lock(L);
  ci = luaG_getstack(L, level);
  if (ci != NULL)
    ar->i_ci = ci;
  lua_unlock(L);
  return ci != NULL;
}


static const char *upvalname (const Proto *p, int uv) {
  TString *s = check_exp(uv < p->sizeupvalues, p->upvalues[uv].name);
  if (s == NULL) return "?";
  else return getstr(s);
}


static const char *findvararg (CallInfo *ci, int n, StkId *pos) {
  if (clLvalue(s2v(ci->func.p))->p->flag & PF_VAHID) {
    int nextra = ci->u.l.nextraargs;
    if (n >= -nextra) {  /* 'n' is negative */
      *pos = ci->func.p - nextra - (n + 1);
      return "(vararg)";  /* generic name for any vararg */
    }
  }
  return NULL;  /* no such vararg */
}


const char *luaG_findlocal (lua_State *L, CallInfo *ci, int n, StkId *pos) {
  StkId base = ci->func.p + 1;
  const char *name = NULL;
  if (isLua(ci)) {
    if (n < 0)  /* access to vararg values? */
      return findvararg(ci, n, pos);
    else
      name = luaF_getlocalname(ci_func(ci)->p, n, currentpc(ci));
  }
  if (name == NULL) {  /* no 'standard' name? */
    StkId limit = (ci == L->ci) ? L->top.p : ci->next->func.p;
    if (limit - base >= n && n > 0) {  /* is 'n' inside 'ci' stack? */
      /* generic name for any valid slot */
      name = isLua(ci) ? "(temporary)" : "(C temporary)";
    }
    else
      return NULL;  /* no name */
  }
  if (pos)
    *pos = base + (n - 1);
  return name;
}


LUA_API const char *lua_getlocal (lua_State *L, const lua_Debug *ar, int n) {
  const char *name;
  lua_lock(L);
  if (ar == NULL) {  /* information about non-active function? */
    if (!isLfunction(s2v(L->top.p - 1)))  /* not a Lua function? */
      name = NULL;
    else  /* consider live variables at function start (parameters) */
      name = luaF_getlocalname(clLvalue(s2v(L->top.p - 1))->p, n, 0);
  }
  else {  /* active function; get information through 'ar' */
    StkId pos = NULL;  /* to avoid warnings */
    name = luaG_findlocal(L, ar->i_ci, n, &pos);
    if (name) {
      luaF_loadlocal(L, pos);
      setobjs2s(L, L->top.p, pos);
      api_incr_top(L);
    }
  }
  lua_unlock(L);
  return name;
}


LUA_API const char *lua_setlocal (lua_State *L, const lua_Debug *ar, int n) {
  StkId pos = NULL;  /* to avoid warnings */
  const char *name;
  lua_lock(L);
  name = luaG_findlocal(L, ar->i_ci, n, &pos);
  if (name) {
    api_checkpop(L, 1);
    setobjs2s(L, pos, L->top.p - 1);
    luaF_storelocals(L, pos, 1);
    L->top.p--;  /* pop value */
  }
  lua_unlock(L);
  return name;
}


static void funcinfo (lua_Debug *ar, Closure *cl) {
  if (!LuaClosure(cl)) {
    ar->source = "=[C]";
    ar->srclen = LL("=[C]");
    ar->linedefined = -1;
    ar->lastlinedefined = -1;
    ar->what = "C";
  }
  else {
    const Proto *p = cl->l.p;
    if (p->source) {
      ar->source = getlstr(p->source, ar->srclen);
    }
    else {
      ar->source = "=?";
      ar->srclen = LL("=?");
    }
    ar->linedefined = p->linedefined;
    ar->lastlinedefined = p->lastlinedefined;
    ar->what = (ar->linedefined == 0) ? "main" : "Lua";
  }
  luaO_chunkid(ar->short_src, ar->source, ar->srclen);
}


static int nextline (const Proto *p, int currentline, int pc) {
  if (p->lineinfo[pc] != ABSLINEINFO)
    return currentline + p->lineinfo[pc];
  else
    return luaG_getfuncline(p, pc);
}


static void collectvalidlines (lua_State *L, Closure *f) {
  if (!LuaClosure(f)) {
    setnilvalue2s(L->top.p);
    api_incr_top(L);
  }
  else {
    const Proto *p = f->l.p;
    int currentline = p->linedefined;
    Table *t = luaH_new(L, 0, 0);  /* new table to store active lines */
    sethvalue2s(L, L->top.p, t);  /* push it on stack */
    api_incr_top(L);
    if (p->lineinfo != NULL) {  /* proto with debug information? */
      int i;
      TValue v;
      setbtvalue(&v);  /* boolean 'true' to be the value of all indices */
      if (!(isvararg(p)))  /* regular function? */
        i = 0;  /* consider all instructions */
      else {  /* vararg function */
        lua_assert(GET_OPCODE(p->code[0]) == OP_VARARGPREP);
        currentline = nextline(p, currentline, 0);
        i = 1;  /* skip first instruction (OP_VARARGPREP) */
      }
      for (; i < p->sizelineinfo; i++) {  /* for each instruction */
        currentline = nextline(p, currentline, i);  /* get its line */
        luaH_setint(L, t, currentline, &v);  /* table[line] = true */
      }
    }
  }
}


static const char *getfuncname (lua_State *L, CallInfo *ci, const char **name) {
  /* calling function is a known function? */
  if (ci != NULL && !(ci->callstatus & CIST_TAIL))
    return funcnamefromcall(L, ci->previous, name);
  else return NULL;  /* no way to find a name */
}


unsigned luaG_infofields (const char *options) {
  unsigned fields = 0;
  for (; *options != '\0'; options++) {
    switch (*options) {
      case 'S': fields |= INFO_SOURCE; break;
      case 'l': fields |= INFO_LINE; break;
      case 'u': fields |= INFO_UPVALUES; break;
      case 'n': fields |= INFO_NAME; break;
      case 'r': fields |= INFO_TRANSFER; break;
      case 't': fields |= INFO_TAIL; break;
      case 'f': fields |= INFO_FUNCTION; break;
      case 'L': fields |= INFO_LINES; break;
      default: fields |= INFO_INVALID; break;
    }
  }
  return fields;
}


/* With a frame, the function argument is read from that frame. */
void luaG_info (lua_State *L, unsigned fields, lua_Debug *ar,
                CallInfo *ci, const TValue *func) {
  Closure *f;
  if (ci != NULL)
    func = s2v(ci->func.p);
  lua_assert(ttisfunction(func));
  f = ttisclosure(func) ? clvalue(func) : NULL;
  if (fields & INFO_SOURCE)
    funcinfo(ar, f);
  if (fields & INFO_LINE)
    ar->currentline = (ci && isLua(ci)) ? getcurrentline(ci) : -1;
  if (fields & INFO_UPVALUES) {
    ar->nups = (f == NULL) ? 0 : f->c.nupvalues;
    ar->isvararg = !LuaClosure(f) || isvararg(f->l.p);
    ar->nparams = LuaClosure(f) ? f->l.p->numparams : 0;
  }
  if (fields & INFO_TAIL) {
    ar->istailcall = ci != NULL && (ci->callstatus & CIST_TAIL);
    ar->extraargs = ci != NULL
                  ? cast_uchar((ci->callstatus & MAX_CCMT) >> CIST_CCMT) : 0;
  }
  if (fields & INFO_NAME) {
    ar->namewhat = getfuncname(L, ci, &ar->name);
    if (ar->namewhat == NULL) {
      ar->namewhat = "";
      ar->name = NULL;
    }
  }
  if (fields & INFO_TRANSFER) {
    if (ci == NULL || !(ci->callstatus & CIST_HOOKED))
      ar->ftransfer = ar->ntransfer = 0;
    else {
      ar->ftransfer = L->transferinfo.ftransfer;
      ar->ntransfer = L->transferinfo.ntransfer;
    }
  }
  if (fields & INFO_FUNCTION) {
    setobj2s(L, L->top.p, func);
    api_incr_top(L);
  }
  if (fields & INFO_LINES)
    collectvalidlines(L, f);
}


LUA_API int lua_getinfo (lua_State *L, const char *what, lua_Debug *ar) {
  unsigned fields;
  lua_lock(L);
  if (*what == '>') {
    const TValue *func = s2v(L->top.p - 1);
    api_check(L, ttisfunction(func), "function expected");
    fields = luaG_infofields(what + 1);
    L->top.p--;
    luaG_info(L, fields, ar, NULL, func);
  }
  else {
    fields = luaG_infofields(what);
    luaG_info(L, fields, ar, ar->i_ci, NULL);
  }
  lua_unlock(L);
  return (fields & INFO_INVALID) == 0;
}


/*
** {======================================================
** Symbolic Execution
** =======================================================
*/


static int filterpc (int pc, int jmptarget) {
  if (pc < jmptarget)  /* is code conditional (inside a jump)? */
    return -1;  /* cannot know who sets that register */
  else return pc;  /* current position sets that register */
}


/*
** Try to find last instruction before 'lastpc' that modified register 'reg'.
*/
static int findsetreg (const Proto *p, int lastpc, int reg) {
  int pc;
  int setreg = -1;  /* keep last instruction that changed 'reg' */
  int jmptarget = 0;  /* any code before this address is conditional */
  if (testMMMode(GET_OPCODE(p->code[lastpc])))
    lastpc--;  /* previous instruction was not actually executed */
  for (pc = 0; pc < lastpc; pc++) {
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    int a = GETARG_A(i);
    int change;  /* true if current instruction changed 'reg' */
    switch (op) {
      case OP_LOADNIL: {  /* set registers from 'a' to 'a+b' */
        int b = GETARG_B(i);
        change = (a <= reg && reg <= a + b);
        break;
      }
      case OP_TFORCALL: {  /* affect all regs above its base */
        change = (reg >= a + 2);
        break;
      }
      case OP_CALL:
      case OP_TAILCALL: {  /* affect all registers above base */
        change = (reg >= a);
        break;
      }
      case OP_JMP: {  /* doesn't change registers, but changes 'jmptarget' */
        int b = GETARG_sJ(i);
        int dest = pc + 1 + b;
        /* jump does not skip 'lastpc' and is larger than current one? */
        if (dest <= lastpc && dest > jmptarget)
          jmptarget = dest;  /* update 'jmptarget' */
        change = 0;
        break;
      }
      default:  /* any instruction that sets A */
        change = (testAMode(op) && reg == a);
        break;
    }
    if (change)
      setreg = filterpc(pc, jmptarget);
  }
  return setreg;
}


/*
** Find a "name" for the constant 'c'.
*/
static const char *kname (const Proto *p, int index, const char **name) {
  TValue *kvalue = &p->k[index];
  if (ttisstring(kvalue)) {
    *name = getstr(tsvalue(kvalue));
    return "constant";
  }
  else {
    *name = "?";
    return NULL;
  }
}


static const char *basicgetobjname (const Proto *p, int *ppc, int reg,
                                    const char **name) {
  int pc = *ppc;
  *name = luaF_getlocalname(p, reg + 1, pc);
  if (*name)  /* is a local? */
    return strlocal;
  /* else try symbolic execution */
  *ppc = pc = findsetreg(p, pc, reg);
  if (pc != -1) {  /* could find instruction? */
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    switch (op) {
      case OP_MOVE: {
        int b = GETARG_B(i);  /* move from 'b' to 'a' */
        if (b < GETARG_A(i))
          return basicgetobjname(p, ppc, b, name);  /* get name for 'b' */
        break;
      }
      case OP_GETUPVAL: {
        *name = upvalname(p, GETARG_B(i));
        return strupval;
      }
      case OP_LOADK: return kname(p, GETARG_Bx(i), name);
      case OP_LOADKX: return kname(p, GETARG_Ax(p->code[pc + 1]), name);
      default: break;
    }
  }
  return NULL;  /* could not find reasonable name */
}


/*
** Find a "name" for the register 'c'.
*/
static void rname (const Proto *p, int pc, int c, const char **name) {
  const char *what = basicgetobjname(p, &pc, c, name); /* search for 'c' */
  if (!(what && *what == 'c'))  /* did not find a constant name? */
    *name = "?";
}


/*
** Check whether table being indexed by instruction 'i' is the
** environment '_ENV'
*/
static const char *isEnv (const Proto *p, int pc, Instruction i, int isup) {
  int t = GETARG_B(i);  /* table index */
  const char *name;  /* name of indexed variable */
  if (isup)  /* is 't' an upvalue? */
    name = upvalname(p, t);
  else {  /* 't' is a register */
    const char *what = basicgetobjname(p, &pc, t, &name);
    /* 'name' must be the name of a local variable (at the current
       level or an upvalue) */
    if (what != strlocal && what != strupval)
      name = NULL;  /* cannot be the variable _ENV */
  }
  return (name && strcmp(name, LUA_ENV) == 0) ? "global" : "field";
}


/*
** Extend 'basicgetobjname' to handle table accesses
*/
static const char *getobjname (const Proto *p, int lastpc, int reg,
                               const char **name) {
  const char *kind = basicgetobjname(p, &lastpc, reg, name);
  if (kind != NULL)
    return kind;
  else if (lastpc != -1) {  /* could find instruction? */
    Instruction i = p->code[lastpc];
    OpCode op = GET_OPCODE(i);
    switch (op) {
      case OP_GETTABUP: {
        int k = GETARG_C(i);  /* key index */
        kname(p, k, name);
        return isEnv(p, lastpc, i, 1);
      }
      case OP_GETTABLE: case OP_GETVARG: {
        int k = GETARG_C(i);  /* key index */
        rname(p, lastpc, k, name);
        return isEnv(p, lastpc, i, 0);
      }
      case OP_GETI: {
        *name = "integer index";
        return "field";
      }
      case OP_GETFIELD: {
        int k = GETARG_C(i);  /* key index */
        kname(p, k, name);
        return isEnv(p, lastpc, i, 0);
      }
      case OP_SELF: {
        int k = GETARG_C(i);  /* key index */
        kname(p, k, name);
        return "method";
      }
      default: break;  /* go through to return NULL */
    }
  }
  return NULL;  /* could not find reasonable name */
}


/*
** Try to find a name for a function based on the code that called it.
** (Only works when function was called by a Lua function.)
** Returns what the name is (e.g., "for iterator", "method",
** "metamethod") and sets '*name' to point to the name.
*/
static const char *funcnamefromcode (lua_State *L, const Proto *p,
                                     int pc, const char **name) {
  TMS tm = (TMS)0;  /* (initial value avoids warnings) */
  Instruction i = p->code[pc];  /* calling instruction */
  switch (GET_OPCODE(i)) {
    case OP_CALL:
    case OP_TAILCALL:
      return getobjname(p, pc, GETARG_A(i), name);  /* get function name */
    case OP_TFORCALL: {  /* for iterator */
      *name = "for iterator";
       return "for iterator";
    }
    /* other instructions can do calls through metamethods */
    case OP_SELF: case OP_GETTABUP: case OP_GETTABLE:
    case OP_GETI: case OP_GETFIELD:
      tm = TM_INDEX;
      break;
    case OP_SETTABUP: case OP_SETTABLE: case OP_SETI: case OP_SETFIELD:
      tm = TM_NEWINDEX;
      break;
    case OP_MMBIN: case OP_MMBINI: case OP_MMBINK: {
      tm = cast(TMS, GETARG_C(i));
      break;
    }
    case OP_UNM: tm = TM_UNM; break;
    case OP_BNOT: tm = TM_BNOT; break;
    case OP_LEN: tm = TM_LEN; break;
    case OP_CONCAT: tm = TM_CONCAT; break;
    case OP_EQ: tm = TM_EQ; break;
    /* no cases for OP_EQI and OP_EQK, as they don't call metamethods */
    case OP_LT: case OP_LTI: case OP_GTI: tm = TM_LT; break;
    case OP_LE: case OP_LEI: case OP_GEI: tm = TM_LE; break;
    case OP_CLOSE: case OP_RETURN: tm = TM_CLOSE; break;
    default:
      return NULL;  /* cannot find a reasonable name */
  }
  *name = getshrstr(G(L)->tmname[tm]) + 2;
  return "metamethod";
}


/*
** Try to find a name for a function based on how it was called.
*/
static const char *funcnamefromcall (lua_State *L, CallInfo *ci,
                                                   const char **name) {
  if (ci->callstatus & CIST_HOOKED) {  /* was it called inside a hook? */
    *name = "?";
    return "hook";
  }
  else if (ci->callstatus & CIST_FIN) {  /* was it called as a finalizer? */
    *name = "__gc";
    return "metamethod";  /* report it as such */
  }
  else if (isLua(ci))
    return funcnamefromcode(L, ci_func(ci)->p, currentpc(ci), name);
  else
    return NULL;
}

/* }====================================================== */



/*
** Check whether pointer 'o' points to some value in the stack frame of
** the current function and, if so, returns its index.  Because 'o' may
** not point to a value in this stack, we cannot compare it with the
** region boundaries (undefined behavior in ISO C).
*/
static int instack (CallInfo *ci, const TValue *o) {
  int pos;
  StkId base = ci->func.p + 1;
  for (pos = 0; base + pos < ci->top.p; pos++) {
    if (o == s2v(base + pos))
      return pos;
  }
  return -1;  /* not found */
}


/*
** Checks whether value 'o' came from an upvalue. (That can only happen
** with instructions OP_GETTABUP/OP_SETTABUP, which operate directly on
** upvalues.)
*/
static const char *getupvalname (lua_State *L, CallInfo *ci, const TValue *o,
                                 const char **name) {
  LClosure *c = ci_func(ci);
  LuaNativeRoot capture;
  const char *kind = NULL;
  int i;
  luaR_pushroot(L, &capture);
  for (i = 0; i < c->nupvalues; i++) {
    UpVal *uv;
    int matches;
    uint64_t writing;
    luaF_getcapture(L, c, i, &capture);
    uv = gco2upv(gcvalue(&capture.value));
    writing = luaF_lockcell(uv);
    matches = &uv->value == o ||
      (uv->owner == L && s2v(restorestack(L, uv->level)) == o);
    luaF_unlockcell(uv, writing);
    if (matches) {
      *name = upvalname(c->p, i);
      kind = strupval;
      break;
    }
  }
  luaR_poproot(L, &capture);
  return kind;
}


static const char *formatvarinfo (lua_State *L, const char *kind,
                                                const char *name) {
  if (kind == NULL)
    return "";  /* no information */
  else
    return luaO_pushfstring(L, " (%s '%s')", kind, name);
}

/*
** Build a string with a "description" for the value 'o', such as
** "variable 'x'" or "upvalue 'y'".
*/
static const char *varinfo (lua_State *L, const TValue *o) {
  CallInfo *ci = L->ci;
  const char *name = NULL;  /* to avoid warnings */
  const char *kind = NULL;
  if (isLua(ci)) {
    kind = getupvalname(L, ci, o, &name);  /* check whether 'o' is an upvalue */
    if (!kind) {  /* not an upvalue? */
      int reg = instack(ci, o);  /* try a register */
      if (reg >= 0)  /* is 'o' a register? */
        kind = getobjname(ci_func(ci)->p, currentpc(ci), reg, &name);
    }
  }
  return formatvarinfo(L, kind, name);
}


/*
** Raise a type error
*/
static l_noret typeerror (lua_State *L, const TValue *o, const char *op,
                          const char *extra) {
  LuaNativeRoot name;
  const char *t;
  luaR_pushroot(L, &name);
  t = luaT_objtypename(L, o, &name);
  luaG_runerror(L, "attempt to %s a %s value%s", op, t, extra);
}


/*
** Raise a type error with "standard" information about the faulty
** object 'o' (using 'varinfo').
*/
l_noret luaG_typeerror (lua_State *L, const TValue *o, const char *op) {
  typeerror(L, o, op, varinfo(L, o));
}

/* A rooted operand can outlive a stack relocation during ownership waits. */
l_noret luaG_regtypeerror (lua_State *L, const TValue *o, int reg,
                          const char *op) {
  CallInfo *ci = L->ci;
  const char *name = NULL;
  const char *kind;
  lua_assert(isLua(ci));
  kind = getobjname(ci_func(ci)->p, currentpc(ci), reg, &name);
  typeerror(L, o, op, formatvarinfo(L, kind, name));
}

/* Captured-value snapshots have identity by slot, not by a borrowed address. */
l_noret luaG_upvaltypeerror (lua_State *L, const TValue *o, int upvalue,
                            const char *op) {
  const char *name;
  lua_assert(isLua(L->ci));
  name = upvalname(ci_func(L->ci)->p, upvalue);
  typeerror(L, o, op, formatvarinfo(L, strupval, name));
}


/*
** Raise an error for calling a non-callable object. Try to find a name
** for the object based on how it was called ('funcnamefromcall'); if it
** cannot get a name there, try 'varinfo'.
*/
l_noret luaG_callerror (lua_State *L, const TValue *o) {
  CallInfo *ci = L->ci;
  const char *name = NULL;  /* to avoid warnings */
  const char *kind = funcnamefromcall(L, ci, &name);
  const char *extra = kind ? formatvarinfo(L, kind, name) : varinfo(L, o);
  typeerror(L, o, "call", extra);
}


l_noret luaG_forerror (lua_State *L, const TValue *o, const char *what) {
  LuaNativeRoot name;
  luaR_pushroot(L, &name);
  luaG_runerror(L, "bad 'for' %s (number expected, got %s)",
                   what, luaT_objtypename(L, o, &name));
}


l_noret luaG_concaterror (lua_State *L, const TValue *p1, const TValue *p2) {
  if (ttisstring(p1) || cvt2str(p1)) p1 = p2;
  luaG_typeerror(L, p1, "concatenate");
}


l_noret luaG_opinterror (lua_State *L, const TValue *p1,
                         const TValue *p2, const char *msg) {
  if (!ttisnumber(p1))  /* first operand is wrong? */
    p2 = p1;  /* now second is wrong */
  luaG_typeerror(L, p2, msg);
}


/*
** Error when both values are convertible to numbers, but not to integers
*/
l_noret luaG_tointerror (lua_State *L, const TValue *p1, const TValue *p2) {
  lua_Integer temp;
  if (!luaV_tointegerns(p1, &temp, LUA_FLOORN2I))
    p2 = p1;
  luaG_runerror(L, "number%s has no integer representation", varinfo(L, p2));
}


l_noret luaG_ordererror (lua_State *L, const TValue *p1, const TValue *p2) {
  LuaNativeRoot name1, name2;
  const char *t1, *t2;
  luaR_pushroot(L, &name1);
  luaR_pushroot(L, &name2);
  t1 = luaT_objtypename(L, p1, &name1);
  t2 = luaT_objtypename(L, p2, &name2);
  if (strcmp(t1, t2) == 0)
    luaG_runerror(L, "attempt to compare two %s values", t1);
  else
    luaG_runerror(L, "attempt to compare %s with %s", t1, t2);
}


l_noret luaG_errnnil (lua_State *L, LClosure *cl, int k) {
  const char *globalname = "?";  /* default name if k == 0 */
  if (k > 0)
    kname(cl->p, k - 1, &globalname);
  luaG_runerror(L, "global '%s' already defined", globalname);
}


/* add src:line information to 'msg' */
const char *luaG_addinfo (lua_State *L, const char *msg, TString *src,
                                        int line) {
  if (src == NULL)  /* no debug information? */
    return luaO_pushfstring(L, "?:?: %s", msg);
  else {
    char buff[LUA_IDSIZE];
    size_t idlen;
    const char *id = getlstr(src, idlen);
    luaO_chunkid(buff, id, idlen);
    return luaO_pushfstring(L, "%s:%d: %s", buff, line, msg);
  }
}


l_noret luaG_errormsg (lua_State *L) {
  luaD_releaseoperations(L);
  if (L->errfunc != 0 && !luaCP_exhausted(L)) {  /* policy errors bypass handlers */
    luaA_release(L);
    (void)luaW_suspend(L);
    StkId errfunc = restorestack(L, L->errfunc);
    lua_assert(ttisfunction(s2v(errfunc)));
    setobjs2s(L, L->top.p, L->top.p - 1);  /* move argument */
    setobjs2s(L, L->top.p - 1, errfunc);  /* push function */
    L->top.p++;  /* assume EXTRA_STACK */
    luaD_callnoyield(L, L->top.p - 2, 1);  /* call it */
  }
  if (ttisnil(s2v(L->top.p - 1))) {  /* error object is nil? */
    /* change it to a proper message */
    setsvalue2s(L, L->top.p - 1, luaS_newliteral(L, "<no error object>"));
  }
  luaD_throw(L, LUA_ERRRUN);
}


l_noret luaG_runerror (lua_State *L, const char *fmt, ...) {
  CallInfo *ci = L->ci;
  const char *msg;
  va_list argp;
  /* Error construction may collect and run finalizers before the handler.
  ** This operation cannot resume; retire ownership before any user code. */
  luaD_releaseoperations(L);
  luaA_release(L);
  (void)luaW_suspend(L);
  luaC_checkGC(L);  /* error message uses memory */
  pushvfstring(L, argp, fmt, msg);
  if (isLua(ci)) {  /* Lua function? */
    /* add source:line information */
    luaG_addinfo(L, msg, ci_func(ci)->p->source, getcurrentline(ci));
    setobjs2s(L, L->top.p - 2, L->top.p - 1);  /* remove 'msg' */
    L->top.p--;
  }
  luaG_errormsg(L);
}


/*
** Check whether new instruction 'newpc' is in a different line from
** previous instruction 'oldpc'. More often than not, 'newpc' is only
** one or a few instructions after 'oldpc' (it must be after, see
** caller), so try to avoid calling 'luaG_getfuncline'. If they are
** too far apart, there is a good chance of a ABSLINEINFO in the way,
** so it goes directly to 'luaG_getfuncline'.
*/
static int changedline (const Proto *p, int oldpc, int newpc) {
  if (p->lineinfo == NULL)  /* no debug information? */
    return 0;
  if (newpc - oldpc < MAXIWTHABS / 2) {  /* not too far apart? */
    int delta = 0;  /* line difference */
    int pc = oldpc;
    for (;;) {
      int lineinfo = p->lineinfo[++pc];
      if (lineinfo == ABSLINEINFO)
        break;  /* cannot compute delta; fall through */
      delta += lineinfo;
      if (pc == newpc)
        return (delta != 0);  /* delta computed successfully */
    }
  }
  /* either instructions are too far apart or there is an absolute line
     info in the way; compute line difference explicitly */
  return (luaG_getfuncline(p, oldpc) != luaG_getfuncline(p, newpc));
}


/*
** Traces Lua calls. If code is running the first instruction of a function,
** and function is not vararg, and it is not coming from an yield,
** calls 'luaD_hookcall'. (Vararg functions will call 'luaD_hookcall'
** after adjusting its variable arguments; otherwise, they could call
** a line/count hook before the call hook. Functions coming from
** an yield already called 'luaD_hookcall' before yielding.)
*/
int luaG_tracecall (lua_State *L) {
  CallInfo *ci = L->ci;
  Proto *p = ci_func(ci)->p;
  ci->u.l.trap = 1;  /* ensure hooks will be checked */
  if (ci->u.l.savedpc == p->code) {  /* first instruction (not resuming)? */
    if (isvararg(p))
      return 0;  /* hooks will start at VARARGPREP instruction */
    else if (!(ci->callstatus & CIST_HOOKYIELD))  /* not yielded? */
      luaD_hookcall(L, ci);  /* check 'call' hook */
  }
  return 1;  /* keep 'trap' on */
}


/*
** Traces the execution of a Lua function. Called before the execution
** of each opcode, when debug is on. 'L->oldpc' stores the last
** instruction traced, to detect line changes. When entering a new
** function, 'npci' will be zero and will test as a new line whatever
** the value of 'oldpc'.  Some exceptional conditions may return to
** a function without setting 'oldpc'. In that case, 'oldpc' may be
** invalid; if so, use zero as a valid value. (A wrong but valid 'oldpc'
** at most causes an extra call to a line hook.)
** This function is not "Protected" when called, so it should correct
** 'L->top.p' before calling anything that can run the GC.
*/
int luaG_traceexec (lua_State *L, const Instruction *pc) {
  CallInfo *ci = L->ci;
  lu_byte mask = cast_byte(gethookmask(L));
  const Proto *p = ci_func(ci)->p;
  int counthook;
  /* A VM poll can refresh the trap before vararg setup. As at ordinary
  ** entry, defer hooks until that setup has initialized the activation. */
  if (GET_OPCODE(*pc) == OP_VARARGPREP)
    return 0;
  if (!(mask & (LUA_MASKLINE | LUA_MASKCOUNT))) {  /* no hooks? */
    ci->u.l.trap = 0;  /* don't need to stop again */
    return 0;  /* turn off 'trap' */
  }
  pc++;  /* reference is always next instruction */
  ci->u.l.savedpc = pc;  /* save 'pc' */
  counthook = (mask & LUA_MASKCOUNT) &&
    (__atomic_fetch_sub(&L->hookcount, 1u, __ATOMIC_SEQ_CST) == 1u);
  if (counthook)
    resethookcount(L);  /* reset count */
  else if (!(mask & LUA_MASKLINE))
    return 1;  /* no line hook and count != 0; nothing to be done now */
  if (ci->callstatus & CIST_HOOKYIELD) {  /* hook yielded last time? */
    ci->callstatus &= ~CIST_HOOKYIELD;  /* erase mark */
    return 1;  /* do not call hook again (VM yielded, so it did not move) */
  }
  if (!luaP_isIT(*(ci->u.l.savedpc - 1)))  /* top not being used? */
    L->top.p = ci->top.p;  /* correct top */
  if (counthook)
    luaD_hook(L, LUA_HOOKCOUNT, -1, 0, 0);  /* call count hook */
  if (mask & LUA_MASKLINE) {
    /* 'L->oldpc' may be invalid; use zero in this case */
    int oldpc = (L->oldpc < p->sizecode) ? L->oldpc : 0;
    int npci = pcRel(pc, p);
    if (npci <= oldpc ||  /* call hook when jump back (loop), */
        changedline(p, oldpc, npci)) {  /* or when enter new line */
      int newline = luaG_getfuncline(p, npci);
      luaD_hook(L, LUA_HOOKLINE, newline, 0, 0);  /* call line hook */
    }
    L->oldpc = npci;  /* 'pc' of last call to line hook */
  }
  if (L->status == LUA_YIELD) {  /* did hook yield? */
    if (counthook)
      __atomic_store_n(&L->hookcount, 1u, __ATOMIC_SEQ_CST);
    ci->callstatus |= CIST_HOOKYIELD;  /* mark that it yielded */
    luaD_throw(L, LUA_YIELD);
  }
  return 1;  /* keep 'trap' on */
}

