/* Predecoded dispatch families, generated from the same opcode names.
** See Copyright Notice in lua.h. */
#undef vmdispatch
#undef vmbreak
#define vmdispatch(op) goto *disptab[op];
#define vmbreak storeinstructionresults(); vmfetch(); vmdispatch(dispatch)

static const void *const disptab[LUA_DISPATCH_COUNT] = {
#define VM_TARGET(op) [op] = &&L_fast_##op,
#include "lopdispatch.h"
#undef VM_TARGET
#define VM_TARGET(op) [NUM_OPCODES + op] = &&L_cells,
#include "lopdispatch.h"
#undef VM_TARGET
#define VM_PAIR(first,second) \
  [LUA_PAIR_##first##_##second] = &&L_fast_LUA_PAIR_##first##_##second,
#include "loppairs.h"
#undef VM_PAIR
};
