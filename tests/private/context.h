#ifndef LUTHR_TEST_CONTEXT_H
#define LUTHR_TEST_CONTEXT_H

#include <lcontext.h>

/* The fixtures exercise production context storage and transitions. */
#define test_context LuaContext
#define test_fiber LuaFiber
#define test_fiber_create luaCT_create
#define test_fiber_destroy luaCT_destroy
#define test_fiber_resume luaCT_resume
#define test_fiber_pause luaCT_pause

int test_register_pause(struct test_fiber *);

#endif
