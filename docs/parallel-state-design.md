# Parallel shared-VM design investigation

Status: candidate with private execution and GC experiments, 2026-11-15.
The full concurrent heap and scheduler remain unimplemented; no public scheduler
API is frozen.

## Required result

Ordinary Lua functions must run simultaneously across cores in one shared VM,
including functions that allocate, use tables and share captured variables.
Tasks share globals, functions and object identity. A VM-wide interpreter lock,
independent VM per task, copied application world, or permanent OS thread per
task does not meet the requirement. Native waits must preserve progress, with
bounded carriers and explicit native-resource affinity and ownership.

## What free-threaded CPython contributes

CPython's GIL removal combines object locking with changes to reference counting,
allocation and cycle collection. Its design includes coordinated collection
pauses and an attached/detached thread protocol. Optimistic container reads need
additional lifetime and allocator machinery. These mechanisms replace services
the GIL previously supplied; deleting a mutex would not supply them.
[PEP 703](https://peps.python.org/pep-0703/)

Current extension guidance requires care with borrowed references, direct field
access, module globals and blocking native work. Its critical sections may
suspend object locks when blocking, then reacquire them; protected data must be
revalidated afterward. An unmarked extension can re-enable Python's GIL. Luthrum
must audit its bindings without adopting a silent global-lock fallback.
[CPython free-threaded extension guidance](https://docs.python.org/3/howto/free-threading-extensions.html)

Lua already uses tracing collection. Introducing CPython's reference-counting
design would add a second lifetime model. The useful lessons for Lua are scoped
object protection, explicit native attachment, stable roots, and collection
coordination. This is an inference from those mechanisms and the local Lua code.

## What Go contributes

Go separates a logical task, its carrying OS thread, and execution resources.
Parking a task makes its carrier reusable; a blocking foreign call can retain
its thread while relinquishing managed execution capacity. Multiple execution
resources allow parallel managed code.
[Go scheduler notes](https://go.dev/src/runtime/HACKING),
[Go native-call transitions](https://go.dev/src/runtime/cgocall.go)

Go's collector runs concurrently and uses compiler-supported write barriers and
per-execution-resource allocation machinery. Reproducing that whole collector
is a larger initial change than coordinating Lua's existing tracing work.
[Go collector implementation](https://go.dev/src/runtime/mgc.go),
[Go write-barrier implementation](https://go.dev/src/runtime/mbarrier.go)

Go still requires application synchronization for shared data. Its scheduler and
collector do not make arbitrary application accesses race-free. Luthrum must
separately define safe table/value operations and task-aware synchronization for
multi-step application operations.
[Go memory model](https://go.dev/ref/mem)

## Candidate comparison

| Approach | Fit and consequence |
| --- | --- |
| Global interpreter lock | Simpler execution serialization, but fails parallel Lua execution. Excluded. |
| Separate heaps with messages or copied values | Avoids concurrent mutation of one heap, but fails the required shared VM and ordinary object identity. Excluded. |
| Object-scoped synchronization and coordinated GC pauses | Preserves shared objects while allowing independent tasks to execute in parallel. First candidate. Requires a complete mutation, lifetime and reentry protocol. |
| Optimistic or lock-free reads with concurrent collection | Potentially reduces contention and pauses, but adds reclamation, ordering, retry and barrier proof obligations. Consider only where measurements show the simpler candidate cannot meet the workload. |

## First candidate and its limits

Keep coroutine stacks exclusive to their executing task. Protect shared mutable
objects at operation boundaries, starting with ordinary mutexes and explicit
lock order where an operation needs multiple objects. Protect allocator,
interning, registry/module and collector metadata under their own short scopes.
Do not hold a global lock while executing Lua bytecode. Do not hold object locks
across arbitrary callbacks or native waits.

Use a rendezvous protocol for GC work: participating mutators publish stable
roots and reach safe states before the collector accesses their data. Detached
native frames retain explicit owners and roots and must reattach before managed
access. Collection pauses are distinct from a permanent interpreter lock.
Existing incremental/generational modes and barriers require an explicit audit;
pausing collector work alone does not make their shared metadata safe. Run
user finalizers outside a stopped world and account for resurrection and reentry.

This candidate minimizes new mechanisms by keeping Lua's tracing model and
using conventional synchronization. Its costs include object-lock space,
contention, additional safepoints and GC pause latency. It is the current
recommendation, not a claim of optimal measured performance. Start with a
correct implementation; introduce optimistic reads, sharding or concurrent
collection only for demonstrated bottlenecks.

For live captured variables, first investigate preserving Lua's existing
register/stack representation with independently gated coroutine stacks.
Owner-local writes and foreign upvalue reads/writes must use the same gate. A
foreign access must relinquish its own stack gate before waiting on the owner's,
copy a complete value into a GC-visible snapshot, then resume under its own gate.
No thread may hold two coroutine stack gates in opposite orders. Periodic VM
safepoints must allow an owner running a tight loop to service foreign access.
Upvalue closing, stack relocation, closure mutation and GC still need their own
coherent protocol. The private experiment below establishes only the gated
capture transfers, not the complete concurrent VM.

The alternative is to replace stack-backed captures with heap cells and change
all owner-register accesses to use them. That also needs synchronized value
transfers and roots, while adding compiler/interpreter representation work.
The gated-stack experiment tests whether that additional machinery is necessary
before choosing it. Different coroutines must remain able to execute in parallel
regardless of which capture representation is selected.

The private temporary-root mechanism in `lua/src/lroots.h` supplies snapshot
lifetime support. Its storage is caller-owned; collection scans those values,
and protected unwinding retires abandoned scopes. It introduces no execution
lock. Its sequential GC/unwinding tests establish this mechanism only; they do
not prove concurrent VM mutation by themselves.

## Initial gated-stack experiment

`lua/src/lgate.c` gives each coroutine a FIFO ownership gate. Core entry owns
that coroutine's stack; native callbacks release it. VM dispatch publishes its
register extent and saved PC, then hands off ownership every 1,024 instructions.
That interval is experimental, not a measured scheduler policy. A recursive
gate entry is an internal boundary error. Warning callbacks now release ownership
before calling native code, including callbacks that change their own handler.

Lua upvalues have a metadata/value mutex and an open-stack owner. Foreign
transfers root the upvalue, owning coroutine and copied value, release the
caller's gate before waiting for the owner, and re-read the current value under
protection. They release the owner's gate before reacquiring their own. Closing
copies the value and clears its stack owner under the same upvalue mutex. Lua
capture bytecodes and native get/setupvalue use this transfer mechanism. This
does not protect mutation of a closure's upvalue array or C closure captures.

`tests/parallel_vm_test.c` prepares two coroutines in one VM and pins their
carriers to two available logical CPUs when possible. An internal VM poll barrier
requires both dispatchers to arrive while retaining their respective stack
ownership. A VM-wide execution lock would prevent this rendezvous. Each case
then checks observable results:

- Independent preallocated tables with concurrent typed writes/reads of a shared
  closed capture.
- A foreign write to a running owner's captured local, required to terminate
  that owner's tight loop; a separate case uses native get/setupvalue.
- Reversed cross-coroutine capture reads, including an owner's eventual close.
- An unchanged blocking C binding whose release is withheld until the other
  coroutine has completed managed work.

The fixture warms call frames with an explicit initial yield, stops collection,
and aborts on any allocation or free during overlap. Preparation and cleanup are
serialized. Thus it proves neither arbitrary concurrent allocation/collection
nor automatic task scheduling, migration, carrier reuse, replacement limits or
scaling. Shared-table mutation, closure mutation, C API paths and native-global
state still need their full synchronization audit. Public embedding callers
must continue to serialize operations on a VM.

CTest registers these cases under `parallel`; `make racecheck` runs them with
Helgrind and a failing exit status for reported races. `make memcheck` checks
them alongside the synchronous contracts. Separate initialization-failure cases
inject mutex/condition errors during VM, coroutine and upvalue creation, then
check memory/resource balance and continued VM use where construction survived.
The tests run on native GNU and musl workers with the normal stack constraints.
These gates narrow implementation uncertainty without satisfying the remaining
acceptance requirements below.

## Collection rendezvous experiment

`lua/src/lworld.c` tracks managed participation. A coroutine reserves its stack
gate before entering managed execution; waiting for that gate does not count as
an active mutator. Entry waits for an existing collection pause to end. Exit
releases the physical stack gate before dropping participation, so collection
cannot reclaim a state while its final gate access is still in progress. The
world mutex is released during ordinary bytecode execution.

A collection request prevents new managed entries and waits for current ones
to reach a VM poll, detach for native work, or finish their API entry. Competing
collectors park as pinned inactive mutators before retrying. GC steps, full
collections and explicit GC options use this rendezvous. Finalizers suspend the
pause before executing Lua, then reacquire exclusive collection access before
continuing collector work. Native callback boundaries also suspend an enclosing
pause when necessary.

Native calls, continuations, debug hooks, reader/writer callbacks, warnings and
temporary stack handoffs retain a coroutine pin while detached. Collection
marks these pinned coroutines independently of their ordinary Lua references,
including their stack and private snapshot roots. Protected unwinding restores
the enclosing native and collection depths, including successful self-close;
unprotected error forwarding and panic abandon the affected state's scopes.
Ordinary stack/conversion getters now enter managed execution too. Public
handles still need ordinary Lua roots between host calls.

`tests/gc_rendezvous_test.c` removes the ordinary reference to an entered
coroutine and requests collection from another carrier. Explicit acknowledgments
hold a pure-Lua worker inside dispatch until a collection request is pending.
The tests require the worker and its userdata to survive four collections, and
verify exactly-once finalization during cleanup. Native-wait cases require
collection to complete before releasing the unchanged blocking binding.
Competing-request cases initiate a second real collection from the managed
worker while the first request is pending. Both incremental and generational
modes are exercised.

An unrelated Lua finalizer must run outside the collection pause. In managed
cases its native continuation withholds completion until another VM poll proves
worker progress. `tests/world_unwind_test.c` raises an error from a finalizer's
warning callback, abandoning the collector/native chain; an allocation-failure
variant first forces emergency collection inside the finalizer. Both require
retired pins/pauses, exactly-once cleanup and subsequent VM use.
An error-forwarding case injects two real collections between a child's detach
and its parent's entry, with no ordinary reference to the child. Its userdata
error must stay alive until copied to the parent and be finalized exactly once
after the parent releases it.

These remain constrained experiments. Ordinary workers cannot allocate during
overlap; allocation belongs to the collector carrier, or to the competing worker
while it exclusively collects. Finalizer execution is allowed on the collector
carrier. This does not yet establish concurrent allocation, shared-container
mutation, barrier/metadata synchronization or arbitrary native module safety.
Debug-hook signal behavior, closure mutation and the remaining public API paths
still require their parallel-safety audit. General embedding access remains
serialized until these boundaries and the scheduler are complete.

## Lua-specific proof obligations

The following are observed in the owned Lua 5.5.1 source:

- `lua/src/lfunc.c` creates open upvalues pointing into a coroutine's stack.
  `lua/src/lvm.c` reads and writes through those pointers, while the owning
  function also accesses its local registers. Locking only the upvalue accessor
  does not protect ordinary owner-stack writes. Both paths need one coherent
  shared-cell protocol, including stack growth, closing and migration. The exact
  representation must be established by the first prototype.
- `lua/src/lobject.h` represents a Lua value using separate payload and type
  fields. Concurrently copying those fields without synchronization can produce
  an invalid combination. Synchronize complete shared-value transfers and root
  snapshots before another task can replace their original owner.
- Tables have split array/tag storage and resizable hash nodes. VM fast paths,
  C API paths, metatables and cached metamethod flags must obey the same rules.
  A pointer into a table cannot survive an unlock and callback without a lifetime
  guarantee and revalidation.
- `lua/src/lstate.h` holds shared string tables, GC lists, accounting, registry
  and open-upvalue metadata. `lua/src/lmem.c` can initiate emergency collection
  during allocation failure. This can happen inside an object operation and
  must not deadlock against a mutator waiting for that object's lock.
- Lua errors use native-stack unwinding. Critical scopes and native transitions
  need protected-boundary cleanup on the same carrier; a statement after a
  potentially throwing Lua API call is insufficient.

These observations rule out treating a scheduler or a table mutex as the whole
solution. They do not establish that the candidate has already solved them.

## Evidence required before committing the scheduler API

1. A shared-VM prototype must demonstrate simultaneous managed execution on
   separate cores. Synchronization inside ordinary native callbacks is not
   sufficient evidence, since a GIL runtime could release its lock there.
2. Exercise independent mutable tables and genuinely shared captured locals,
   including a capture whose owner is still running. Test stack growth, closure
   closing, collection, allocation failure and task migration while sharing.
3. Exercise metamethod reentry, reversed object-access order, native callbacks,
   error unwinding, finalization and resurrection without leaked locks or roots.
4. Prove unrelated progress during blocked native work and collection requests,
   including saturation and foreign-thread callbacks. Preserve real streaming
   and native-resource serialization/affinity.
5. Run memory and race checks on these actual VM paths on x86_64, with native
   musl workers and constrained GNU Debug workers. Passing a standalone lock
   fixture is not acceptance.
6. Measure single-task overhead, independent-table and shared-table scaling,
   allocation/GC behavior, mixed native/Lua throughput, tail latency, retained
   memory and carrier counts. Use those results to select the automatic worker
   cap and native-compensation budget. Report CPU affinity/quota and effective
   execution capacity alongside results.

The complete compatibility and lifecycle gates remain in the
[concept specification](coroutine-multiplexing-concept.md). This investigation
does not narrow those requirements or authorize dropping a required module.
