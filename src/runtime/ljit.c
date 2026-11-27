/* Loop eligibility, publication and executable storage. See lua.h for license. */
#define LUA_CORE
#include "lprefix.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ljit.h"
#include "lmem.h"
#include "lopcodes.h"

enum {
  JIT_MIN_WORDS = 3,
  JIT_MAX_WORDS = 64,
  JIT_HOT = 64,
  JIT_CODE_LIMIT = 2 * 1024 * 1024
};


static int arithmetic (OpCode op) {
  return op == OP_ADDI || op == OP_ADD || op == OP_SUB || op == OP_MUL ||
         op == OP_ADDK || op == OP_SUBK || op == OP_MULK ||
         op == OP_MODK || op == OP_IDIVK;
}

static int validconstant (const Proto *p, int index, int key) {
  return index < p->sizek && (!key || ttisshrstring(&p->k[index]));
}

/* A straight-line body keeps its real Lua registers and exits at the next
** original instruction on a failed guard. Nested loops and explicit function
** calls stay outside this tier. */
static int loopentry (const Proto *p, int end) {
  Instruction branch = p->code[end];
  int span, start, pc, control;
  if (GET_OPCODE(branch) != OP_FORLOOP || end + 1 >= p->sizecode) return -1;
  span = GETARG_Bx(branch);
  if (span < JIT_MIN_WORDS || span > JIT_MAX_WORDS || span > end + 1)
    return -1;
  start = end + 1 - span;
  control = GETARG_A(branch);
  if (control + 2 >= p->maxstacksize ||
      p->dispatch[start].opcode != cast_uint(GET_OPCODE(p->code[start])))
    return -1;
  for (pc = start; pc < end; pc++) {
    Instruction ins = p->code[pc];
    OpCode op = GET_OPCODE(ins);
    int a = GETARG_A(ins);
    if (a >= p->maxstacksize ||
        (op != OP_SETFIELD && a >= control && a <= control + 2))
      return -1;
    switch (op) {
      case OP_LOADI: case OP_LOADF:
        break;
      case OP_LOADK:
        if (!validconstant(p, GETARG_Bx(ins), 0)) return -1;
        break;
      case OP_MOVE:
        if (GETARG_B(ins) >= p->maxstacksize) return -1;
        break;
      case OP_GETUPVAL: case OP_GETTABUP:
        if (GETARG_B(ins) >= p->sizeupvalues ||
            (op == OP_GETTABUP && !validconstant(p, GETARG_C(ins), 1)))
          return -1;
        break;
      case OP_GETFIELD: case OP_GETI: case OP_GETTABLE:
        if (GETARG_B(ins) >= p->maxstacksize ||
            (op == OP_GETFIELD && !validconstant(p, GETARG_C(ins), 1)) ||
            (op == OP_GETTABLE && GETARG_C(ins) >= p->maxstacksize))
          return -1;
        break;
      case OP_SETFIELD:
        if (!validconstant(p, GETARG_B(ins), 1) ||
            (TESTARG_k(ins) ? !validconstant(p, GETARG_C(ins), 0)
                           : GETARG_C(ins) >= p->maxstacksize))
          return -1;
        break;
      case OP_NEWTABLE:
        if (p->dispatch[pc].opcode != OP_NEWTABLE ||
            pc + 1 >= end || GET_OPCODE(p->code[pc + 1]) != OP_EXTRAARG)
          return -1;
        pc++;
        break;
      default: {
        OpCode fallback;
        int c;
        if (!arithmetic(op) || GETARG_B(ins) >= p->maxstacksize || pc + 1 >= end)
          return -1;
        fallback = op == OP_ADDI ? OP_MMBINI :
                   (op == OP_ADD || op == OP_SUB || op == OP_MUL) ? OP_MMBIN :
                   OP_MMBINK;
        if (GET_OPCODE(p->code[pc + 1]) != fallback) return -1;
        c = GETARG_C(ins);
        if (fallback == OP_MMBIN && c >= p->maxstacksize) return -1;
        if (fallback == OP_MMBINK) {
          const TValue *value;
          if (!validconstant(p, c, 0)) return -1;
          value = &p->k[c];
          if (!ttisnumber(value)) return -1;
          if ((op == OP_IDIVK || op == OP_MODK) &&
              (!ttisinteger(value) || ivalue(value) <= 0))
            return -1;
        }
        pc++;  /* arithmetic reaches this instruction only on a side exit */
      }
    }
  }
  return start;
}

/* This body never parks in a helper. Every forward target names a real
** instruction boundary, so a failed guard can leave a live Lua frame at
** that instruction even after earlier assignments took effect. */
static int callentry (const Proto *p) {
  uint32_t entries = 0, targets = 0;
  int pc, last = p->sizecode - 1, writes = 0;
  if (last < 4 || last >= LUA_NATIVE_CALL_WORDS ||
      (p->flag & (PF_VAHID | PF_VATAB)) ||
      GET_OPCODE(p->code[last]) != OP_RETURN0) return 0;
  for (pc = 0; pc <= last; pc++) {
    Instruction ins = p->code[pc];
    OpCode op = GET_OPCODE(ins);
    int a = GETARG_A(ins), b = getarg(ins, POS_B, SIZE_B);
    int c = getarg(ins, POS_C, SIZE_C);
    entries |= UINT32_C(1) << pc;
    if (op != OP_JMP && op != OP_RETURN0 && a >= p->maxstacksize) return 0;
    switch (op) {
      case OP_RETURN0: case OP_RETURN1: case OP_LOADI: case OP_LOADF:
        break;
      case OP_MOVE: case OP_LEN:
        if (b >= p->maxstacksize) return 0;
        break;
      case OP_LOADK:
        if (!validconstant(p, GETARG_Bx(ins), 0)) return 0;
        break;
      case OP_GETUPVAL: case OP_SETUPVAL:
        if (b >= p->sizeupvalues) return 0;
        writes += op == OP_SETUPVAL;
        break;
      case OP_ADD: case OP_ADDI: {
        OpCode fallback = op == OP_ADD ? OP_MMBIN : OP_MMBINI;
        if (b >= p->maxstacksize || (op == OP_ADD && c >= p->maxstacksize) ||
            pc + 1 >= last || GET_OPCODE(p->code[pc + 1]) != fallback)
          return 0;
        pc++;
        break;
      }
      case OP_EQK: case OP_TEST: case OP_JMP: {
        int target;
        if (op != OP_JMP) {
          if (op == OP_EQK &&
              (!validconstant(p, b, 0) || !ttisnil(&p->k[b]))) return 0;
          if (++pc >= last || GET_OPCODE(p->code[pc]) != OP_JMP) return 0;
          ins = p->code[pc];
        }
        target = pc + 1 + GETARG_sJ(ins);
        if (target <= pc || target > last) return 0;
        targets |= UINT32_C(1) << target;
        break;
      }
      default: return 0;
    }
  }
  return writes != 0 && (targets & ~entries) == 0;
}


void luaJ_lower (lua_State *L, Proto *p) {
  int pc, count = 0;
  LuaLoopSite *sites;
  if (p->sizep != 0) return;  /* a frame with captured locals uses cell dispatch */
  if (callentry(p)) {
    sites = luaM_tryalloc(L, sizeof(*sites));
    if (sites != NULL) {
      *sites = (LuaLoopSite){ .words = cast_uint(p->sizecode) };
      p->nativecall = sites;
    }
    return;
  }
  for (pc = 0; pc < p->sizecode; pc++)
    count += loopentry(p, pc) >= 0;
  if (count == 0 || luaM_testsize(count, sizeof(*sites))) return;
  sites = luaM_tryalloc(L, cast_sizet(count) * sizeof(*sites));
  if (sites == NULL) return;
  count = 0;
  for (pc = 0; pc < p->sizecode; pc++) {
    int start = loopentry(p, pc);
    if (start < 0) continue;
    sites[count++] = (LuaLoopSite){ .pc = start, .words = cast_uint(pc - start + 1) };
    p->dispatch[start].opcode = LUA_NATIVE_LOOP;
  }
  p->loops = sites;
  p->sizeloops = count;
}


LuaLoopSite *luaJ_loopat (const Proto *p, int pc) {
  int low = 0, high = p->sizeloops;
  /* Eligible intervals contain no interior back edge, so their entry and
  ** end orders are the same. */
  while (low < high) {
    int middle = low + (high - low) / 2;
    if (p->loops[middle].pc < pc) low = middle + 1;
    else high = middle;
  }
  lua_assert(low < p->sizeloops && p->loops[low].pc == pc);
  return &p->loops[low];
}


static int reservebytes (global_State *g, size_t bytes) {
  size_t capacity = __atomic_load_n(&g->native_loop_bytes, __ATOMIC_RELAXED);
  do {
    if (bytes > JIT_CODE_LIMIT - capacity) return 0;
  } while (!__atomic_compare_exchange_n(&g->native_loop_bytes, &capacity,
             capacity + bytes, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
  return 1;
}


/* Only whole pages within the allocator's payload become executable. Both
** edge fragments, including any allocator metadata, stay writable. */
static uintptr_t compile (lua_State *L, const Proto *p, LuaLoopSite *site) {
  long systempage = sysconf(_SC_PAGESIZE);
  size_t page, codesize, bytes, estimate = 256;
  unsigned i;
  unsigned char *allocation, *code;
  uintptr_t address;
  if (systempage <= 0 || systempage > JIT_CODE_LIMIT) return LUA_LOOP_UNAVAILABLE;
  page = cast_sizet(systempage);
  /* Field reads include both the direct and native metamethod sequences. */
  for (i = 0; i < site->words; i++)
    estimate += GET_OPCODE(p->code[site->pc + i]) == OP_GETFIELD ? 176u : 112u;
  codesize = ((estimate + page - 1) / page) * page;
  bytes = codesize + page - 1;
  if (!reservebytes(G(L), bytes)) return LUA_LOOP_UNAVAILABLE;
  allocation = luaM_tryalloc(L, bytes);
  if (allocation != NULL) {
    address = cast(uintptr_t, allocation);
    code = allocation + (page - address % page) % page;
    int emitted = site == p->nativecall ? luaJ_emitcall(p, code, codesize)
                                       : luaJ_emit(p, site, code, codesize);
    if (emitted) {
      __builtin___clear_cache(cast_charp(code), cast_charp(code + codesize));
      if (mprotect(code, codesize, PROT_READ | PROT_EXEC) == 0) {
        site->allocation = allocation;
        site->allocsize = bytes;
        site->codesize = codesize;
        return cast(uintptr_t, code);
      }
    }
    luaM_free_(L, allocation, bytes);
  }
  __atomic_fetch_sub(&G(L)->native_loop_bytes, bytes, __ATOMIC_RELAXED);
  return LUA_LOOP_UNAVAILABLE;
}


LuaLoopCode luaJ_prepare (lua_State *L, Proto *p, LuaLoopSite *site) {
  uintptr_t code = __atomic_load_n(&site->entry, __ATOMIC_ACQUIRE);
  if (code == LUA_LOOP_COLD) {
    unsigned hits = __atomic_load_n(&site->hits, __ATOMIC_RELAXED);
    if (hits < JIT_HOT) {
      __atomic_store_n(&site->hits, hits + 1, __ATOMIC_RELAXED);
      return NULL;
    }
    if (__atomic_compare_exchange_n(&site->entry, &code, LUA_LOOP_BUILDING, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      int saved = errno;
      code = compile(L, p, site);
      if (code > LUA_LOOP_UNAVAILABLE)
        __atomic_fetch_add(&p->loopbytes, site->allocsize, __ATOMIC_RELAXED);
      errno = saved;
      __atomic_store_n(&site->entry, code, __ATOMIC_RELEASE);
    }
  }
  return code > LUA_LOOP_UNAVAILABLE ? cast(LuaLoopCode, code) : NULL;
}


static void freenative (lua_State *L, LuaLoopSite *sites, size_t count,
                        int retire) {
  size_t i;
  for (i = 0; i < count; i++) {
    LuaLoopSite *site = &sites[i];
    uintptr_t code = __atomic_load_n(&site->entry, __ATOMIC_ACQUIRE);
    lua_assert(code != LUA_LOOP_BUILDING);  /* a compiling frame keeps its Proto live */
    if (code <= LUA_LOOP_UNAVAILABLE) continue;
    if (retire) luaM_retire_(L, site->allocation, site->allocsize);
    else {
      if (mprotect(cast(void *, code), site->codesize, PROT_READ | PROT_WRITE) != 0)
        abort();  /* return writable storage to the allocator */
      __atomic_fetch_sub(&G(L)->native_loop_bytes, site->allocsize, __ATOMIC_RELAXED);
      luaM_free_(L, site->allocation, site->allocsize);
    }
  }
  if (retire) luaM_retirearray(L, sites, count);
  else luaM_freearray(L, sites, count);
}

void luaJ_freeloops (lua_State *L, Proto *p, int retire) {
  freenative(L, p->loops, cast_sizet(p->sizeloops), retire);
  if (p->nativecall != NULL) freenative(L, p->nativecall, 1, retire);
}
