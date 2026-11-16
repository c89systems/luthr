/* Retained runtime stacks, System V x86_64. See lua.h for license. */
#ifndef lcontext_h
#define lcontext_h

#include "lstate.h"

typedef struct LuaContext {
  uintptr_t rsp, rbx, rbp, r12, r13, r14, r15;
  unsigned mxcsr;
  unsigned short control;
} LuaContext;

typedef struct LuaFiber {
  LuaContext context;
  LuaContext *caller;
  NativeStackContext native;
  LuaExecution execution;
  void *mapping;
  size_t mapping_size;
  char *stack;
  size_t stack_size;
  unsigned valgrind_stack;
  void (*entry)(void *);
  void *argument;
#if defined(__SANITIZE_THREAD__)
  void *sanitizer_context;
  void *sanitizer_caller;
#endif
} LuaFiber;

LUAI_FUNC int luaCT_create (LuaFiber *, void (*)(void *), void *);
LUAI_FUNC void luaCT_destroy (LuaFiber *);
LUAI_FUNC void luaCT_resume (LuaFiber *, LuaContext *);
LUAI_FUNC void luaCT_pause (LuaFiber *);
LUAI_FUNC void luaCT_switch (LuaContext *, const LuaContext *);
LUAI_FUNC void luaCT_boot (void);
LUAI_FUNC l_noret luaCT_entry (LuaFiber *);

#endif
