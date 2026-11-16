/* Retained runtime stack transitions. See lua.h for license. */
#define _GNU_SOURCE
#define LUA_CORE
#include "lcontext.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <valgrind.h>

_Static_assert(offsetof(LuaContext, rsp) == 0, "rsp offset");
_Static_assert(offsetof(LuaContext, rbx) == 8, "rbx offset");
_Static_assert(offsetof(LuaContext, rbp) == 16, "rbp offset");
_Static_assert(offsetof(LuaContext, r12) == 24, "r12 offset");
_Static_assert(offsetof(LuaContext, r13) == 32, "r13 offset");
_Static_assert(offsetof(LuaContext, r14) == 40, "r14 offset");
_Static_assert(offsetof(LuaContext, r15) == 48, "r15 offset");
_Static_assert(offsetof(LuaContext, mxcsr) == 56, "mxcsr offset");
_Static_assert(offsetof(LuaContext, control) == 60, "x87 offset");

int luaCT_create(LuaFiber *fiber, void (*entry)(void *), void *argument) {
  long page = sysconf(_SC_PAGESIZE);
  uintptr_t *start;
  if (page <= 0)
    return EINVAL;
  memset(fiber, 0, sizeof(*fiber));
  fiber->stack_size = 128U * 1024U;
  fiber->mapping_size = fiber->stack_size + 2U * (size_t)page;
  fiber->mapping = mmap(NULL, fiber->mapping_size, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (fiber->mapping == MAP_FAILED) {
    fiber->mapping = NULL;
    return errno;
  }
  fiber->stack = (char *)fiber->mapping + page;
  if (mprotect(fiber->stack, fiber->stack_size, PROT_READ | PROT_WRITE) != 0) {
    int error = errno;
    (void)munmap(fiber->mapping, fiber->mapping_size);
    fiber->mapping = NULL;
    return error;
  }
  fiber->entry = entry;
  fiber->argument = argument;
  fiber->native.limit = (uintptr_t)fiber->stack + 32768U;
  fiber->native.execution = &fiber->execution;
  fiber->valgrind_stack =
      VALGRIND_STACK_REGISTER(fiber->stack, fiber->stack + fiber->stack_size);
  start = (uintptr_t *)(fiber->stack + fiber->stack_size - 16U);
  start[0] = (uintptr_t)luaCT_boot;
  start[1] = 0; /* bootstrap never returns */
  fiber->context.rsp = (uintptr_t)start;
  fiber->context.r12 = (uintptr_t)fiber;
  __asm__ volatile("stmxcsr %0" : "=m"(fiber->context.mxcsr));
  __asm__ volatile("fnstcw %0" : "=m"(fiber->context.control));
  return 0;
}

void luaCT_destroy(LuaFiber *fiber) {
  if (fiber->native.coroutine_scopes != NULL)
    abort();
  VALGRIND_STACK_DEREGISTER(fiber->valgrind_stack);
  if (munmap(fiber->mapping, fiber->mapping_size) != 0)
    abort();
  fiber->mapping = NULL;
}

void luaCT_resume(LuaFiber *fiber, LuaContext *caller) {
  NativeStackContext *previous;
  fiber->caller = caller;
  previous = luaE_setnativestackcontext(&fiber->native);
  luaCT_switch(caller, &fiber->context);
  (void)luaE_setnativestackcontext(previous);
}

void luaCT_pause(LuaFiber *fiber) {
  /* No carrier/TLS address may be used after returning on another carrier. */
  luaCT_switch(&fiber->context, fiber->caller);
}

void luaCT_entry(LuaFiber *fiber) {
  fiber->entry(fiber->argument);
  abort();
}
