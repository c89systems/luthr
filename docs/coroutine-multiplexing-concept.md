# Luthrum: coroutine multiplexing and native-call boundaries

Status: conceptual draft for discussion, 2026-11-14.

This records a direction and several implementation ideas. It is not a complete
specification, an approved API, or a claim of implemented behavior.

## Accepted project decisions

- Lua 5.5.1 source lives directly under the repository's top-level `lua/` and is
  modified in place under Git ownership, without a patch series.
- Parallel Lua bytecode execution across CPU cores within the shared VM is
  required. A VM-wide execution lock that serializes Lua is excluded. The
  earlier conditional acceptance of serialized bytecode has been superseded.
  Automatic parking, safe migration, carrier handoff, CPU fairness and bounded
  replacement-thread behavior remain acceptance requirements.
- Implementation follows the C/CMake lifecycle, with documentation under `docs/`.
  Work initially targets x86_64; other architectures remain deferred.
- Initial products are static/shared runtime libraries, a strict C89 SDK, an
  interpreter and a bytecode compiler.
- The current phase modifies only Luthrum and verifies installed SDK consumers.
  Coordinated cutover in cpkt, Vectis and libknurl is a later phase.
- Automatic carrier sizing should start from usable logical CPUs, including HT,
  account for process affinity and Linux CPU quotas, and allow an explicit
  override. The ordinary worker budget, maximum carrier count and compensation
  for blocked native calls are distinct limits. Benchmark pure Lua, native waits
  and mixed workloads before fixing the automatic cap for large vCPU counts.
  Managed execution permits limit parallelism and are distinct from object
  synchronization and carriers occupied by native work.

These decisions supersede the corresponding open questions below. Remaining
runtime mechanisms and public scheduling operations require feasibility evidence.

## Required baseline and module compatibility

Luthrum is based on the latest stable Lua 5.5 release. At this draft's research
date that is **Lua 5.5.1**, as confirmed by the
[official download page](https://www.lua.org/download.html). Record the exact
upstream source and digest for each implementation candidate; advancing to a
later 5.5 maintenance release requires fresh verification.

Support for the existing C89-stack Lua modules is a product requirement. This
includes **curl, MDF/libmdf, Softline, LoneJSON and liblockdc**; the list is
representative, not permission to omit the rest of the supported stack.

Compatibility means preserving documented Lua APIs and ordinary call sites,
native integration, result/error behavior, callbacks, streaming, userdata
ownership, finalizers and cleanup. It must hold when eligible operations run
from scheduled coroutines, including during native waits and callback reentry.
Successful module loading is insufficient evidence. Native module artifacts
must use the same compatible Lua runtime; compatibility with unchanged existing
binaries is a separate ABI question to verify, not an assumption.

The runtime and binding strategy must accommodate these modules' documented
resource serialization and thread-affinity rules. Compatibility does not make
simultaneous use of one native handle valid when its library forbids it.
Preserve legal module usage through affinity, synchronization or explicit
integration metadata, rather than silently dropping capabilities or requiring
an alternative async module namespace. A required module that cannot meet its
contract blocks acceptance of the chosen runtime design.

## Intent

Luthrum should let applications run ordinary Lua functions as concurrent tasks
in one shared Lua VM. Tasks are coroutines multiplexed over OS threads. The
runtime owns scheduling, waiting and native-call transitions. Application code
should not need an async module namespace, callback chains, or a rewritten
version of every operation that might block.

The important unit of execution is the whole function. A function may perform
CPU work, call existing Lua modules, enter a native binding, wait, and continue
with sequential-looking code. An explicit application-authored yield cannot be
the only way other tasks make progress.

The desired effect is that one task waiting inside a native library does not
prevent another runnable task from executing. Luthrum must also address fairness
between tasks running pure Lua, rather than solving only network waits.

One VM per task and one permanently assigned OS thread per task are outside
this direction. A native call can nevertheless occupy an OS thread for its
entire duration. That cost must be accounted for honestly.

## Working execution model

Three runtime objects are useful, without adopting another language's names:

- **Task:** a coroutine, its Lua stack, result/error, cancellation state, roots
  and scheduling identity. Shared tables, functions and globals belong to the
  same VM; spawning does not copy an application's world into another state.
- **Carrier:** an OS thread temporarily executing a task or a native call.
  Carriers are reused and managed within an explicit resource budget.
- **Execution permit:** one of the runtime's available managed-execution slots.
  Multiple permits allow simultaneous Lua execution. Permits limit parallelism;
  they do not replace synchronization for shared VM objects. A carrier blocked
  in foreign code must not monopolize a permit needed by runnable tasks.

A task moves conceptually between runnable, running, waiting, in-native and
terminal states. A native result can be ready before its task is scheduled
again. Cancellation requested and native execution finished are separate facts.

Task creation, waiting for task results, timers and synchronization need public
operations. Their names, syntax, channels and structured lifetime model are
unresolved. A wait must park the task or relinquish runtime execution rights;
it must not hold the interpreter lock while waiting for its own dependency.

Migration is allowed only at proven safe boundaries. An actual foreign C
activation remains on its carrier until that activation unwinds. Audited
runtime C activations can park on retained runtime-owned stacks and migrate
under the proven context protocol. Libraries with stronger thread affinity may
require a resource or an entire task to stay on a particular thread for longer.

A shared-VM execution lock would serialize Lua bytecode and is no longer an
acceptable candidate. Multiplexing, shared-memory correctness and simultaneous
multicore Lua execution must all be proved. Synchronization must protect shared
tables, captured variables, registry/module state, allocation and collection
without retaining a global execution lock across Lua code or native waits.
The implementation candidate retains Lua's tracing collector, with object-scoped
synchronization, explicit roots and coordinated collection pauses. The
[parallel-state investigation](parallel-state-design.md) records the lessons
from free-threaded CPython, private implementation evidence and remaining
acceptance gates. Public scheduler interfaces remain unfrozen.

The CPython precedent is its free-threaded shared-object runtime: replacing
GIL protection with object synchronization, lifetime safety and collection
coordination. It informs shared-heap correctness. Go's task/carrier separation
informs multiplexing and native-call handoff. Both responsibilities must be
satisfied; neither free threading alone nor scheduling alone supplies the other.

## What to borrow from Go

Go separates a goroutine, the OS thread carrying it, and runtime execution
resources. Its scheduler can let another thread execute runnable Go work when
one enters a system call. The useful lesson is separating the logical task from
both its current thread and its execution rights, not copying the scheduler
literally. See the [Go runtime scheduler notes](https://go.dev/src/runtime/HACKING).

Go's C bridge performs a runtime transition before foreign execution. Its call
machinery uses an OS-thread-associated stack and informs the scheduler that
other work may need another thread. Return and callback paths reenter managed
execution through corresponding transitions. This is an explicit boundary, not
automatic discovery that an arbitrary instruction is about to block. See
[Go's C call and callback implementation](https://go.dev/src/runtime/cgocall.go).

Luthrum needs equivalent answers to four questions: who owns the native stack,
who may execute managed code while C runs, how callbacks reenter, and which
objects remain alive. Go's pointer restrictions and explicit pinning illustrate
that foreign memory needs a contract even with a garbage collector. Lua's
contract will differ. See [cgo pointer-passing rules](https://pkg.go.dev/cmd/cgo#hdr-Passing_pointers).

## The Lua-specific opportunity and difficulty

Lua already has interpreter entry/exit lock hooks, disabled by default. Its
current C-function call path unlocks before invoking the C function and locks
again afterward. These are useful investigation points, not proof that enabling
a mutex produces a complete threaded runtime. Every relevant API access and
runtime invariant still needs review. See the [Lua lock hooks](https://www.lua.org/source/5.5/lapi.h.html)
and [C-function dispatch](https://www.lua.org/source/5.5/ldo.c.html).

Lua's coroutine yield uses stack unwinding. An ordinary C activation cannot be
discarded and subsequently resumed as if nothing happened; continuation-aware
API boundaries have special rules. Luthrum must distinguish parking a Lua
continuation from leaving an intact native stack blocked on an OS thread. See
[Lua's C yield contract](https://www.lua.org/manual/5.5/manual.html#4.5).

CPU fairness therefore needs interpreter safe points that preserve stack, GC,
error and hook invariants. Thread replacement handles blocked native calls;
it does not by itself make a pure-Lua loop release the VM execution permit.
Whether safe points return a coroutine to the scheduler or temporarily hand
execution to another carrier while retaining a C stack is unresolved.

## Several approaches to bindings and FFI

Here, a **Lua/C binding** is a C function using the Lua API. A **direct FFI**
invokes a native symbol from a signature description. Both cross the scheduler
boundary, but their access to Lua and their marshaling requirements differ.
Supporting one does not automatically support the other.

### A. Runtime-managed entry for existing Lua/C bindings

Treat entry into an eligible C binding as a conservative native transition.
Keep its task, argument stack and owners rooted, retain its carrier, release
managed execution rights, and make another carrier available for runnable Lua.
Before native return resumes Lua, reacquire the required execution rights.

The C binding may call the Lua API before, during or after its native operation.
Those accesses must obey runtime synchronization. A nested Lua callback requires
a proper managed reentry transition. This is more involved than classifying the
entire binding as code that never touches Lua.

This is the closest route to preserving ordinary handler code and many existing
bindings without per-operation rewrites. Its limit is compatibility: VM locks
do not protect a binding's static variables, make a shared native handle
reentrant, or remove thread-affinity requirements. Unknown bindings need an
explicit policy; unverified shared-state safety cannot be inferred from
successful loading or linking.

### B. Generated adapters from C declarations and annotations

Describe a native ABI plus ownership, callback and affinity rules, and generate
the Lua entry point, marshaling, runtime transitions and result conversion.
Application calls remain ordinary function calls. Generator output, rather
than application code, handles scheduler participation.

A declaration could distinguish ordinary possibly-blocking calls, audited short
leaf calls, callbacks and thread-affine resource operations. The safe scheduling
default is to assume a call can occupy its carrier; the runtime need not predict
the exact blocking syscall.

C signatures alone do not describe pointer retention, borrowed memory lifetimes,
thread safety or callback timing. These require annotations or handwritten
policy. A generic ABI invocation library could be an implementation tool, but
would not supply those policies or the scheduler. No generator or FFI dependency
is selected here.

### C. An explicit native-call contract for binding authors

Offer a supported low-level boundary for bindings that need more control:
prepare and root inputs; enter foreign execution; perform native work; reenter
managed execution; convert results and release owners. Pairing must survive
errors, nested callbacks and cancellation. A plain cleanup statement after a
Lua long jump is insufficient.

An owned-operation variant could separate preparation, native execution and
completion into runtime-managed phases. That can free the coroutine from a
live C activation while a native executor owns the operation. It is useful for
new bindings and callback-heavy libraries, but requires binding work and cannot
be presented as a transparent solution for every unchanged binding.

This is a binding-author interface, not a separate async application API.
Audited leaf-call optimizations can be considered later; incorrect claims that
a call cannot block or reenter must not silently compromise runtime progress.

### D. Affinity-aware dispatch for native resources

Some libraries require create/use/close on the same OS thread, or require all
operations on one resource to be serialized. Represent that affinity explicitly.
Possible mechanisms are a pinned task region or a resource-owned execution
thread reached through a runtime adapter.

This complements the other approaches. It cannot be solved by placing a mutex
around an arbitrary borrowed pointer. It also reduces scheduling freedom and
must count against the thread budget. An existing thread-affine binding may
need adaptation even if the application's handler body remains unchanged.

### Starting hypothesis

Investigate A for existing compatible bindings, C as the explicit escape hatch,
and B as a way to reduce repetitive integration work. D supplies necessary
resource constraints across them. These mechanisms could coexist behind one
ordinary Lua calling convention. This is a research hypothesis, not a settled
compatibility guarantee or a requirement to implement all four immediately.

## Callbacks, ownership and streaming

A callback on the native call's own carrier may use a controlled nested reentry
into its suspended managed context. It must acquire execution rights, preserve
the native activation and contain Lua errors before returning to C.

A callback arriving on a foreign thread needs a registered runtime entry and
retained callback context. Candidate policies are attaching that thread through
a runtime trampoline or delivering a callback task to an allowed carrier. The
same coroutine must never execute concurrently on two threads. Synchronous
callbacks need reserved progress capacity so the caller cannot wait forever for
a callback queued behind itself. Late callbacks after close need a defined
revocation and acknowledgment protocol.

Lua errors must unwind only through the protected boundary on the executing
thread. Neither an error nor cancellation may long-jump into another carrier's
stack. Foreign exceptions and callback failures need translation at their
registered boundary, with exactly-once release of execution rights and roots.

Native-stack affinity and memory ownership must be specified independently.
The runtime must retain tasks, arguments, userdata and callback registrations
until no native call can still use them. A pointer to a Lua object is not a GC
root. Lua's object layout or nonmoving objects do not make arbitrary borrowed
pointers safe. Retaining an owner prevents collection; it does not prevent
another task from explicitly closing or mutating the same resource.

The binding contract must distinguish call-scoped borrowing, explicit retained
handles, owned native buffers and bounded copies. It must also define how
close/use conflicts and callback reentry are rejected or synchronized.

Streaming must preserve producer-to-consumer flow with bounded chunks and
backpressure. A native callback can retain one bounded chunk while awaiting a
managed callback, provided the runtime guarantees that callback can run.
Collecting the whole body or using an undisclosed staging file is not a valid
substitute. Callback yield permission remains an explicit boundary decision.

## Runtime responsibilities that must stay visible

- **Shared-state correctness:** coordinate table mutation, registry access,
  allocation, GC, finalizers and module initialization. Task stacks and native
  roots must remain visible during collection.
- **Application synchronization:** VM memory safety does not make a multi-step
  application operation atomic. Provide synchronization with task-aware waits;
  document what shared-state interleaving applications must expect.
- **Task-local execution state:** errors, tracebacks, instruction budgets,
  cancellation and tracing must not accidentally share one mutable invocation
  slot across concurrent tasks. Embedding policy must survive nested calls.
- **Bounded resources:** distinguish runnable tasks, parked tasks, native calls,
  carriers and retained bytes. Compensating for blocked carriers must not spawn
  threads without a limit. Define native admission and reserved progress before
  claiming that a full pool cannot stall runnable work.
- **Thread and process ownership:** create runtime threads after any embedding
  application's fork boundary. Thread-local library state, stack guards and
  platform-default stack limits are part of the foreign-call contract.
- **Cancellation and shutdown:** request cancellation, stop new admission and
  retain owners until running native work acknowledges completion. An opaque
  uninterruptible C call cannot be safely killed by a coroutine operation.
  Forced process termination, if needed, is an embedding policy.
- **Host integration:** provide a way to submit tasks and observe completions
  without making a host control thread wait on managed execution or join a
  running native call. Timers, I/O readiness and completion wakes need an
  explicit integration seam; application-specific transports remain outside
  Luthrum's scope.

Finite thread budgets cannot guarantee unlimited simultaneous blocking calls.
At saturation, admission, errors and diagnostics must follow a stated policy.
Timeout is not evidence that native work stopped or that a side effect did not
happen. A timed-out task's late completion still needs safe retirement.

## Focused feasibility experiments

Prove the execution model before selecting a public API or attempting a broad
binding migration. Use explicit gates and acknowledgments, rather than sleeps,
to establish ordering.

1. Multiplex many Lua-only tasks over fewer carriers; record carrier identity
   and prove safe migration and fairness without application-authored yields.
   This distinguishes the design from a thread permanently assigned per task.
2. Run Lua tasks simultaneously on separate cores in the same VM. Prove actual
   overlap without serializing the interpreter, including independent mutable
   tables and shared captured variables. Verify shared-state correctness and
   GC coordination; arithmetic-only overlap does not establish the full model.
3. Block an unchanged Lua/C binding behind a native fixture gate. After native
   entry is acknowledged, require another task to finish before opening the
   gate. Verify shared VM identity and bounded replacement-thread behavior.
4. Keep a pure-Lua task CPU-busy while another runs. Prove scheduling fairness
   while preserving instruction limits, errors and protected-call behavior.
5. Exercise same-thread callbacks, nested native calls and callbacks from a
   foreign thread, including errors and callback reentry at capacity.
6. Force GC and allocation failure while tasks are native, waiting or receiving
   callbacks. Check exactly-once resource cleanup and explicit close/use races.
7. Verify thread-affine resource create/use/close and rejection of unsupported
   sharing. Include task cancellation, shutdown and a deliberately stuck call.
8. Withhold the end of a streaming input until downstream progress is observed.
   Prove bounded retained memory and backpressure in both directions.
9. Run behavioral compatibility tests for the C89-stack Lua modules on the
   selected Lua 5.5 baseline. Include curl transfers and callbacks; MDF and
   Softline output/input and owner-thread behavior; LoneJSON incremental
   parsing/serialization and callback ownership; and liblockdc transactions,
   leases, jobs and error/cleanup paths. Check legal concurrent use of independent
   resources and preserved rejection or serialization of conflicting use.

Memory, race and stack checks must cover the actual runtime and binding paths,
not just a scheduler fixture. A successful mutex experiment is evidence for one
mechanism, not certification of arbitrary third-party bindings.

## Decisions deliberately left open

- Runtime modification surface, supported architectures and treatment of
  existing binary modules. Latest stable Lua 5.5 and behavioral support for the
  C89-stack Lua modules are fixed requirements, not open alternatives.
- Coroutine stack representation, safe-point implementation and limits on
  migration in the presence of C activations.
- Native-binding admission categories, default compatibility policy and where
  annotations or generated adapters are necessary.
- Callback attachment, affinity, nested-call and cancellation contracts.
- Task lifetime, join/result semantics, synchronization, thread budgets,
  fairness, observability and host event integration.

The next commitment should be the smallest runtime experiment that can falsify
this concept: parallel shared-VM execution with real shared mutable objects,
coroutine multiplexing, unchanged blocking native work, and continued execution
of another task. The [parallel-state investigation](parallel-state-design.md)
records the initial candidate and the evidence required before committing the
scheduler API. The broader design should follow those experiments.
