/*
** $Id: lfunc.c $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#define lfunc_c
#define LUA_CORE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "lprefix.h"


#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "lua.h"

#include "laccess.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "ljit.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lroots.h"
#include "ltable.h"
#include "lvm.h"



CClosure *luaF_newCclosure (lua_State *L, int nupvals) {
  GCObject *o = luaC_newobj(L, LUA_VCCL, sizeCclosure(nupvals));
  CClosure *c = gco2ccl(o);
  lua_assert(nupvals > 0);
  luaA_initlock(&c->access);
  luaA_initsharing(L, c);
  c->sequence = 0;
  c->managed = 0;
  c->nupvalues = cast_byte(nupvals);
  return c;
}


LClosure *luaF_newLclosure (lua_State *L, int nupvals) {
  GCObject *o = luaC_newobj(L, LUA_VLCL, sizeLclosure(nupvals));
  LClosure *c = gco2lcl(o);
  luaA_initlock(&c->access);
  luaA_initsharing(L, c);
  c->managed = 0;
  c->p = NULL;
  c->nupvalues = cast_byte(nupvals);
  while (nupvals--) c->upvals[nupvals] = NULL;
  return c;
}


static UpVal *newupvalobject (lua_State *L) {
  GCObject *object = luaC_newobj(L, LUA_VUPVAL, sizeof(UpVal));
  UpVal *uv = gco2upv(object);
  uv->owner = NULL;
  uv->gclist = NULL;
  uv->level = 0;
  uv->sequence = 0;
  luaA_initsharing(L, uv);
  uv->value.value_.gc = NULL;
  setnilvalue(&uv->value);
  return uv;
}

/*
** fill a closure with new closed upvalues
*/
void luaF_initupvals (lua_State *L, LClosure *cl) {
  int i;
  LuaObjectScope scope;
  luaA_begin(L, obj2gco(cl), &scope);
  for (i = 0; i < cl->nupvalues; i++) {
    lua_assert(cl->upvals[i] == NULL);
    UpVal *uv = newupvalobject(L);
    if (__atomic_load_n(&cl->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED)
      luaA_share(L, obj2gco(uv));
    __atomic_store_n(&cl->upvals[i], uv, __ATOMIC_RELEASE);
    luaC_objbarrier(L, cl, uv);
  }
  luaA_end(L, &scope);
}

void luaF_setproto (lua_State *L, LClosure *cl, Proto *proto) {
  LuaObjectScope scope;
  luaA_begin(L, obj2gco(cl), &scope);
  luaC_prewriteptr(L, cl->p, proto);
  __atomic_store_n(&cl->p, proto, __ATOMIC_RELEASE);
  luaC_objbarrier(L, cl, proto);
  luaA_end(L, &scope);
}


/*
** Create a new upvalue at the given level, and link it to the list of
** open upvalues of 'L' after entry 'prev'.
**/
static UpVal *newupval (lua_State *L, StkId level, UpVal **prev) {
  ptrdiff_t offset = savestack(L, level);
  UpVal *uv = newupvalobject(L);
  UpVal *next = *prev;
  level = restorestack(L, offset);
  uv->level = offset;
  uv->reg = cast_byte(level - (L->ci->func.p + 1));
  /* The stack snapshot owns the binding. A scanner can follow the owner
  ** as an ordinary heap edge without borrowing a moving stack. */
  setthvalue(L, &uv->value, L);
  uv->owner = L;
  uv->open.next = next;
  uv->open.previous = prev;
  if (next)
    next->open.previous = &uv->open.next;
  *prev = uv;
  return uv;
}

/* The low word of the sequence is the futex on the supported x86-64 targets.
** Register against the current writer so a completed unlock cannot be lost. */
uint64_t luaF_waitcell (UpVal *uv) {
  for (;;) {
    uint64_t version = __atomic_load_n(&uv->sequence, __ATOMIC_RELAXED);
    if (!(version & LUA_CELL_WRITING)) {
      uint64_t writing = luaF_trylockcell(uv);
      if (writing != 0) return writing;
      continue;
    }
    if (!(version & LUA_CELL_WAITERS)) {
      uint64_t waiting = version | LUA_CELL_WAITERS;
      if (!__atomic_compare_exchange_n(&uv->sequence, &version, waiting, 0,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        continue;
      version = waiting;
    }
    if (syscall(SYS_futex, &uv->sequence, FUTEX_WAIT_PRIVATE,
                cast(uint32_t, version), NULL, NULL, 0) < 0 &&
        errno != EAGAIN && errno != EINTR)
      luaX_checksync(errno);
  }
}

void luaF_wakecell (UpVal *uv) {
  if (syscall(SYS_futex, &uv->sequence, FUTEX_WAKE_PRIVATE, INT_MAX,
              NULL, NULL, 0) < 0)
    luaX_checksync(errno);
}

/* Cell holders perform only fixed-size reads, writes and binding changes.
** Waiting here retains admission and keeps the destination stack stationary. */
void luaF_loadshared (UpVal *uv, TValue *value) {
  unsigned attempt;
  uint64_t writing;
  for (attempt = 0; attempt < 7; attempt++)
    if (luaF_tryloadshared(uv, value)) return;
  writing = luaF_lockcell(uv);
  value->value_ = uv->value.value_;
  value->tt_ = uv->value.tt_;
  luaF_unlockcell(uv, writing);
}

l_sinline void storecell (lua_State *L, UpVal *uv, const TValue *value) {
  luaC_prewrite(L, &uv->value, value);
  luaA_store(L, &uv->value, value);
  luaC_barrier(L, uv, value);
}

void luaF_writecell (lua_State *L, UpVal *uv, const TValue *value) {
  uint64_t writing = luaF_lockcell(uv);
  storecell(L, uv, value);
  luaF_unlockcell(uv, writing);
}

void luaF_publishupval (lua_State *L, UpVal *uv) {
  if (upisopen(uv)) {
    StkId slot = uplevel(uv);
    StkId frame = slot - uv->reg - 1;
    CallInfo *ci = L->ci;
    lua_assert(luaF_localupval(L, uv) && uv->owner == L);
    while (ci->func.p != frame) ci = ci->previous;
    lua_assert(isLua(ci));
    luaF_writecell(L, uv, s2v(slot));
    ci->u.l.captures |= luaF_capturebit(uv->reg);
  }
}

void luaF_writeupval (lua_State *L, UpVal *uv, const TValue *value) {
  if (luaF_localupval(L, uv)) {
    luaF_writeprivate(L, uv, value);
    return;
  }
  luaF_writecapture(L, uv, value);
}

void luaF_writecapture (lua_State *L, UpVal *uv, const TValue *value) {
  LuaNativeRoot transfer, upvalue_root;
  /* An immediate value can be stored entirely under the current admission. */
  if (!iscollectable(value)) {
    luaF_writecell(L, uv, value);
    return;
  }
  luaR_pushroot(L, &transfer);
  luaR_setroot(L, &transfer, value);
  luaR_pushroot(L, &upvalue_root);
  setgcovalue(L, &upvalue_root.value, obj2gco(uv));
  luaC_tracepointer(L, uv);
  luaA_sharevalue(L, &transfer.value);
  luaF_writecell(L, uv, &transfer.value);
  luaR_poproot(L, &upvalue_root);
  luaR_poproot(L, &transfer);
}

void luaF_loadrange (lua_State *L, StkId first, int count) {
  UpVal *uv;
  ptrdiff_t from = savestack(L, first);
  ptrdiff_t to = from + cast(ptrdiff_t, count) * sizeof(StackValue);
  for (uv = L->openupval; uv != NULL; uv = uv->open.next) {
    if (uv->level < from) break;
    if (uv->level < to && !luaF_localupval(L, uv)) {
      luaF_loadupval(L, uv, s2v(restorestack(L, uv->level)));
    }
  }
}

StkId luaF_loadlocal (lua_State *L, StkId slot) {
  UpVal *uv;
  ptrdiff_t level = savestack(L, slot);
  for (uv = L->openupval; uv != NULL && uv->level >= level;
       uv = uv->open.next) {
    if (uv->level == level) {
      if (!luaF_localupval(L, uv)) luaF_loadupval(L, uv, s2v(slot));
      break;
    }
  }
  return slot;
}

void luaF_storelocals (lua_State *L, StkId first, int count) {
  UpVal *uv;
  ptrdiff_t top = savestack(L, L->top.p);
  ptrdiff_t from = savestack(L, first);
  ptrdiff_t to = from + cast(ptrdiff_t, count) * sizeof(StackValue);
  if (L->top.p < L->ci->top.p)
    L->top.p = L->ci->top.p;
  for (uv = L->openupval; uv != NULL; uv = uv->open.next) {
    if (uv->level < from) break;
    if (uv->level < to && !luaF_localupval(L, uv))
      luaF_writecapture(L, uv, s2v(restorestack(L, uv->level)));
  }
  L->top.p = restorestack(L, top);
}

/* A join publishes a complete pointer. Managed participation prevents cell
** reclamation between this load and root publication; neither operation can
** park, allocate or call user code. Later joins do not change the selected cell. */
void luaF_getcapture (lua_State *L, LClosure *closure, int n,
                      LuaNativeRoot *capture) {
  UpVal *uv;
  UNUSED(L);
  lua_assert(0 <= n && n < closure->nupvalues);
  uv = luaF_capturecell(closure, n);
  lua_assert(uv != NULL);
  setgcovalue(L, &capture->value, obj2gco(uv));
  luaC_tracepointer(L, uv);
}

void luaF_joincaptures (lua_State *L, LClosure *to, int target,
                        LClosure *from, int source) {
  LuaObjectScope first, second;
  LuaNativeRoot target_root, source_root;
  int pair = to != from;
  int targetfirst = cast(uintptr_t, to) < cast(uintptr_t, from);
  luaR_pushroot(L, &target_root);
  setclLvalue(L, &target_root.value, to);
  luaR_pushroot(L, &source_root);
  setclLvalue(L, &source_root.value, from);
  luaA_begin(L, obj2gco(targetfirst ? to : from), &first);
  if (pair)
    luaA_begin(L, obj2gco(targetfirst ? from : to), &second);
  lua_assert(to->upvals[target] != NULL && from->upvals[source] != NULL);
  if (__atomic_load_n(&to->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED)
    luaA_share(L, obj2gco(from->upvals[source]));
  luaC_prewriteptr(L, to->upvals[target], from->upvals[source]);
  __atomic_store_n(&to->upvals[target],
    __atomic_load_n(&from->upvals[source], __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
  luaC_objbarrier(L, to, to->upvals[target]);
  if (pair) luaA_end(L, &second);
  luaA_end(L, &first);
  luaR_poproot(L, &source_root);
  luaR_poproot(L, &target_root);
}


/*
** Find and reuse, or create if it does not exist, an upvalue
** at the given level.
*/
UpVal *luaF_findupval (lua_State *L, StkId level) {
  UpVal **pp;
  UpVal *p;
  lua_assert(isLua(L->ci) && level > L->ci->func.p && level < L->ci->top.p);
  pp = &L->openupval;
  while ((p = *pp) != NULL && uplevel(p) >= level) {  /* search for it */
    lua_assert(!isdead(G(L), p));
    if (uplevel(p) == level) {  /* corresponding upvalue? */
      return p;  /* return it */
    }
    pp = &p->open.next;
  }
  /* not found: create a new upvalue after 'pp' */
  p = newupval(L, level, pp);
  return p;
}


/*
** Call closing method for object 'obj' with error object 'err'. The
** boolean 'yy' controls whether the call is yieldable.
** (This function assumes EXTRA_STACK.)
*/
static void callclosemethod (lua_State *L, TValue *obj, TValue *err, int yy) {
  LuaNativeRoot method;
  StkId top = L->top.p;
  StkId func = top;
  luaR_pushroot(L, &method);
  luaT_gettmbyobj(L, obj, TM_CLOSE, &method);
  setobj2s(L, top++, &method.value);  /* will call metamethod... */
  setobj2s(L, top++, obj);  /* with 'self' as the 1st argument */
  if (err != NULL)  /* if there was an error... */
    setobj2s(L, top++, err);  /* then error object will be 2nd argument */
  L->top.p = top;  /* add function and arguments */
  if (yy)
    luaD_call(L, func, 0);
  else
    luaD_callnoyield(L, func, 0);
  luaR_poproot(L, &method);
}


/*
** Check whether object at given level has a close metamethod and raise
** an error if not.
*/
static void checkclosemth (lua_State *L, StkId level) {
  LuaNativeRoot method;
  luaR_pushroot(L, &method);
  luaT_gettmbyobj(L, s2v(level), TM_CLOSE, &method);
  if (ttisnil(&method.value)) {  /* no metamethod? */
    int idx = cast_int(level - L->ci->func.p);  /* variable index */
    const char *vname = luaG_findlocal(L, L->ci, idx, NULL);
    if (vname == NULL) vname = "?";
    luaG_runerror(L, "variable '%s' got a non-closable value", vname);
  }
  luaR_poproot(L, &method);
}


/*
** Prepare and call a closing method.
** If status is CLOSEKTOP, the call to the closing method will be pushed
** at the top of the stack. Otherwise, values can be pushed right after
** the 'level' of the upvalue being closed, as everything after that
** won't be used again.
*/
static void prepcallclosemth (lua_State *L, StkId level, TStatus status,
                                            int yy) {
  TValue *uv = s2v(level);  /* value being closed */
  TValue *errobj;
  switch (status) {
    case LUA_OK:
      L->top.p = level + 1;  /* call will be at this level */
      /* FALLTHROUGH */
    case CLOSEKTOP:  /* don't need to change top */
      errobj = NULL;  /* no error object */
      break;
    default:  /* 'luaD_seterrorobj' will set top to level + 2 */
      errobj = s2v(level + 1);  /* error object goes after 'uv' */
      luaD_seterrorobj(L, status, level + 1);  /* set error object */
      break;
  }
  callclosemethod(L, uv, errobj, yy);
}


/* Maximum value for deltas in 'tbclist' */
#define MAXDELTA       USHRT_MAX


/*
** Insert a variable in the list of to-be-closed variables.
*/
void luaF_newtbcupval (lua_State *L, StkId level) {
  lua_assert(level > L->tbclist.p);
  if (l_isfalse(s2v(level)))
    return;  /* false doesn't need to be closed */
  checkclosemth(L, level);  /* value must have a close method */
  while (cast_uint(level - L->tbclist.p) > MAXDELTA) {
    L->tbclist.p += MAXDELTA;  /* create a dummy node at maximum delta */
    L->tbclist.p->tbclist.delta = 0;
  }
  level->tbclist.delta = cast(unsigned short, level - L->tbclist.p);
  L->tbclist.p = level;
}


void luaF_unlinkupval (UpVal *uv) {
  lua_assert(upisopen(uv));
  *uv->open.previous = uv->open.next;
  if (uv->open.next)
    uv->open.next->open.previous = uv->open.previous;
}


/*
** Close all upvalues up to the given stack level.
*/
static void unlinkbinding (UpVal *uv) {
  luaF_unlinkupval(uv);
  uv->owner = NULL;
}

/* A retired stack needs no writeback. Any surviving closure has already
** published its binding before it could outlive that stack. */
int luaF_dropupvals (lua_State *L, int wait) {
  UpVal *uv;
  while ((uv = L->openupval) != NULL) {
    uint64_t writing = wait ? luaF_lockcell(uv) : luaF_trylockcell(uv);
    if (!writing) return 0;
    unlinkbinding(uv);
    luaF_unlockcell(uv, writing);
  }
  return 1;
}

void luaF_closeupval (lua_State *L, StkId level) {
  UpVal *uv;
  while ((uv = L->openupval) != NULL && uplevel(uv) >= level) {
    TValue *slot = s2v(uplevel(uv));
    uint64_t writing = luaF_lockcell(uv);
    if (luaF_localupval(L, uv)) storecell(L, uv, slot);
    else {  /* Keep to-be-closed and debug operands usable after unlink. */
      slot->value_ = uv->value.value_;
      slot->tt_ = uv->value.tt_;
    }
    unlinkbinding(uv);
    luaF_unlockcell(uv, writing);
  }
}


/*
** Remove first element from the tbclist plus its dummy nodes.
*/
static void poptbclist (lua_State *L) {
  StkId tbc = L->tbclist.p;
  lua_assert(tbc->tbclist.delta > 0);  /* first element cannot be dummy */
  tbc -= tbc->tbclist.delta;
  while (tbc > L->stack.p && tbc->tbclist.delta == 0)
    tbc -= MAXDELTA;  /* remove dummy nodes */
  L->tbclist.p = tbc;
}


/*
** Close all upvalues and to-be-closed variables up to the given stack
** level. Return restored 'level'.
*/
StkId luaF_close (lua_State *L, StkId level, TStatus status, int yy) {
  ptrdiff_t levelrel = savestack(L, level);
  luaF_closeupval(L, level);  /* first, close the upvalues */
  while (L->tbclist.p >= level) {  /* traverse tbc's down to that level */
    StkId tbc = L->tbclist.p;  /* get variable index */
    poptbclist(L);  /* remove it from list */
    prepcallclosemth(L, tbc, status, yy);  /* close variable */
    level = restorestack(L, levelrel);
  }
  return level;
}


/* Does this instruction access a register which can have a shared cell?
** Only closure creation can bind registers. Child descriptors therefore give
** an exact set of possible bindings, independent of a particular invocation.
** Variable ranges remain conservative; call results use poscall's publication.
** The fused generic-for group must select one family throughout the group. */
static int capturedoperand (Proto *p, int pc, int reg) {
  Instruction i = p->code[pc];
  OpCode op = GET_OPCODE(i);
  int a = GETARG_A(i);
  int b = getarg(i, POS_B, SIZE_B);
  int c = getarg(i, POS_C, SIZE_C);
  switch (op) {
    case OP_RETURN0: case OP_JMP: case OP_EXTRAARG:
      return 0;
    case OP_CALL: case OP_TAILCALL: case OP_RETURN: case OP_CLOSE:
      return reg >= a;
    case OP_CONCAT:
      return reg >= a && reg < a + b;
    case OP_LOADNIL:
      return reg >= a && reg <= a + b;
    case OP_SELF:
      return reg == a || reg == a + 1 || reg == b;
    case OP_FORLOOP: case OP_FORPREP:
      return reg >= a && reg < a + 3;
    case OP_TFORPREP: case OP_TFORCALL: case OP_TFORLOOP:
    case OP_VARARGPREP:
      return 1;
    case OP_SETLIST: {
      int n = GETARG_vB(i);
      return reg >= a && (n == 0 || reg <= a + n);
    }
    case OP_VARARG:
      return (TESTARG_k(i) && reg == b) ||
        (reg >= a && (c == 0 || reg < a + c - 1));
    case OP_MMBIN: case OP_MMBINI: case OP_MMBINK:
      return reg == a || (op == OP_MMBIN && reg == b) ||
        (pc > 0 && reg == GETARG_A(p->code[pc - 1]));
    case OP_SETTABUP:
      return !TESTARG_k(i) && reg == c;
    case OP_SETTABLE:
      return reg == a || reg == b || (!TESTARG_k(i) && reg == c);
    case OP_SETI: case OP_SETFIELD:
      return reg == a || (!TESTARG_k(i) && reg == c);
    case OP_SETUPVAL: case OP_TBC: case OP_EQK: case OP_EQI:
    case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI: case OP_TEST:
    case OP_RETURN1: case OP_ERRNNIL:
      return reg == a;
    case OP_EQ: case OP_LT: case OP_LE: case OP_TESTSET:
      return reg == a || reg == b;
    default:
      if (testAMode(op) && reg == a) return 1;
      if (op == OP_MOVE || op == OP_GETI || op == OP_GETFIELD ||
          (OP_ADDI <= op && op <= OP_SHRI) ||
          (OP_UNM <= op && op <= OP_LEN))
        return reg == b;
      if (op == OP_GETTABLE || op == OP_GETVARG ||
          (OP_ADD <= op && op <= OP_SHR))
        return reg == b || reg == c;
      return 0;
  }
}

/* A scalar call reads only its arguments, constants and registers it has
** already written. Arithmetic skips the following metamethod instruction
** when its operands pass the same numeric checks as the VM. */
static lu_byte scalarticks (const Proto *p, int hascaptures) {
  unsigned live;
  int pc, ticks = 0;
  if (isvararg(p) || hascaptures ||
      p->sizecode > LUA_SCALAR_CODE_LIMIT ||
      p->maxstacksize > LUA_SCALAR_REGISTERS || p->numparams > p->maxstacksize)
    return 0;
  live = (1u << p->numparams) - 1u;
  for (pc = 0; pc < p->sizecode; pc++) {
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    unsigned a = GETARG_A(i);
    unsigned end = a;
#define livein(r) ((r) < p->maxstacksize && (live & (1u << (r))))
    ticks++;
    if (op == OP_RETURN0) return cast_byte(ticks);
    if (op == OP_RETURN1) return livein(a) ? cast_byte(ticks) : 0;
    if (a >= p->maxstacksize) return 0;
    switch (op) {
      case OP_LOADNIL:
        end += GETARG_B(i);
        if (end >= p->maxstacksize) return 0;
        break;
      case OP_LOADK:
        if (GETARG_Bx(i) >= p->sizek) return 0;
        break;
      case OP_LOADI: case OP_LOADF: case OP_LOADFALSE: case OP_LOADTRUE:
        break;
      case OP_MOVE: case OP_UNM: case OP_NOT:
        if (!livein(GETARG_B(i))) return 0;
        break;
      case OP_ADDI:
      case OP_ADDK: case OP_SUBK: case OP_MULK:
      case OP_ADD: case OP_SUB: case OP_MUL:
        if (!livein(GETARG_B(i))) return 0;
        if (op >= OP_ADD && !livein(GETARG_C(i))) return 0;
        if (op >= OP_ADDK && op <= OP_MULK && GETARG_C(i) >= p->sizek)
          return 0;
        if (++pc >= p->sizecode || !testMMMode(GET_OPCODE(p->code[pc])))
          return 0;
        break;
      default: return 0;
    }
    live |= ((1u << (end - a + 1)) - 1u) << a;
#undef livein
  }
  return 0;
}

typedef struct IntegerExpr {
  lua_Unsigned scale, bias;
} IntegerExpr;

/* The bounded-call verifier has checked every register and branch boundary.
** Integer arithmetic wraps, so these identities also hold across overflow.
** Folding never removes a dynamic type check except for the guarded input. */
static void scalaraffine (Proto *p) {
  IntegerExpr reg[LUA_SCALAR_REGISTERS];
  const Instruction *pc = p->code;
  if (p->numparams != 1 || p->scalar_ticks <= 1) return;
  reg[0] = (IntegerExpr){1, 0};
  for (;;) {
    Instruction i = *pc++;
    OpCode op = GET_OPCODE(i);
    unsigned a = GETARG_A(i);
    IntegerExpr left, right;
    int arithmetic;
    switch (op) {
      case OP_RETURN1: {
        lua_Integer scale = l_castU2S(reg[a].scale);
        lua_Integer bias = l_castU2S(reg[a].bias);
        if (scale != 0 && scale >= INT16_MIN && scale <= INT16_MAX &&
            bias >= INT32_MIN && bias <= INT32_MAX) {
          p->scalar_scale = cast(int16_t, scale);
          p->scalar_bias = cast(int32_t, bias);
        }
        return;
      }
      case OP_MOVE:
        reg[a] = reg[GETARG_B(i)];
        continue;
      case OP_LOADI:
        reg[a] = (IntegerExpr){0, l_castS2U(GETARG_sBx(i))};
        continue;
      case OP_LOADK: {
        const TValue *k = p->k + GETARG_Bx(i);
        if (!ttisinteger(k)) return;
        reg[a] = (IntegerExpr){0, l_castS2U(ivalue(k))};
        continue;
      }
      case OP_UNM:
        left = reg[GETARG_B(i)];
        reg[a] = (IntegerExpr){0u - left.scale, 0u - left.bias};
        continue;
      case OP_ADDI:
        right = (IntegerExpr){0, l_castS2U(GETARG_sC(i))};
        arithmetic = LUA_OPADD;
        break;
      case OP_ADDK: case OP_SUBK: case OP_MULK: {
        const TValue *k = p->k + GETARG_C(i);
        if (!ttisinteger(k)) return;
        right = (IntegerExpr){0, l_castS2U(ivalue(k))};
        arithmetic = LUA_OPADD + (op - OP_ADDK);
        break;
      }
      case OP_ADD: case OP_SUB: case OP_MUL:
        right = reg[GETARG_C(i)];
        arithmetic = LUA_OPADD + (op - OP_ADD);
        break;
      default: return;
    }
    left = reg[GETARG_B(i)];
    if (arithmetic == LUA_OPADD)
      reg[a] = (IntegerExpr){left.scale + right.scale, left.bias + right.bias};
    else if (arithmetic == LUA_OPSUB)
      reg[a] = (IntegerExpr){left.scale - right.scale, left.bias - right.bias};
    else {
      if (left.scale != 0 && right.scale != 0) return;
      reg[a] = (IntegerExpr){left.scale * right.bias + left.bias * right.scale,
                            left.bias * right.bias};
    }
    pc++;  /* integer operands make the metamethod instruction unreachable */
  }
}

static unsigned dispatchpair (unsigned first, unsigned second) {
  switch ((first << SIZE_OP) | second) {
#define VM_PAIR(first,second) \
    case ((first) << SIZE_OP) | (second): return LUA_PAIR_##first##_##second;
#include "loppairs.h"
#undef VM_PAIR
    default: return 0;
  }
}

/* A fixed-result range call can consume a positive index in its caller.
** Keep the bytecode and each later instruction's dispatch entry intact. */
static int stringrangecallsite (const Proto *p, int pc) {
  const Instruction *code = p->code + pc;
  TString *key;
  int a, k;
  int moves = 1;
  if (p->sizecode - pc < 3 || GET_OPCODE(code[0]) != OP_SELF ||
      GET_OPCODE(code[1]) != OP_MOVE)
    return 0;
  a = GETARG_A(code[0]);
  if (GETARG_A(code[1]) != a + 2 || GETARG_B(code[1]) >= a) return 0;
  if (GET_OPCODE(code[2]) == OP_MOVE) {
    if (p->sizecode - pc < 4 || GETARG_A(code[2]) != a + 3 ||
        GETARG_B(code[2]) != GETARG_B(code[1]))
      return 0;
    moves = 2;
  }
  if (GET_OPCODE(code[moves + 1]) != OP_CALL ||
      GETARG_A(code[moves + 1]) != a ||
      GETARG_B(code[moves + 1]) != moves + 2 ||
      GETARG_C(code[moves + 1]) != 2)
    return 0;
  k = GETARG_C(code[0]);
  if (k >= p->sizek || !ttisshrstring(&p->k[k])) return 0;
  key = tsvalue(&p->k[k]);
  return (key->shrlen == 4 && memcmp(getshrstr(key), "byte", 4) == 0) ||
         (moves == 2 && key->shrlen == 3 && memcmp(getshrstr(key), "sub", 3) == 0);
}

/* The hints name possible fields. An extra assignment or a different key
** uses the ordinary dictionary image. Loading and dumping keep Lua code. */
static unsigned recordfields (const Proto *p, int pc, lu_byte *fields,
                              lu_byte *stores) {
  Instruction insn = p->code[pc];
  unsigned hash, count = 0, reg;
  int at;
  if (GET_OPCODE(insn) != OP_NEWTABLE || TESTARG_k(insn) ||
      GETARG_vC(insn) != 0) return 0;
  hash = cast_uint(GETARG_vB(insn));
  if (hash < 2 || hash > 3) return 0;
  hash = 1u << (hash - 1);
  reg = cast_uint(GETARG_A(insn));
  for (at = pc + 2; at < p->sizecode && at - pc <= 16; at++) {
    unsigned op = GET_OPCODE(p->code[at]);
    if (op == OP_SETFIELD && cast_uint(GETARG_A(p->code[at])) == reg) {
      unsigned i, key = cast_uint(GETARG_B(p->code[at]));
      if (key >= cast_uint(p->sizek) || !ttisshrstring(&p->k[key])) return 0;
      for (i = 0; i < count; i++)
        if (tsvalue(&p->k[fields[i]]) == tsvalue(&p->k[key])) break;
      if (i == count) {
        stores[count] = cast_byte(at - pc);
        fields[count++] = cast_byte(key);
      }
      if (count == hash) return count;
    }
    else if (op == OP_NEWTABLE || op == OP_RETURN || op == OP_RETURN0 ||
             op == OP_RETURN1 || op == OP_TAILCALL || op == OP_JMP)
      break;
  }
  return count > hash / 2 ? count : 0;
}

/* The last read kills the anonymous table's only reference. The accepted
** stores and register moves cannot expose it or execute a metamethod. */
static LuaRecordSite projectconstructor (const Proto *p, int pc) {
  LuaRecordSite plan = {0};
  Instruction allocation = p->code[pc];
  unsigned array, hash, reg;
  int first, at;
  if (GET_OPCODE(allocation) != OP_NEWTABLE || TESTARG_k(allocation) ||
      p->sizecode - pc <= 2 || GET_OPCODE(p->code[pc + 1]) != OP_EXTRAARG)
    return plan;
  reg = GETARG_A(allocation);
  array = GETARG_vC(allocation);
  hash = GETARG_vB(allocation);
  if (reg >= p->maxstacksize || array > 4 || hash > 3 ||
      (array != 0 && hash != 0))
    return plan;
  first = at = pc + 2;
  if (array != 0) {
    Instruction pack, read;
    OpCode op;
    unsigned i, index;
    if (reg + array >= p->maxstacksize ||
        cast_uint(p->sizecode - at) < array + 2)
      return plan;
    for (i = 1; i <= array; i++) {
      Instruction move = p->code[at++];
      if (GET_OPCODE(move) != OP_MOVE || GETARG_A(move) != reg + i ||
          GETARG_B(move) >= reg)
        return plan;
    }
    pack = p->code[at++];
    read = p->code[at++];
    op = GET_OPCODE(read);
    if (GET_OPCODE(pack) != OP_SETLIST || GETARG_A(pack) != reg ||
        TESTARG_k(pack) || GETARG_vB(pack) != array || GETARG_vC(pack) != 0 ||
        (op != OP_GETI && op != OP_GETTABLE) ||
        GETARG_A(read) != reg || GETARG_B(read) != reg)
      return plan;
    index = GETARG_C(read);
    if (op == OP_GETI) {
      if (index > 0 && index <= array) {
        plan.source = LUA_PROJECT_REGISTER;
        plan.operand = cast_byte(reg + index);
      }
    }
    else if (index < reg) {
      plan.source = LUA_PROJECT_INDEX;
      plan.operand = cast_byte(index);
    }
    else return plan;
    plan.moves = cast_byte(array);
  }
  else {
    Instruction read;
    OpCode op;
    unsigned field;
    for (; at < p->sizecode && at - first < 4; at++) {
      Instruction store = p->code[at];
      unsigned key, operand;
      if (GET_OPCODE(store) != OP_SETFIELD || GETARG_A(store) != reg)
        break;
      key = GETARG_B(store);
      operand = GETARG_C(store);
      if (key >= cast_uint(p->sizek) || !ttisshrstring(&p->k[key]) ||
          operand >= (TESTARG_k(store) ? cast_uint(p->sizek) : reg))
        return plan;
    }
    if (at == p->sizecode) return plan;
    read = p->code[at++];
    op = GET_OPCODE(read);
    if ((op != OP_GETFIELD && op != OP_GETTABLE) ||
        GETARG_A(read) != reg || GETARG_B(read) != reg)
      return plan;
    field = GETARG_C(read);
    if (op == OP_GETTABLE) {
      if (field >= reg) return plan;
      plan.source = LUA_PROJECT_FIELD;
      plan.operand = cast_byte(field);
      plan.projection = cast_byte(at - first);
      return plan;
    }
    if (field >= cast_uint(p->sizek) || !ttisshrstring(&p->k[field]))
      return plan;
    for (int i = at - 2; i >= first; i--) {
      Instruction store = p->code[i];
      if (tsvalue(&p->k[GETARG_B(store)]) == tsvalue(&p->k[field])) {
        plan.source = TESTARG_k(store) ? LUA_PROJECT_CONSTANT
                                      : LUA_PROJECT_REGISTER;
        plan.operand = cast_byte(GETARG_C(store));
        break;
      }
    }
  }
  plan.projection = cast_byte(at - first);
  return plan;
}

/* A short constructor that feeds a store into an earlier register can
** bypass the task nursery if that receiver is shared at execution time. */
static lu_byte constructorreceiver (const Proto *p, int pc) {
  Instruction allocation = p->code[pc];
  int reg = GETARG_A(allocation);
  int distance;
  if (GETARG_vB(allocation) > 3 || GETARG_vC(allocation) > 4 ||
      TESTARG_k(allocation) || p->sizecode - pc <= 2)
    return NO_REG;
  for (distance = 2; distance < p->sizecode - pc && distance < 10; distance++) {
    Instruction ins = p->code[pc + distance];
    int dest = GETARG_A(ins);
    switch (GET_OPCODE(ins)) {
      case OP_SETFIELD: case OP_SETI: case OP_SETTABLE:
        if (dest != reg)
          return dest < reg && !TESTARG_k(ins) && GETARG_C(ins) == reg
            ? cast_byte(dest) : NO_REG;
        break;
      case OP_SETLIST:
        if (dest != reg || TESTARG_k(ins)) return NO_REG;
        break;
      case OP_MOVE: case OP_LOADI: case OP_LOADF: case OP_LOADK:
      case OP_LOADFALSE: case OP_LOADTRUE: case OP_LOADNIL:
        if (dest <= reg) return NO_REG;
        break;
      default: return NO_REG;
    }
  }
  return NO_REG;
}

static void lowerrecords (lua_State *L, Proto *p) {
  lu_byte fields[LUA_RECORD_MAX_FIELDS], stores[LUA_RECORD_MAX_FIELDS];
  int pc, sites = 0;
  _Static_assert(LUA_SETRECORD_3 - LUA_SETRECORD_0 + 1 == LUA_RECORD_MAX_FIELDS,
                 "record store dispatch range");
  for (pc = 0; pc < p->sizecode; pc++) {
    if (GET_OPCODE(p->code[pc]) == OP_NEWTABLE &&
        (recordfields(p, pc, fields, stores) != 0 ||
         projectconstructor(p, pc).projection != 0 ||
         constructorreceiver(p, pc) != NO_REG))
      sites++;
  }
  if (sites == 0) return;
  p->records = luaM_newvectorchecked(L, sites, LuaRecordSite);
  p->sizerecords = sites;
  for (pc = 0; pc < sites; pc++) p->records[pc].keys = NULL;
  sites = 0;
  for (pc = 0; pc < p->sizecode; pc++) {
    unsigned i, count;
    LuaRecordSite plan;
    Table *keys;
    if (GET_OPCODE(p->code[pc]) != OP_NEWTABLE) continue;
    count = recordfields(p, pc, fields, stores);
    plan = projectconstructor(p, pc);
    if (plan.projection == 0) {
      plan.operand = constructorreceiver(p, pc);
      if (count == 0 && plan.operand == NO_REG) continue;
    }
    plan.pc = pc;
    p->records[sites++] = plan;
    if (plan.projection != 0 && p->dispatch[pc].opcode == OP_NEWTABLE)
      p->dispatch[pc].opcode = LUA_PROJECT_TABLE;
    if (count == 0) continue;
    keys = luaH_new(L, count, 0);
    p->records[sites - 1].keys = keys;
    luaC_objbarrier(L, p, keys);
    for (i = 0; i < count; i++) {
      const TValue *key = &p->k[fields[i]];
      int store = pc + stores[i];
      obj2arr(keys, i, key);
      luaC_barrierback(L, obj2gco(keys), key);
      if (p->dispatch[store].opcode == OP_SETFIELD)
        p->dispatch[store].opcode = LUA_SETRECORD_0 + i;
    }
    luaA_share(L, obj2gco(keys));
  }
}

void luaF_lowerproto (lua_State *L, Proto *p) {
  lu_byte seen[256] = {0}, registers[256];
  unsigned count = 0, r;
  int child, upvalue, pc = 0;
  lua_assert(p->dispatch == NULL);
  for (child = 0; child < p->sizep; child++) {
    Proto *nested = p->p[child];
    for (upvalue = 0; upvalue < nested->sizeupvalues; upvalue++) {
      Upvaldesc *binding = &nested->upvalues[upvalue];
      if (binding->instack && !seen[binding->idx]) {
        seen[binding->idx] = 1;
        registers[count++] = binding->idx;
      }
    }
  }
  if (p->sizecode != 0) {
    /* Prototype metadata can outlive the parser's pooled storage. */
    luaM_checksize(L, p->sizecode, sizeof(*p->dispatch));
    p->dispatch = luaM_alloccontrol(L, cast_sizet(p->sizecode) *
                                      sizeof(*p->dispatch));
    if (p->dispatch == NULL) luaM_error(L);
  }
  for (pc = 0; pc < p->sizecode; pc++) {
    unsigned op = GET_OPCODE(p->code[pc]);
    if (op >= NUM_OPCODES) luaG_runerror(L, "invalid opcode");
    for (r = 0; r < count; r++) {
      if (capturedoperand(p, pc, registers[r])) {
        op += NUM_OPCODES;
        break;
      }
    }
    p->dispatch[pc].opcode = op;
  }
  for (pc = 0; pc + 1 < p->sizecode; pc++) {
    unsigned first = p->dispatch[pc].opcode;
    unsigned second = p->dispatch[pc + 1].opcode;
    if (first < NUM_OPCODES && second < NUM_OPCODES) {
      unsigned pair = dispatchpair(first, second);
      if (pair != 0) p->dispatch[pc].opcode = pair;
      else if (count == 0 && first == OP_SELF && stringrangecallsite(p, pc))
        p->dispatch[pc].opcode = LUA_STRING_RANGE_CALL;
      else if ((first == OP_SETFIELD && second == OP_GETFIELD) ||
               (first == OP_SETI && second == OP_GETI) ||
               (first == OP_SETTABLE && second == OP_GETTABLE)) {
        Instruction store = p->code[pc], load = p->code[pc + 1];
        if (GETARG_A(store) == GETARG_B(load) &&
            GETARG_B(store) == GETARG_C(load)) {
          if (first == OP_SETFIELD)
            p->dispatch[pc].opcode = LUA_RELOAD_FIELD;
          else if (first == OP_SETI)
            p->dispatch[pc].opcode = LUA_RELOAD_INDEX;
          else
            p->dispatch[pc].opcode = LUA_RELOAD_KEY;
        }
      }
    }
  }
  p->scalar_ticks = scalarticks(p, count != 0);
  scalaraffine(p);
  lowerrecords(L, p);
  luaJ_lower(L, p);
  luaV_linkdispatch(p);
}


Proto *luaF_newproto (lua_State *L) {
  GCObject *o = luaC_newobj(L, LUA_VPROTO, sizeof(Proto));
  Proto *f = gco2p(o);
  f->k = NULL;
  f->sizek = 0;
  f->p = NULL;
  f->sizep = 0;
  f->code = NULL;
  f->dispatch = NULL;
  f->records = NULL;
  f->sizerecords = 0;
  f->loops = NULL;
  f->sizeloops = 0;
  f->loopbytes = 0;
  f->sizecode = 0;
  f->lineinfo = NULL;
  f->sizelineinfo = 0;
  f->abslineinfo = NULL;
  f->sizeabslineinfo = 0;
  f->upvalues = NULL;
  f->sizeupvalues = 0;
  f->numparams = 0;
  f->flag = 0;
  f->maxstacksize = 0;
  f->scalar_ticks = 0;
  f->scalar_scale = 0;
  f->scalar_bias = 0;
  f->locvars = NULL;
  f->sizelocvars = 0;
  f->linedefined = 0;
  f->lastlinedefined = 0;
  f->source = NULL;
  f->builder = L;
  f->build_next = L->building_protos;
  f->trace_snapshot = 0;
  f->closure_cache = NULL;
  f->closure_epoch = 0;
  f->trace_bytes = 0;
  L->building_protos = f;
  return f;
}


lu_mem luaF_protosize (Proto *p) {
  lu_mem sz = cast(lu_mem, sizeof(Proto))
            + cast_uint(p->sizep) * sizeof(Proto*)
            + cast_uint(p->sizek) * sizeof(TValue)
            + cast_uint(p->sizelocvars) * sizeof(LocVar)
            + cast_uint(p->sizeupvalues) * sizeof(Upvaldesc);
  if (p->dispatch != NULL)
    sz += cast_uint(p->sizecode) * sizeof(*p->dispatch);
  sz += cast_uint(p->sizerecords) * sizeof(LuaRecordSite);
  sz += cast_uint(p->sizeloops) * sizeof(LuaLoopSite) +
        __atomic_load_n(&p->loopbytes, __ATOMIC_RELAXED);
  if (!(p->flag & PF_FIXED)) {
    sz += cast_uint(p->sizecode) * sizeof(Instruction);
    sz += cast_uint(p->sizelineinfo) * sizeof(lu_byte);
    sz += cast_uint(p->sizeabslineinfo) * sizeof(AbsLineInfo);
  }
  return sz;
}


void luaF_retireproto (lua_State *L, Proto *f) {
  luaJ_freeloops(L, f, 1);
  luaM_retirearray(L, f->records, cast_sizet(f->sizerecords));
  if (f->dispatch != NULL)
    luaM_retirearray(L, f->dispatch, cast_sizet(f->sizecode));
  if (!(f->flag & PF_FIXED)) {
    luaM_retirearray(L, f->code, cast_sizet(f->sizecode));
    luaM_retirearray(L, f->lineinfo, cast_sizet(f->sizelineinfo));
    luaM_retirearray(L, f->abslineinfo, cast_sizet(f->sizeabslineinfo));
  }
  luaM_retirearray(L, f->p, cast_sizet(f->sizep));
  luaM_retirearray(L, f->k, cast_sizet(f->sizek));
  luaM_retirearray(L, f->locvars, cast_sizet(f->sizelocvars));
  luaM_retirearray(L, f->upvalues, cast_sizet(f->sizeupvalues));
  luaM_retire_(L, f, sizeof(*f));
}

void luaF_freeproto (lua_State *L, Proto *f) {
  luaJ_freeloops(L, f, 0);
  luaM_freearray(L, f->records, cast_sizet(f->sizerecords));
  if (f->dispatch != NULL)
    luaM_freearray(L, f->dispatch, cast_sizet(f->sizecode));
  if (!(f->flag & PF_FIXED)) {
    luaM_freearray(L, f->code, cast_sizet(f->sizecode));
    luaM_freearray(L, f->lineinfo, cast_sizet(f->sizelineinfo));
    luaM_freearray(L, f->abslineinfo, cast_sizet(f->sizeabslineinfo));
  }
  luaM_freearray(L, f->p, cast_sizet(f->sizep));
  luaM_freearray(L, f->k, cast_sizet(f->sizek));
  luaM_freearray(L, f->locvars, cast_sizet(f->sizelocvars));
  luaM_freearray(L, f->upvalues, cast_sizet(f->sizeupvalues));
  luaM_free(L, f);
}


/*
** Look for n-th local variable at line 'line' in function 'func'.
** Returns NULL if not found.
*/
const char *luaF_getlocalname (const Proto *f, int local_number, int pc) {
  int i;
  for (i = 0; i<f->sizelocvars && f->locvars[i].startpc <= pc; i++) {
    if (pc < f->locvars[i].endpc) {  /* is variable active? */
      local_number--;
      if (local_number == 0)
        return getstr(f->locvars[i].varname);
    }
  }
  return NULL;  /* not found */
}

