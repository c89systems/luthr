# Lua runtime execution policy

This describes the extracted synchronous baseline. It has no task scheduler or
internal global execution lock; callers serialize host operations on a handle.
Parallel Lua execution within one shared VM remains required. A private
[gated-stack experiment](parallel-state-design.md#initial-gated-stack-experiment)
and [collection rendezvous](parallel-state-design.md#collection-rendezvous-experiment),
extended by [allocating VM tests](parallel-state-design.md#allocating-shared-vm-experiment),
exercise constrained concurrent execution. Shared-container synchronization and
the public API audit remain unfinished; this does not change the public contract.

The full `<luthr/lua.h>` facade preserves Lua's public C API. The separate
`<luthr/lua_runtime.h>` facade owns a VM and adds embedding policy, including
allocation caps, warning callbacks and an optional instruction limit.

Owned Lua also checks native recursion against the current OS thread's stack
bounds. This covers ordinary non-yieldable protected calls, coroutine resumes,
parser recursion and standard-library callbacks on default musl workers.
Lua-owned native recursion raises a normal Lua error at the stack boundary;
protected calls can catch it and subsequent calls remain usable.
Error handlers receive reserved stack headroom; exhausting that reserve reports
an error in error handling.

Stack bounds are discovered once per OS thread and copied on outer entry, so a
Lua state can move between threads between completed host operations. Worker
stack mappings and the main thread's stack resource limit must remain fixed for
the OS thread's lifetime. On Linux/musl, initial-thread discovery accounts for
permitted stack growth using its resource limit and the preceding mapping;
musl's reported current mapping alone is not its available main stack. Default
and explicit worker stacks, including a forked worker, keep their pthread bounds.
Recursive checks sample the stack pointer and compare a cached limit. They add no
per-protected-call allocation or VM instruction hook. Linux x86_64 builds use TLS descriptors for
the cache, including when Lua is loaded dynamically. Upstream public declarations
are unchanged; callers do not need larger thread stacks or private Lua headers.
Compatibility with unchanged binary modules is not established by these tests.

The owned core has a private temporary-root chain for values held outside Lua
stack slots. These scopes allocate no storage themselves and retain complete
object graphs during collection. Successful protected calls preserve roots in
their caller's live storage; errors and yields restore the enclosing boundary,
and successful self-close also discards unwound scopes. An unprotected throw
abandons that state's scopes before error forwarding or panic. Callers must
remove roots in LIFO order before their storage expires. This is an internal
snapshot mechanism, not a public binding API, object lock or parallel scheduler.
The owning coroutine must remain strongly reachable independently; its private
root-chain field does not itself make that coroutine a collector root.
Separately, native boundaries and temporary handoffs pin their owning coroutine
until reattachment or protected unwind. This protects an entered activation;
host code must still retain ordinary Lua references between operations.
Trusted native callbacks remain responsible for their own stack consumption.
If OS stack bounds cannot be discovered, protected execution fails without
invoking its error handler; discovery is retried on the next outer entry.

With a limit enabled, exhaustion ends the host invocation with
`LUTHR_LUA_RUNTIME_ERR_LIMIT`. Lua `pcall` and `xpcall` cannot repeatedly swallow
that failure. Their normal error results, argument forwarding and yielding
continuations remain native Lua behavior. Each host invocation receives a fresh
instruction budget; after exhaustion, the next invocation can run without
clearing or changing that budget. This is a VM instruction policy, not a wall
clock timeout or a security sandbox; trusted C modules can block, execute native
code or alter VM policy. The counter is shared across all coroutine switches,
including short yields. With a limit enabled, the VM calls the accounting hook
for each instruction; unlimited execution does not install a count hook.
The private parallel-policy experiment also verifies this shared counter across
simultaneously executing coroutines, including observation and fresh-budget
recovery. This does not authorize concurrent host operations on the handle.

`luthr_lua_runtime_set_trace_callback()` installs an optional line observer with
caller-owned context. Each synchronous callback receives a borrowed snapshot of
the source identifier, its byte length and the positive one-based line about to
execute. Source identifiers follow Lua's conventions: `@path`, `=name`, or source
text. Copy needed bytes before returning; source text can contain embedded NUL
bytes. Native C execution and chunks without line information emit no line events.
The observer covers main-thread and coroutine execution, Lua finalizers during
collection and destruction, and Lua close handlers. Keep the callback and context
alive until removal or until runtime destruction completes.

The runtime multiplexes line events with its count hook. Observation neither
consumes an extra instruction budget unit nor replenishes the budget. With no
limit, only line observation is enabled. Passing NULL removes the observer.
Install or remove it between host operations. Changes from warning callbacks or
C module openers during execution or destruction return `LUTHR_LUA_RUNTIME_ERR_ARG`.
Replacement or removal inside the trace callback takes effect on the next event,
including events from another coroutine. Read-only facade calls are allowed
inside an observer. Other operations on the same runtime return
`LUTHR_LUA_RUNTIME_ERR_ARG`, or do nothing for void functions, without changing
the runtime error. This includes execution, instruction-limit changes and free.
Do not access its Lua state through another API from the callback. Callbacks
must return normally; yielding, raising Lua errors and longjmp are unsupported.
Record observer errors in caller-owned context and handle them after the host
call returns. Runtime use remains serialized.

Private parallel execution serializes observers as one native resource while
leaving unrelated unobserved work runnable. Reentry restrictions identify the
carrier inside that observer; another carrier cannot replace the observer
during execution just because an observation is active elsewhere. Warning
configuration is snapshotted separately and does not hold policy ownership
through the user callback. Context lifetime and warning payload synchronization
remain caller responsibilities. See the
[parallel policy experiment](parallel-state-design.md#parallel-embedding-policy-experiment).

Lua error handlers and finalizers are included in this policy. After exhaustion,
the runtime's `xpcall` handler returns the limit error without calling the Lua
handler. Owned Lua has opt-in collector behavior that preserves instruction hooks
during finalizers for the runtime's private hook-mask bit. Ordinary native Lua
hooks keep upstream finalizer suppression. This changes no public types or
symbols; the collector source and shared private header are tracked build inputs.
`luthr_lua_runtime_free()` grants finalizers a fresh configured budget while the
limit remains enabled. Finalizer errors use the warning callback. Clearing the
limit also removes accounting during finalization.

The runtime handle lives in Lua's embedder extra space, outside the registry that
`debug.getregistry()` exposes. Native C module openers borrow a VM state and must
leave that extra space intact. Modules should use their own registry keys or
userdata for module state. `luthr_lua_runtime_context_from_state()` supplies the
embedding context without changing ownership. C89 module callbacks can use
`<luthr/lua.h>` with the borrowed state; upstream headers require C99 or newer.

Coroutine wrappers install the runtime's hook and delegate execution, stack
growth, return/yield transfers and error closure to Lua's own coroutine library.
`coroutine.close` applies the current policy to a suspended thread before its
Lua `<close>` handlers run, including threads created before the limit changed.
Clearing the limit removes the thread's accounting hook on its next close.
An errored wrapped coroutine closes its `<close>` variables before propagating
the resulting error. Wrappers created before a limit change receive the current
policy on their next invocation. Opening standard libraries again reinstalls
these wrappers and the protected-call guards, including after clearing
`package.loaded` entries; repeated opens preserve wrapper identity.

C and Lua module registration writes directly to `package.preload`, bypassing
its `__newindex` metamethod. This prevents an assignment callback from publishing
a Lua loader and then raising an error while its copied source is being freed.
The runtime keeps successful source copies and C opener records until runtime
destruction, including replaced registrations; failed registration releases its
new record. Loaders check upvalue type and membership in their runtime-owned
records before dereferencing data or calling an opener. Substituting foreign
userdata, nil, or a record of the wrong kind raises a Lua error. Restoring a
retained valid record remains safe after garbage collection. Lua loaders
forward all arguments to the source chunk, including the module name and loader
data from `require`. Compilation happens when the loader is invoked.

The bounded `lua_contract_*` tests exercise both static and shared
libraries on default worker threads on x86_64 GNU and musl Linux.
Musl workers use their libc defaults; glibc Debug workers use 128 KiB stacks.
VM creation, execution and destruction all run on the worker. The 10-second
per-case timeout includes thread creation and joining. They cover large value
counts, coroutine error cleanup, protected calls and yields, registry/upvalue
mutation, changed limits, shared budgets, protected error conversion under
allocation failure and recovery, preload publication, replacement and loader
argument forwarding, loader-upvalue substitution, reopened standard libraries
and failed C registration recovery, bounded error handlers, finalization during
collection and destruction, and ordinary native GC-hook behavior. A timeout or
process abort is a failing result.

The `lua_native_stack_*` regressions exercise both library variants with default
musl worker attributes and 128 KiB glibc workers. They cover protected calls,
recursive error handlers, string callbacks, module loading, parser nesting,
coroutine resumes, close handlers, native C callbacks, reentrant C readers and
coroutine reuse after an overflow handler, followed by recovery.
They also cover yielded protected calls resumed at the stack boundary and
successful self-close from an overflow handler, followed by ordinary calls.
Recursive programs run through native protected/coroutine entry and runtime entry
with observation disabled/enabled. All cases use zero or 32 KiB of deliberate
caller stack consumption.
States created on the main thread are reused on successive workers to verify
serialized migration. A GC-only first operation on a migrated state must still
run its pending finalizers. A suspended coroutine can also invoke a metamethod
on its new worker before resuming, then resume on the main thread.
The Linux static test also injects stack-bound discovery failure: execution must
fail without calling the user error handler, then recover when discovery succeeds.
