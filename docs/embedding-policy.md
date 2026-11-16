# Embedding and execution policy

## Interfaces and ownership

`<luthr/lua_runtime.h>` owns a VM and supplies synchronous execution, allocation
caps, module registration, warnings and default invocation policy.
`luthr_lua_runtime_state()` borrows its state for the full C89 Lua API in
`<luthr/lua.h>` or the scheduler API in `<luthr/tasks.h>`. Native bindings use
the ordinary Lua headers.

Serialize synchronous host operations on a runtime handle. Scheduler creation,
stack preparation and result collection on the same receiver are also
host-serialized. Task poll, wait and cancel operations may run from other host
threads. Admitted tasks execute concurrently within one VM; the host must stop
accessing an adopted coroutine after successful admission.

A VM outlives its scheduler, task handles and callback registrations. Outcomes
remain rooted until collected, including Lua errors. Collection transfers them
once to a receiver in the same VM, or discards them for a NULL receiver. If
receiver stack growth fails, the outcome remains available for retry. Free task
handles after collection and before freeing the scheduler or VM.

## Invocation policy

Synchronous runs create fresh invocation policy from the facade defaults.
Task options are copied at submission. NULL options snapshot facade defaults,
or unrestricted defaults for a raw VM. Explicit zero fields disable limits;
a NULL trace callback disables observation. Callback and context storage remain
caller-owned for the lifetime of the invocation or registration.

An instruction budget belongs to one invocation and follows its nested
coroutines and same-carrier callbacks across yields. It is not reset by a yield
or shared as a mutable VM-wide task budget. Registered foreign callbacks use
their own invocation policy. Exhaustion reports
`LUTHR_LUA_RUNTIME_ERR_LIMIT`; Lua protected calls cannot repeatedly swallow it
and continue indefinitely. Instruction limits bound VM work, not elapsed time
or arbitrary native execution.

Trace callbacks receive borrowed source bytes and a positive one-based line.
Copy data needed after return. Callbacks must return normally; yielding, Lua
errors and longjmp across the observer boundary are unsupported. Respect the
public facade's reentry restrictions and keep callback/context storage alive
until observation ends. Separate task invocations may invoke observers
concurrently, so shared observer context requires caller synchronization.

## Native work, callbacks and stopping

Blocking native calls keep their live C activation on its original carrier
while releasing managed execution capacity. Native API reentry follows the
runtime's managed ownership protocol. Explicit carrier pins let binding authors
keep create/use/close operations on the required thread; they do not make a
foreign library reentrant.

Foreign callbacks are registered with bounded entry capacity. Revoke a
registration to prevent new calls, obtain provider acknowledgment, then free it
after entered calls retire. The invoke function marshals and calls through Lua
inside a protected boundary and cannot yield across the provider boundary.

Cancellation requests stop at supported boundaries. A timeout or cancellation
does not forcibly interrupt arbitrary native C. Shutdown stops admission and
requests task cancellation; pending native activations must still unwind.
Observe completion, collect outcomes and retire registrations before freeing
the scheduler. A pending result is not proof that cleanup has completed.

Bounded pipes expose Lua `send`, `recv`, `close` and `stats` methods. A chunk
is at most 1024 bytes; zero configured capacity selects one slot. Producers
and consumers make incremental progress with backpressure. Creation can raise
a Lua allocation error and belongs inside a protected Lua boundary.

## Allocation, stacks and module state

The facade allocator owns runtime-managed memory. A zero byte cap means
unrestricted; a nonzero cap applies to physical allocator requests, including
pooled backing pages. This is not a process RSS limit or a cap on memory
allocated independently by native libraries.

Lua-owned recursion checks the current OS thread's stack bounds and reports
exhaustion as a Lua error, including on default musl worker stacks. Native
modules remain responsible for their own stack consumption and cleanup.

Module openers borrow a state and must preserve embedder-owned extra space.
Keep module data in owned userdata or registry keys. Registered Lua source and
C opener records are retained until facade destruction, including replaced
registrations; failed registration releases its new record.

The public headers specify exact status codes, defaults and lifetimes. The
`public_tasks_*`, `native_lifecycle_*`, `lua_contract_*` and
`lua_native_stack_*` tests exercise these contracts across static/shared
libraries and GNU/musl builds.
