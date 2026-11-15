#ifndef LUTHRUM_TEST_CONTEXT_H
#define LUTHRUM_TEST_CONTEXT_H

#include <lstate.h>
#include <stddef.h>
#include <stdint.h>

/* Initial private System V x86_64 context; offsets are checked in context.c. */
struct test_context {
  uintptr_t rsp, rbx, rbp, r12, r13, r14, r15;
  unsigned mxcsr;
  unsigned short control;
};
struct test_fiber {
  struct test_context context;
  struct test_context *caller;
  NativeStackContext native;
  void *mapping;
  size_t mapping_size;
  char *stack;
  size_t stack_size;
  unsigned valgrind_stack;
  void (*entry)(void *);
  void *argument;
};

int test_fiber_create(struct test_fiber *, void (*)(void *), void *);
void test_fiber_destroy(struct test_fiber *);
void test_fiber_resume(struct test_fiber *, struct test_context *);
void test_fiber_pause(struct test_fiber *);
int test_register_pause(struct test_fiber *);
void test_context_switch(struct test_context *, const struct test_context *);
void test_context_boot(void);
void test_fiber_entry(struct test_fiber *);

#endif
