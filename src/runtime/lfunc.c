/*
** $Id: lfunc.c $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#define lfunc_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>
#include <errno.h>

#include "lua.h"

#include "laccess.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lroots.h"



CClosure *luaF_newCclosure (lua_State *L, int nupvals) {
  GCObject *o = luaC_newobj(L, LUA_VCCL, sizeCclosure(nupvals));
  CClosure *c = gco2ccl(o);
  luaA_initlock(&c->access);
  luaA_initsharing(L, c);
  if (nupvals != 0) c->sequence = 0;
  c->managed = 0;
  c->managed_next = NULL;
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
  uv->mutex_ready = 0;
  uv->level = 0;
  uv->sequence = 0;
  luaA_initsharing(L, uv);
  uv->value.value_.gc = NULL;
  setnilvalue(&uv->value);
  if (pthread_mutex_init(&uv->mutex, NULL) != 0)
    luaM_error(L);
  uv->mutex_ready = 1;
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
  uv->level = savestack(L, level);
  setobj(L, &uv->value, s2v(level));
  uv->owner = L;
  uv->open.next = next;
  uv->open.previous = prev;
  if (next)
    next->open.previous = &uv->open.next;
  *prev = uv;
  return uv;
}

/* Readers do not write shared ownership words. Each component is atomic:
** an acquire seeing a writer's component also sees its preceding odd sequence,
** so a matching even sequence proves a complete snapshot. No safepoint occurs
** until validation and root publication finish. Continuous writers get bounded
** retries followed by the cell mutex; no arbitrary Lua code runs under it. */
static void loadshared (UpVal *uv, TValue *value) {
  TValue snapshot;
  unsigned attempt;
  for (attempt = 0; attempt < 8; attempt++) {
    uint64_t before = __atomic_load_n(&uv->sequence, __ATOMIC_ACQUIRE);
    if (before & 1) continue;
    __atomic_load(&uv->value.value_, &snapshot.value_, __ATOMIC_ACQUIRE);
    snapshot.tt_ = __atomic_load_n(&uv->value.tt_, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&uv->sequence, __ATOMIC_ACQUIRE) == before) {
      *value = snapshot;
      return;
    }
  }
  luaX_checksync(pthread_mutex_lock(&uv->mutex));
  *value = uv->value;
  luaX_checksync(pthread_mutex_unlock(&uv->mutex));
}

static void loadupval (lua_State *L, UpVal *uv, TValue *value) {
  if (__atomic_load_n(&uv->sharing, __ATOMIC_ACQUIRE) == cast(uintptr_t, L))
    *value = uv->value;
  else
    loadshared(uv, value);
}

void luaF_readupval (lua_State *L, UpVal *uv, LuaNativeRoot *value) {
  UNUSED(L);
  loadupval(L, uv, &value->value);
  luaC_tracevalue(L, &value->value);
  value->table_view = NULL;
}

void luaF_writeownedupval (lua_State *L, UpVal *uv, const TValue *value) {
  uint64_t sequence;
  luaC_prewrite(L, &uv->value, value);
  sequence = __atomic_load_n(&uv->sequence, __ATOMIC_RELAXED);
  lua_assert((sequence & 1) == 0);
  __atomic_store_n(&uv->sequence, sequence + 1, __ATOMIC_RELEASE);
  luaA_store(L, &uv->value, value);
  __atomic_store_n(&uv->sequence, sequence + 2, __ATOMIC_RELEASE);
  luaC_barrier(L, uv, value);
}

void luaF_writeupval (lua_State *L, UpVal *uv, const TValue *value) {
  if (__atomic_load_n(&uv->sharing, __ATOMIC_ACQUIRE) == cast(uintptr_t, L)) {
    /* No allocation, wait, callback or safepoint can expose a partial store. */
    luaF_writeownedupval(L, uv, value);
    return;
  }
  luaF_writecapture(L, uv, value);
}

void luaF_writecapture (lua_State *L, UpVal *uv, const TValue *value) {
  LuaNativeRoot transfer, upvalue_root;
  luaR_pushroot(L, &transfer);
  luaR_setroot(L, &transfer, value);
  luaR_pushroot(L, &upvalue_root);
  setgcovalue(L, &upvalue_root.value, obj2gco(uv));
  luaC_tracepointer(L, uv);
  luaA_sharevalue(L, &transfer.value);
  luaX_checksync(pthread_mutex_lock(&uv->mutex));
  luaF_writeownedupval(L, uv, &transfer.value);
  luaX_checksync(pthread_mutex_unlock(&uv->mutex));
  luaR_poproot(L, &upvalue_root);
  luaR_poproot(L, &transfer);
}

void luaF_loadrange (lua_State *L, StkId first, int count) {
  UpVal *uv;
  ptrdiff_t from = savestack(L, first);
  ptrdiff_t to = from + cast(ptrdiff_t, count) * sizeof(StackValue);
  for (uv = L->openupval; uv != NULL; uv = uv->open.next) {
    if (uv->level < from) break;
    if (uv->level < to) {
      loadupval(L, uv, s2v(restorestack(L, uv->level)));
    }
  }
}

StkId luaF_loadlocal (lua_State *L, StkId slot) {
  UpVal *uv;
  ptrdiff_t level = savestack(L, slot);
  for (uv = L->openupval; uv != NULL && uv->level >= level;
       uv = uv->open.next) {
    if (uv->level == level) {
      loadupval(L, uv, s2v(slot));
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
    if (uv->level < to)
      luaF_writeupval(L, uv, s2v(restorestack(L, uv->level)));
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
  uv = __atomic_load_n(&closure->upvals[n], __ATOMIC_ACQUIRE);
  lua_assert(uv != NULL);
  setgcovalue(L, &capture->value, obj2gco(uv));
  luaC_tracepointer(L, uv);
}

void luaF_readcapture (lua_State *L, UpVal *uv, LuaNativeRoot *value) {
  LuaNativeRoot capture;
  if (__atomic_load_n(&uv->sharing, __ATOMIC_ACQUIRE) == cast(uintptr_t, L)) {
    setobj(L, &value->value, &uv->value);
    luaC_tracevalue(L, &value->value);
    value->table_view = NULL;
    return;
  }
  luaR_pushroot(L, &capture);
  setgcovalue(L, &capture.value, obj2gco(uv));
  luaC_tracepointer(L, uv);
  loadshared(uv, &value->value);
  luaC_tracevalue(L, &value->value);
  value->table_view = NULL;
  luaR_poproot(L, &capture);
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
  L->ci->u.l.captures |= luaF_capturebit(level - (L->ci->func.p + 1));
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
static void closeupval (UpVal *uv) {
  lua_assert(uplevel(uv) < uv->owner->top.p);
  s2v(uplevel(uv))->value_ = uv->value.value_;
  s2v(uplevel(uv))->tt_ = uv->value.tt_;
  luaF_unlinkupval(uv);
  uv->owner = NULL;
}

int luaF_trycloseupval (lua_State *L) {
  UpVal *uv;
  while ((uv = L->openupval) != NULL) {
    int status = pthread_mutex_trylock(&uv->mutex);
    if (status == EBUSY) return 0;
    luaX_checksync(status);
    closeupval(uv);
    luaX_checksync(pthread_mutex_unlock(&uv->mutex));
  }
  return 1;
}

void luaF_closeupval (lua_State *L, StkId level) {
  UpVal *uv;
  while ((uv = L->openupval) != NULL && uplevel(uv) >= level) {
    luaX_checksync(pthread_mutex_lock(&uv->mutex));
    /* Preserve the authoritative binding for to-be-closed/debug operands after
    ** unlink. This also runs for dead threads: copy without inspecting the
    ** referent, which may already be logically retired by this collection. */
    closeupval(uv);
    luaX_checksync(pthread_mutex_unlock(&uv->mutex));
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

void luaF_lowerproto (lua_State *L, Proto *p) {
  lu_byte seen[256] = {0}, registers[256];
  unsigned count = 0, r;
  int child, upvalue, pc;
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
  if (count != 0)
    p->dispatch = luaM_newvectorchecked(L, p->sizecode, lu_byte);
  for (pc = 0; pc < p->sizecode; pc++) {
    unsigned op = GET_OPCODE(p->code[pc]);
    if (op >= NUM_OPCODES) luaG_runerror(L, "invalid opcode");
    for (r = 0; r < count; r++) {
      if (capturedoperand(p, pc, registers[r])) {
        op += NUM_OPCODES;
        break;
      }
    }
    if (p->dispatch != NULL) p->dispatch[pc] = cast_byte(op);
  }
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
  f->locvars = NULL;
  f->sizelocvars = 0;
  f->linedefined = 0;
  f->lastlinedefined = 0;
  f->source = NULL;
  f->builder = L;
  f->build_next = L->building_protos;
  f->trace_snapshot = 0;
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
    sz += cast_uint(p->sizecode) * sizeof(lu_byte);
  if (!(p->flag & PF_FIXED)) {
    sz += cast_uint(p->sizecode) * sizeof(Instruction);
    sz += cast_uint(p->sizelineinfo) * sizeof(lu_byte);
    sz += cast_uint(p->sizeabslineinfo) * sizeof(AbsLineInfo);
  }
  return sz;
}


void luaF_retireproto (lua_State *L, Proto *f) {
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

