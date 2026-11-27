/* System V x86-64 loop emitter. See lua.h for license. */
#define LUA_CORE
#include "lprefix.h"

#include <string.h>

#include "lfunc.h"
#include "ljit.h"
#include "lopcodes.h"
#include "ltableaccess.h"
#include "lvm.h"

/* r12 = frame, r13 = Lua registers, r14 = thread, ebx = side-exit PC.
** Generated code accesses only the owning frame directly. Shared objects
** always go through the same C accessors and barriers as the interpreter. */
typedef struct Emitter {
  unsigned char *next, *end;
  unsigned char *exits[384];
  unsigned nexits;
  int failed;
} Emitter;

static void writebytes (Emitter *e, const void *bytes, size_t count) {
  if (count > cast_sizet(e->end - e->next)) e->failed = 1;
  if (!e->failed) {
    memcpy(e->next, bytes, count);
    e->next += count;
  }
}

#define emit(e, ...) \
  writebytes((e), (const unsigned char[]){__VA_ARGS__}, \
             sizeof((const unsigned char[]){__VA_ARGS__}))

static void word (Emitter *e, unsigned value) {
  writebytes(e, &value, sizeof(value));
}

static void immediate (Emitter *e, unsigned reg, uint64_t value) {
  emit(e, 0x48, 0xb8 + reg);  /* movabs reg, imm64 */
  writebytes(e, &value, sizeof(value));
}

static unsigned char *branch (Emitter *e, unsigned condition) {
  unsigned char *patch;
  if (condition == 0) emit(e, 0xe9);         /* jmp rel32 */
  else emit(e, 0x0f, condition);             /* jcc rel32 */
  patch = e->next;
  word(e, 0);
  return patch;
}

enum {
  BE = 0x86, EQUAL = 0x84, NOT_EQUAL = 0x85,
  NEGATIVE = 0x88, NONNEGATIVE = 0x89
};

static void linkbranch (Emitter *e, unsigned char *patch,
                        const unsigned char *target) {
  if (!e->failed) {
    int32_t displacement = cast(int32_t, target - (patch + sizeof(int32_t)));
    memcpy(patch, &displacement, sizeof(displacement));
  }
}

static void exitif (Emitter *e, unsigned condition) {
  if (e->nexits == sizeof(e->exits) / sizeof(e->exits[0])) {
    e->failed = 1;
    return;
  }
  e->exits[e->nexits++] = branch(e, condition);
}

static unsigned slot (unsigned reg) {
  return reg * sizeof(StackValue);
}

static unsigned tagoffset (unsigned reg) {
  return slot(reg) + offsetof(TValue, tt_);
}

static void loadvalue (Emitter *e, unsigned reg) {
  emit(e, 0x49, 0x8b, 0x85);  /* mov rax, [r13 + payload] */
  word(e, slot(reg));
}

static void storevalue (Emitter *e, unsigned reg) {
  emit(e, 0x49, 0x89, 0x85);  /* mov [r13 + payload], rax */
  word(e, slot(reg));
}

static void storetag (Emitter *e, unsigned reg, unsigned tag) {
  emit(e, 0x41, 0xc6, 0x85);  /* mov byte [r13 + tag], imm8 */
  word(e, tagoffset(reg));
  emit(e, tag);
}

static void comparetag (Emitter *e, unsigned reg, unsigned tag) {
  emit(e, 0x41, 0x80, 0xbd);  /* cmp byte [r13 + tag], imm8 */
  word(e, tagoffset(reg));
  emit(e, tag);
}

static void opcodepc (Emitter *e, unsigned offset) {
  emit(e, 0xbb);  /* mov ebx, imm32 */
  word(e, offset);
}

static void beginstep (Emitter *e, unsigned offset) {
  opcodepc(e, offset);
  emit(e, 0x41, 0x83, 0xbe);  /* cmp dword [r14 + poll_remaining], 1 */
  word(e, offsetof(lua_State, poll_remaining));
  emit(e, 1);
  exitif(e, BE);
}

static void charge (Emitter *e) {
  emit(e, 0x41, 0xff, 0x8e);  /* dec dword [r14 + poll_remaining] */
  word(e, offsetof(lua_State, poll_remaining));
}

static void checkhooks (Emitter *e) {
  _Static_assert(sizeof(l_signalT) == 4, "x86 hook test loads a dword");
  emit(e, 0x41, 0x83, 0xbe);  /* cmp dword [r14 + hookmask], 0 */
  word(e, offsetof(lua_State, hookmask));
  emit(e, 0);
  exitif(e, NOT_EQUAL);
}


static void loadfloat (Emitter *e, unsigned reg, unsigned xmm) {
  unsigned char *numeric, *done;
  comparetag(e, reg, LUA_VNUMFLT);
  numeric = branch(e, NOT_EQUAL);
  emit(e, 0xf2, 0x41, 0x0f, 0x10, 0x85 + 8 * xmm);  /* movsd xmm, m64 */
  word(e, slot(reg));
  done = branch(e, 0);
  linkbranch(e, numeric, e->next);
  comparetag(e, reg, LUA_VNUMINT);
  exitif(e, NOT_EQUAL);
  emit(e, 0xf2, 0x49, 0x0f, 0x2a, 0x85 + 8 * xmm);  /* cvtsi2sd xmm, m64 */
  word(e, slot(reg));
  linkbranch(e, done, e->next);
}

static void arith (Emitter *e, OpCode op, unsigned a, unsigned b, unsigned c,
                   uint64_t bits, int constant, int floating) {
  unsigned char *asfloat = NULL, *rightfloat = NULL, *done = NULL;
  unsigned action = op == OP_SUB || op == OP_SUBK ? 1u :
                    (op == OP_MUL || op == OP_MULK ? 2u : 0u);
  if (!floating) {
    comparetag(e, b, LUA_VNUMINT);
    asfloat = branch(e, NOT_EQUAL);
    if (!constant) {
      comparetag(e, c, LUA_VNUMINT);
      rightfloat = branch(e, NOT_EQUAL);
    }
    loadvalue(e, b);
    if (constant) { immediate(e, 2, bits); } /* rdx */
    else {
      emit(e, 0x49, 0x8b, 0x95);  /* mov rdx, [r13 + payload] */
      word(e, slot(c));
    }
    if (action == 2) { emit(e, 0x48, 0x0f, 0xaf, 0xc2); } /* imul rax, rdx */
    else { emit(e, 0x48, action == 0 ? 0x01 : 0x29, 0xd0); } /* add/sub */
    storevalue(e, a);
    storetag(e, a, LUA_VNUMINT);
    done = branch(e, 0);
    linkbranch(e, asfloat, e->next);
    if (rightfloat != NULL) linkbranch(e, rightfloat, e->next);
  }
  loadfloat(e, b, 0);
  if (constant) {
    immediate(e, 2, bits);
    if (floating) { emit(e, 0x66, 0x48, 0x0f, 0x6e, 0xca); }  /* movq xmm1, rdx */
    else { emit(e, 0xf2, 0x48, 0x0f, 0x2a, 0xca); }  /* cvtsi2sd xmm1, rdx */
  }
  else loadfloat(e, c, 1);
  emit(e, 0xf2, 0x0f, action == 0 ? 0x58 : action == 1 ? 0x5c : 0x59, 0xc1);
  emit(e, 0xf2, 0x41, 0x0f, 0x11, 0x85);  /* movsd [r13 + payload], xmm0 */
  word(e, slot(a));
  storetag(e, a, LUA_VNUMFLT);
  if (done != NULL) linkbranch(e, done, e->next);
}


static void divide (Emitter *e, OpCode op, unsigned a, unsigned b,
                    lua_Unsigned divisor) {
  comparetag(e, b, LUA_VNUMINT);
  exitif(e, NOT_EQUAL);
  loadvalue(e, b);
  if (op == OP_MODK && (divisor & (divisor - 1)) == 0) {
    immediate(e, 2, divisor - 1);
    emit(e, 0x48, 0x21, 0xd0);  /* and rax, rdx */
  }
  else {
    immediate(e, 1, divisor);  /* rcx */
    emit(e, 0x48, 0x99, 0x48, 0xf7, 0xf9);  /* cqo; idiv rcx */
    if (op == OP_IDIVK) {
      /* Lua rounds toward minus infinity for a positive divisor. */
      emit(e, 0x48, 0xc1, 0xea, 63, 0x48, 0x29, 0xd0);  /* shr rdx; sub */
    }
    else {
      unsigned char *positive;
      emit(e, 0x48, 0x85, 0xd2);  /* test rdx, rdx */
      positive = branch(e, NONNEGATIVE);
      emit(e, 0x48, 0x01, 0xca);  /* add rdx, rcx */
      linkbranch(e, positive, e->next);
      emit(e, 0x48, 0x89, 0xd0);  /* mov rax, rdx */
    }
  }
  storevalue(e, a);
  storetag(e, a, LUA_VNUMINT);
}


static int getfield (lua_State *L, TValue *dst, const TValue *table,
                     const TValue *key) {
  TValue result;
  lu_byte tag;
  if (!ttistable(table) ||
      !luaH_trygetshortstr(L, hvalue(table), tsvalue(key), &result, &tag) ||
      luaV_needsindex(tag))
    return 0;
  setobj(L, dst, &result);
  return 1;
}

static int gettable (lua_State *L, TValue *dst, const TValue *table,
                     const TValue *key) {
  TValue result;
  lu_byte tag;
  if (!ttistable(table) ||
      !luaH_tryget(L, hvalue(table), key, &result, &tag) || luaV_needsindex(tag))
    return 0;
  setobj(L, dst, &result);
  return 1;
}

static int getinteger (lua_State *L, TValue *dst, const TValue *table,
                       lua_Integer key) {
  TValue result;
  lu_byte tag;
  if (!ttistable(table) ||
      !luaH_trygetint(L, hvalue(table), key, &result, &tag) || luaV_needsindex(tag))
    return 0;
  setobj(L, dst, &result);
  return 1;
}

static int gettabup (LuaLoopFrame *frame, TValue *dst, unsigned index,
                     const TValue *key) {
  TValue table;
  luaF_loadupval(frame->thread, luaF_capturecell(frame->closure, index), &table);
  return getfield(frame->thread, dst, &table, key);
}

static void getupval (LuaLoopFrame *frame, TValue *dst, unsigned index) {
  luaF_loadupval(frame->thread, luaF_capturecell(frame->closure, index), dst);
}

/* A write may request a nursery poll. Charge the instruction first and
** restore the budget on a side exit only if the barrier left it alone. */
static int setfield (lua_State *L, const TValue *receiver, const TValue *key,
                     TValue *value, int record) {
  unsigned budget = L->poll_remaining--;
  if (ttistable(receiver)) {
    Table *table = hvalue(receiver);
    if ((record >= 0 && luaH_trysetrecord(L, table, tsvalue(key),
                                         cast_uint(record), value)) ||
        luaH_trysetfield(L, table, key, value, 0))
      return 1;
  }
  if (L->poll_remaining == budget - 1) L->poll_remaining = budget;
  return 0;
}


static void argument (Emitter *e, unsigned reg, unsigned index) {
  emit(e, 0x49, 0x8d, 0x85 + 8 * reg);  /* lea reg, [r13 + payload] */
  word(e, slot(index));
}

static void call (Emitter *e, uintptr_t target, int guarded) {
  immediate(e, 0, target);
  emit(e, 0xff, 0xd0);  /* call rax */
  if (guarded) {
    emit(e, 0x85, 0xc0);  /* test eax, eax */
    exitif(e, EQUAL);
  }
}

static void loadbase (Emitter *e) {
  emit(e, 0x4d, 0x8b, 0xac, 0x24);  /* mov r13, [r12 + base] */
  word(e, offsetof(LuaLoopFrame, base));
}

static void indexarg (Emitter *e, unsigned reg, unsigned value) {
  emit(e, 0xb8 + reg);  /* mov 32-bit argument register, imm32 */
  word(e, value);
}

static void readtable (Emitter *e, const Proto *p, OpCode op,
                       unsigned a, unsigned b, unsigned c) {
  switch (op) {
    case OP_GETTABUP: case OP_GETUPVAL:
      emit(e, 0x4c, 0x89, 0xe7);  /* rdi = frame */
      argument(e, 6, a);          /* rsi = destination */
      indexarg(e, 2, b);          /* edx = upvalue number */
      if (op == OP_GETUPVAL) call(e, cast(uintptr_t, getupval), 0);
      else {
        immediate(e, 1, cast(uintptr_t, &p->k[c]));  /* rcx = key */
        call(e, cast(uintptr_t, gettabup), 1);
      }
      break;
    default:
      emit(e, 0x4c, 0x89, 0xf7);  /* rdi = thread */
      argument(e, 6, a);          /* rsi = destination */
      argument(e, 2, b);          /* rdx = receiver */
      if (op == OP_GETFIELD) {
        immediate(e, 1, cast(uintptr_t, &p->k[c]));
        call(e, cast(uintptr_t, getfield), 1);
      }
      else if (op == OP_GETI) {
        indexarg(e, 1, c);
        call(e, cast(uintptr_t, getinteger), 1);
      }
      else {
        argument(e, 1, c);
        call(e, cast(uintptr_t, gettable), 1);
      }
  }
}

static void readfield (Emitter *e, const Proto *p, unsigned pc,
                       unsigned a, unsigned b, unsigned c, unsigned bounded) {
  unsigned char *ordinary, *done;
  comparetag(e, b, ctb(LUA_VUSERDATA));
  ordinary = branch(e, NOT_EQUAL);
  emit(e, 0x4c, 0x89, 0xe7);  /* rdi = frame */
  argument(e, 6, a);          /* rsi = destination */
  argument(e, 2, b);          /* rdx = receiver */
  immediate(e, 1, cast(uintptr_t, &p->k[c]));
  emit(e, 0x41, 0xb8);        /* r8d = original instruction offset */
  word(e, pc);
  call(e, cast(uintptr_t, luaD_loopfieldmethod), 1);
  loadbase(e);
  opcodepc(e, pc + 1);
  exitif(e, NEGATIVE);         /* the completed call set a trap */
  done = branch(e, 0);
  linkbranch(e, ordinary, e->next);
  if (bounded) {
    emit(e, 0x49, 0x83, 0xbc, 0x24);  /* cmp qword [r12 + detached], 0 */
    word(e, offsetof(LuaLoopFrame, detached));
    emit(e, 0);
    exitif(e, NOT_EQUAL);
  }
  readtable(e, p, OP_GETFIELD, a, b, c);
  charge(e);
  linkbranch(e, done, e->next);
}

static void writefield (Emitter *e, const Proto *p, int pc,
                        unsigned a, unsigned b, unsigned c) {
  emit(e, 0x4c, 0x89, 0xf7);  /* rdi = thread */
  argument(e, 6, a);           /* rsi = receiver */
  immediate(e, 2, cast(uintptr_t, &p->k[b]));
  if (TESTARG_k(p->code[pc])) immediate(e, 1, cast(uintptr_t, &p->k[c]));
  else argument(e, 1, c);
  emit(e, 0x41, 0xb8);  /* r8d = optional constructor field */
  word(e, cast_uint(luaV_recordfield(p, pc)));
  call(e, cast(uintptr_t, setfield), 1);
}


static void checkintegerloop (Emitter *e, unsigned control) {
  for (unsigned i = 0; i < 3; i++) {
    comparetag(e, control + i, LUA_VNUMINT);
    exitif(e, NOT_EQUAL);
  }
}

static void backedge (Emitter *e, unsigned control, unsigned words,
                      unsigned char *loop) {
  unsigned char *more, *again;
  checkintegerloop(e, control);
  charge(e);
  loadvalue(e, control);
  emit(e, 0x48, 0x85, 0xc0);  /* test rax, rax */
  more = branch(e, NOT_EQUAL);
  opcodepc(e, words);
  exitif(e, 0);
  linkbranch(e, more, e->next);
  emit(e, 0x48, 0xff, 0xc8);  /* dec rax */
  storevalue(e, control);
  loadvalue(e, control + 2);
  emit(e, 0x49, 0x03, 0x85);  /* add rax, [r13 + step] */
  word(e, slot(control + 1));
  storevalue(e, control + 2);
  again = branch(e, 0);
  linkbranch(e, again, loop);
}

static void entryframe (Emitter *e) {
  emit(e, 0xf3, 0x0f, 0x1e, 0xfa);  /* endbr64 */
  emit(e, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57);
  emit(e, 0x49, 0x89, 0xfc);        /* r12 = frame */
  emit(e, 0x4d, 0x8b, 0xb4, 0x24);  /* mov r14, [r12 + thread] */
  word(e, offsetof(LuaLoopFrame, thread));
  loadbase(e);
}

static void leaveframe (Emitter *e) {
  emit(e, 0x89, 0xd8);  /* eax = exit PC offset */
  emit(e, 0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5b, 0xc3);
}

static void moveorload (Emitter *e, const Proto *p, Instruction ins) {
  unsigned a = GETARG_A(ins);
  switch (GET_OPCODE(ins)) {
    case OP_MOVE: {
      unsigned b = GETARG_B(ins);
      loadvalue(e, b);
      emit(e, 0x41, 0x0f, 0xb6, 0x95);  /* movzx edx, byte [tag] */
      word(e, tagoffset(b));
      storevalue(e, a);
      emit(e, 0x41, 0x88, 0x95);        /* mov byte [tag], dl */
      word(e, tagoffset(a));
      break;
    }
    case OP_LOADI:
      immediate(e, 0, l_castS2U(GETARG_sBx(ins)));
      storevalue(e, a);
      storetag(e, a, LUA_VNUMINT);
      break;
    case OP_LOADTRUE: case OP_LOADFALSE:
      storetag(e, a, GET_OPCODE(ins) == OP_LOADTRUE ? LUA_VTRUE : LUA_VFALSE);
      break;
    case OP_LOADF: {
      double value = cast_num(GETARG_sBx(ins));
      uint64_t bits;
      memcpy(&bits, &value, sizeof(bits));
      immediate(e, 0, bits);
      storevalue(e, a);
      storetag(e, a, LUA_VNUMFLT);
      break;
    }
    default: {
      const TValue *value = &p->k[GETARG_Bx(ins)];
      lua_assert(GET_OPCODE(ins) == OP_LOADK);
      /* The active frame's prototype retains its immutable constants. */
      immediate(e, 0, ttisnil(value) ? 0 : val_(value).bits);
      storevalue(e, a);
      storetag(e, a, rawtt(value));
      break;
    }
  }
}


int luaJ_emit (const Proto *p, const LuaLoopSite *site,
               unsigned char *code, size_t capacity) {
  Emitter e = { .next = code, .end = code + capacity };
  const Instruction *body = p->code + site->pc;
  unsigned pc, last = site->words - 1, control = GETARG_A(body[last]);
  unsigned char *loop;
  _Static_assert(sizeof(lua_Integer) == 8 && sizeof(lua_Number) == 8,
                 "x86 loop primitives implement the 64-bit Lua number model");
  entryframe(&e);
  opcodepc(&e, 0);
  checkintegerloop(&e, control);
  loop = e.next;
  opcodepc(&e, 0);
  checkhooks(&e);
  for (pc = 0; pc <= last; pc++) {
    Instruction ins = body[pc];
    OpCode op = GET_OPCODE(ins);
    unsigned a = GETARG_A(ins);
    /* Some instructions use the same bit positions in a different format. */
    unsigned b = getarg(ins, POS_B, SIZE_B), c = getarg(ins, POS_C, SIZE_C);
    beginstep(&e, pc);
    switch (op) {
      case OP_FORLOOP:
        backedge(&e, control, site->words, loop);
        continue;
      case OP_MOVE: case OP_LOADI: case OP_LOADF: case OP_LOADK:
      case OP_LOADTRUE: case OP_LOADFALSE:
        moveorload(&e, p, ins);
        break;
      case OP_GETFIELD:
        readfield(&e, p, pc, a, b, c, site->bounded);
        continue;
#if !defined(HARDSTACKTESTS)
      case OP_CALL:
        emit(&e, 0x4c, 0x89, 0xe7);  /* rdi = frame */
        argument(&e, 6, a);
        indexarg(&e, 2, pc);
        call(&e, cast(uintptr_t, luaD_tryloopcall), 1);
        loadbase(&e);
        opcodepc(&e, pc + 1);
        exitif(&e, NEGATIVE);
        continue;
#endif
      case OP_GETUPVAL: case OP_GETTABUP:
      case OP_GETTABLE: case OP_GETI:
        readtable(&e, p, op, a, b, c);
        break;
      case OP_SETFIELD:
        writefield(&e, p, site->pc + cast_int(pc), a, b, c);
        continue;  /* charge is part of the call */
      case OP_NEWTABLE:
        charge(&e);
        opcodepc(&e, pc + 2);
        emit(&e, 0x4c, 0x89, 0xe7);  /* rdi = frame */
        indexarg(&e, 6, pc);
        call(&e, cast(uintptr_t, luaV_loopnewtable), 1);
        loadbase(&e);
        pc++;  /* the helper consumes EXTRAARG */
        continue;
      case OP_IDIVK: case OP_MODK:
        divide(&e, op, a, b, l_castS2U(ivalue(&p->k[c])));
        pc++;  /* successful numeric operation skips the metamethod */
        break;
      case OP_ADDI:
        arith(&e, op, a, b, 0, l_castS2U(GETARG_sC(ins)), 1, 0);
        pc++;
        break;
      case OP_ADDK: case OP_SUBK: case OP_MULK:
        arith(&e, op, a, b, 0, val_(&p->k[c]).bits, 1, ttisfloat(&p->k[c]));
        pc++;
        break;
      case OP_ADD: case OP_SUB: case OP_MUL:
        arith(&e, op, a, b, c, 0, 0, 0);
        pc++;
        break;
      default:
        return 0;
    }
    charge(&e);
  }
  for (pc = 0; pc < e.nexits; pc++)
    linkbranch(&e, e.exits[pc], e.next);
  leaveframe(&e);
  return !e.failed;
}

static int readcapture (LuaLoopFrame *frame, TValue *result, unsigned index) {
  UpVal *cell = luaF_capturecell(frame->closure, index);
  if (!luaF_localupval(frame->thread, cell))
    return luaF_tryloadshared(cell, result);
  setobj(frame->thread, result, luaF_privateslot(frame->thread, cell));
  return 1;
}

static int writecapture (LuaLoopFrame *frame, const TValue *value,
                         unsigned index) {
  UpVal *cell = luaF_capturecell(frame->closure, index);
  if (luaF_localupval(frame->thread, cell)) {
    if (!upisopen(cell) || iscollectable(value)) return 0;
    setobj(frame->thread, luaF_privateslot(frame->thread, cell), value);
    return 1;
  }
  return luaF_trystoreinteger(cell, value);
}

static int stringlength (TValue *result, const TValue *value) {
  if (!ttisstring(value)) return 0;
  setivalue(result, cast(lua_Integer, tsslen(tsvalue(value))));
  return 1;
}

typedef struct NativeJump {
  unsigned char *patch;
  unsigned target;
} NativeJump;

/* The call reserves a whole straight-line body's poll budget. Charge actual
** instructions in r15d and publish it when the still-live frame returns. */
int luaJ_emitcall (const Proto *p, unsigned char *code, size_t capacity) {
  Emitter e = { .next = code, .end = code + capacity };
  const unsigned char *labels[LUA_NATIVE_CALL_WORDS] = {NULL};
  NativeJump jumps[3 * LUA_NATIVE_CALL_WORDS];
  unsigned pc, njumps = 0;
  entryframe(&e);
  emit(&e, 0x45, 0x8b, 0xbe);  /* mov r15d, [r14 + poll_remaining] */
  word(&e, offsetof(lua_State, poll_remaining));
  for (pc = 0; pc < cast_uint(p->sizecode); pc++) {
    Instruction ins = p->code[pc];
    OpCode op = GET_OPCODE(ins);
    unsigned a = GETARG_A(ins);
    unsigned b = getarg(ins, POS_B, SIZE_B), c = getarg(ins, POS_C, SIZE_C);
    labels[pc] = e.next;
    opcodepc(&e, pc);
    switch (op) {
      case OP_RETURN0: case OP_RETURN1:
        exitif(&e, 0);  /* call completion uses the ordinary Lua result range */
        continue;
      case OP_MOVE: case OP_LOADI: case OP_LOADF: case OP_LOADK:
        moveorload(&e, p, ins);
        break;
      case OP_GETUPVAL: case OP_SETUPVAL:
        emit(&e, 0x4c, 0x89, 0xe7);  /* rdi = frame */
        argument(&e, 6, a);          /* rsi = register */
        indexarg(&e, 2, b);          /* edx = capture index */
        call(&e, op == OP_GETUPVAL
          ? cast(uintptr_t, readcapture) : cast(uintptr_t, writecapture), 1);
        break;
      case OP_LEN:
        argument(&e, 7, a);          /* rdi = result */
        argument(&e, 6, b);          /* rsi = operand */
        call(&e, cast(uintptr_t, stringlength), 1);
        break;
      case OP_ADD:
        arith(&e, op, a, b, c, 0, 0, 0);
        pc++;
        break;
      case OP_ADDI:
        arith(&e, op, a, b, 0, l_castS2U(GETARG_sC(ins)), 1, 0);
        pc++;
        break;
      case OP_JMP: {
        unsigned target = cast_uint(cast_int(pc) + 1 + GETARG_sJ(ins));
        checkhooks(&e);
        emit(&e, 0x41, 0xff, 0xcf);  /* dec r15d */
        jumps[njumps++] = (NativeJump){branch(&e, 0), target};
        continue;
      }
      case OP_EQK: case OP_TEST: {
        unsigned target = cast_uint(cast_int(pc) + 2 + GETARG_sJ(p->code[pc + 1]));
        unsigned fallthrough = pc + 2;
        unsigned equal = GETARG_k(ins) == (op == OP_EQK) ? target : fallthrough;
        unsigned other = equal == target ? fallthrough : target;
        checkhooks(&e);
        emit(&e, 0x41, 0xff, 0xcf);
        comparetag(&e, a, LUA_VNIL);
        jumps[njumps++] = (NativeJump){branch(&e, EQUAL), equal};
        if (op == OP_TEST) {
          comparetag(&e, a, LUA_VFALSE);
          jumps[njumps++] = (NativeJump){branch(&e, EQUAL), equal};
        }
        jumps[njumps++] = (NativeJump){branch(&e, 0), other};
        pc++;
        continue;
      }
      default: return 0;
    }
    emit(&e, 0x41, 0xff, 0xcf);
  }
  for (pc = 0; pc < njumps; pc++)
    linkbranch(&e, jumps[pc].patch, labels[jumps[pc].target]);
  for (pc = 0; pc < e.nexits; pc++)
    linkbranch(&e, e.exits[pc], e.next);
  emit(&e, 0x45, 0x89, 0xbe);  /* mov [r14 + poll_remaining], r15d */
  word(&e, offsetof(lua_State, poll_remaining));
  leaveframe(&e);
  return !e.failed;
}
