/* Predecoded dispatch families, generated from the same opcode names.
** See Copyright Notice in lua.h. */
#define vmjump() goto *(&&L_cells + dispatch[ip - code].offset)
#define vmbreak vmfetch(); vmjump()

#define DISPATCH_OFFSET(label) (&&label - &&L_cells)
static const int32_t disptab[LUA_DISPATCH_COUNT] = {
#define VM_TARGET(op) [op] = DISPATCH_OFFSET(L_fast_##op),
#include "lopdispatch.h"
#undef VM_TARGET
#define VM_TARGET(op) [NUM_OPCODES + op] = 0,
#include "lopdispatch.h"
#undef VM_TARGET
#define VM_PAIR(first,second) \
  [LUA_PAIR_##first##_##second] = DISPATCH_OFFSET(L_fast_LUA_PAIR_##first##_##second),
#include "loppairs.h"
#undef VM_PAIR
  [LUA_SETRECORD_0] = DISPATCH_OFFSET(L_fast_LUA_SETRECORD_0),
  [LUA_SETRECORD_1] = DISPATCH_OFFSET(L_fast_LUA_SETRECORD_1),
  [LUA_SETRECORD_2] = DISPATCH_OFFSET(L_fast_LUA_SETRECORD_2),
  [LUA_SETRECORD_3] = DISPATCH_OFFSET(L_fast_LUA_SETRECORD_3),
  [LUA_PROJECT_TABLE] = DISPATCH_OFFSET(L_fast_LUA_PROJECT_TABLE),
  [LUA_STRING_RANGE_CALL] = DISPATCH_OFFSET(L_fast_LUA_STRING_RANGE_CALL),
  [LUA_RELOAD_FIELD] = DISPATCH_OFFSET(L_fast_LUA_RELOAD_FIELD),
  [LUA_RELOAD_INDEX] = DISPATCH_OFFSET(L_fast_LUA_RELOAD_INDEX),
  [LUA_RELOAD_KEY] = DISPATCH_OFFSET(L_fast_LUA_RELOAD_KEY),
  [LUA_NATIVE_LOOP] = DISPATCH_OFFSET(L_fast_LUA_NATIVE_LOOP),
};
#undef DISPATCH_OFFSET
