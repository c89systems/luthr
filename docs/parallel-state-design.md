# Parallel shared-VM design investigation

Status: candidate with private shared-VM experiments, 2026-11-15.
The public API audit and scheduler remain unfinished;
no public scheduler API is frozen.

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
objects at operation boundaries, using explicit ownership and conventional
mutex/condition synchronization, with a fixed order for multiple objects. Protect allocator,
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
The allocation experiment below extends the tested paths. Debug-hook signal
behavior, closure mutation and the remaining public API paths still require
their parallel-safety audit. General embedding access remains serialized until
these boundaries and the scheduler are complete.

## Allocating shared-VM experiment

The world coordinator now also owns a short heap-metadata scope, reentrant on
the physical carrier. Allocation callbacks and debt accounting, object-list
insertion, short-string interning/cache updates, open-upvalue registration and
collector barriers use it. It is not retained during ordinary bytecode dispatch
or native binding execution. A collection request first acquires this ownership,
so it cannot interrupt another carrier's partially completed metadata operation.
An allocation failure can collect while retaining its own enclosing metadata
scope; emergency collection preserves the existing no-shrink constraints.

A metadata waiter pins its coroutine, retains its stack gate, and leaves managed
participation before waiting. Its entry resumes atomically with ownership, after
any collection pause. Normal collection does not relocate that waiter's stack:
allocation callers can retain stack pointers, as they could across Lua's existing
emergency-collection path. Stack reallocation acquires metadata ownership before
turning pointers into temporary offsets. Protected/native scope snapshots restore
both collection and metadata ownership, retiring abandoned scopes on errors.
Finalizers suspend both scopes before executing Lua or native callbacks.

Barrier decisions are rechecked under metadata ownership after both referenced
objects have been rooted. GC color reads and allocation-debt checks use atomic
loads; serialized updates use corresponding atomic operations. A mutator barrier
marks an open upvalue gray without reading its foreign running stack. Atomic
collection remarks visited open captures under the rendezvous. Capture writes
and closing acquire metadata ownership before their upvalue mutex, preventing a
mutex holder from waiting behind a collecting carrier.

Long-string hashes are completed after filling contents and before publication,
including concatenation, source/bytecode loading and external strings. Table
lookup now reads immutable hash data. This avoids a shared lazy cache and adds
hashing work for long strings that never become keys; measure that cost alongside
the other synchronization overhead before settling performance policy.

The `parallel_vm_allocate*` cases use two coroutines in one VM on separate usable
logical CPUs, with the same internal-dispatch overlap barrier. Both allocate
independent tables, nested closures, short strings and long keys. Captured table
values are published only after construction. Variants retain a running capture
owner, exercise closing while another coroutine uses its captures, and run with
stopped, incremental and generational collection. The allocator has ordinary
mutable accounting, verifying serialized legacy allocator callbacks under race
checking.
Disposable userdata prove that automatic collection actually runs during active
cases and remains stopped in the stopped case. Their finalizers check released
metadata ownership and exactly-once cleanup, including all remaining objects at
VM close.

The failure case acknowledges a carrier inside its allocator, then waits until
the other carrier is pinned and inactive behind metadata ownership. It rejects
the first carrier's allocation and its emergency-GC retry. With ordinary GC
initially stopped, known garbage must be freed during emergency collection.
A protected call must receive the memory error, then resume ordinary collection;
both coroutines must finish with correct results.
The native-wait case requires an allocating coroutine, with automatic collection
enabled, to finish before releasing the other coroutine's unchanged C binding.
Strict-C89 static/shared SDK tests also check external string key equality and
exactly-once owned-buffer release for empty, short and long strings.

This establishes the exercised concurrent allocation and capture paths. The
shared-container experiment below extends this coverage; it does not establish
arbitrary closure changes, native userdata payloads/module globals, or the
complete public API. No public concurrent embedding contract or measured scaling
claim follows from these fixtures.

## Shared-container experiment

Tables and userdata headers now carry object ownership. Short accesses to the
world mutex arbitrate ownership; the mutex is released during the operation.
Condition waits release managed participation and pin the waiting coroutine and
object, while preserving its stack mapping. Lua register extent is published
before even a formerly nonallocating read can wait. This reuses the existing
rendezvous primitives without allocating an OS mutex/condition per table.
Contention and wakeup costs still require measurement.

Table reads copy complete values under ownership. Writes, array/hash resizing,
length hints, iteration steps, cache invalidation and their collector barriers
use the same protocol in VM and native API paths. A failed fast assignment
returns no reusable slot: its slow path rechecks the key and metamethod. Compound
application operations such as `t.x = t.x + 1` remain multiple operations and
require application synchronization for an atomic update. Iteration does not
promise a snapshot across separate `next` calls while keys are changing.

Metatable discovery roots a snapshot, acquires the object and metatable in
address order, and verifies discovery again before selecting a method. Reversed
and self-metatable references follow the same rule. Method and type-name values
are copied into live native roots; no mutator keeps a borrowed table slot across
an unlock. Set-metatable uses ordered ownership too, including finalizer
registration. Userdata value slots and metatable pointers are protected; opaque
native payloads retain their own resource synchronization requirements.

Object ownership ends before any selected metamethod executes. Native detach
asserts this invariant in Debug builds. Errors release ownership before message
construction can collect or execute a handler, and abandoned scope nodes are
unlinked before longjmp while their native storage still exists. Protected
boundaries retire their roots. Yielded metamethod continuations rely on their
ordinary Lua stack values after native scopes unwind.

The collector bypasses object ownership under world quiescence. This is required
when emergency collection is initiated by an owner while another mutator waits
for that very table. Structural resize also owns heap metadata because mutator
barriers inspect object size. Unrelated object acquisition behind heap ownership
is prohibited; collector quiescence and recursive ownership are explicit cases.

Ten additional `parallel_vm_table_*` cases exercise both coroutines mutating one
table, typed hot-slot replacement, array/hash growth, long and short keys,
deletions, bulk constructors, raw/API access, length and iteration; reversed and
self metatables; userdata values; reentrant, failing and yielding metamethods;
and blocked metamethod/error handlers. The blocked cases require the other
coroutine to finish mutations before the callback is released. A resize-failure
case waits until the other coroutine is demonstrably parked on that table,
forces emergency collection and failed retry, then requires correct recovery
and continued use. The yield case collects between each of six metamethod
continuations. Stopped, incremental and generational collection are covered.

These fixtures extend the private feasibility evidence. The closure experiment
below addresses Lua closure arrays. C captures, debug/hook boundaries,
native/module globals and resource affinity still need their full audit.
Automatic scheduling, migration, carrier reuse, admission limits, compatibility
and performance gates remain required.

## Lua closure and API ownership experiment

Lua closures now have object ownership for their mutable capture arrays.
Selecting a cell copies its identity into a native root while the closure is
protected. The closure scope ends before reading or writing that cell through
its existing value/stack-gate protocol. This avoids retaining a closure lock
while waiting for a foreign coroutine. A concurrent join can change the slot
after selection; an operation already using the selected cell retains that
cell until its own transfer finishes.

VM capture reads/writes, child-closure inheritance and Lua get/setupvalue use
this selection protocol. Joining two closures acquires them in address order,
copies the source cell identity and performs the barrier before releasing
ownership. Self-joins use one scope. The collector continues to read arrays
under quiescence. Partially initialized child closures live in native roots and
are published to their destination register only after their captures are
filled. Debug error-description lookup also snapshots cells and avoids reading
a foreign open stack address while that owner can relocate it.

Five `parallel_vm_join_*` cases repeatedly replace cells using fresh donors,
join in opposite directions, inherit child captures, read/write typed values
through Lua and native APIs, and collect. Error-producing shared closures run
while another coroutine joins their captures. Live-owner cases also require
completion while the owning frame remains open inside an unchanged blocked
native call. Cases cover stopped automatic collection and incremental and
generational modes; explicit collection is still requested by the fixture.

The API audit also removed stack reads after ownership release in
`lua_tolstring`, moved `lua_dump`'s initial stack reads after entry, and copies
the remaining pointer/scalar results in the touched API tails before unlock.
`tests/api_ownership_test.c` injects real collection at the exact native entry
or exit boundary. The collector must move the coroutine's excess stack, and a
fixture allocator clears the retained old allocation. String getters must
return correct bytes/length, and dumped bytecode must reload and return 42.
The tests run on a default-stack worker and verify allocation balance.

This closes the exercised Lua closure-array and API boundary gaps, not the full
native API audit. The next experiment covers C capture and registry access.
Cross-coroutine stack moves/resume, signal/debug-hook settings, and native
resources/module globals remain outstanding. General embedding access still
requires serialization until these boundaries and the scheduler are complete.

## C capture and registry experiment

C closures now use object ownership for their mutable capture values. Each API
index read copies a complete value into a caller-owned native root and retains
that snapshot through its last use. Scalar getters, comparisons, table/userdata
operations and function inspection use these snapshots; helpers retire roots
before leaving managed access. Comparing an index with itself reuses the same
snapshot. Copy/setupvalue writes and their barriers run under capture ownership.
Self-copy validates the index without restoring an earlier snapshot over another
mutator's write. `api_ownership_capture_selfcopy` coordinates two carriers so a
replacement occurs between source selection and copy completion; the new value
must survive. Removing the self-copy guard makes this regression fail.
Registry-value snapshots/replacement use heap metadata ownership, and global
table discovery retains the selected registry while following its globals entry.

Number-to-string conversion rechecks a shared capture under ownership, creates
the string and commits its value/barrier before releasing ownership. Emergency
collection can rendezvous with waiters, as for table resize. Ordinary GC steps
and finalizers execute afterward. Borrowed capture-slot addresses are confined
to the scopes that protect them; no capture lock survives an arbitrary callback
or a complete native invocation. Raw-set key discovery also occurs after API
entry, so it cannot retain a pre-entry stack address.
The `api_ownership_rawset_entry` case forces collection and clears the retired
stack between caller entry and key discovery, then checks the assigned value.

This keeps Lua's existing pointer-lifetime contract: an upvalue string pointer
depends on the active call and unchanged upvalue; retaining a value on the
caller's own stack provides an independent root. This does not promise that a
borrowed pointer survives concurrent removal of its last reference. No new API
or per-call frozen capture view is introduced.
[Lua 5.5 string pointer guarantees](https://www.lua.org/manual/5.5/manual.html#4.1.3)

Ten `parallel_vm_capi_*` cases exercise concurrent C capture copy/setupvalue,
typed scalar getters, same-index comparisons, numeric conversion, raw/ordinary
table operations, userdata headers, Lua capture APIs through pseudo-indices,
and replacing the shared registry during global reads/writes. A conversion OOM
case first proves that another mutator is waiting on the same C closure, then
requires emergency collection, error recovery and subsequent use. A blocked
metamethod must allow replacement of its original capture and collection before
release. A native pointer-retention case roots its copied long string with
ordinary `lua_pushvalue`, then verifies its bytes after capture replacement and
two full collections on the other carrier. Conversion cases drop temporary
references and step collection before reading the string back from its capture.
Stopped automatic, incremental and generational collection are covered.

These tests do not establish the remaining cross-coroutine move/resume and
signal/debug-hook boundaries, shared native callback/configuration state, or
native payload/module-global safety. Scheduling, affinity, carrier limits,
performance, streaming and module compatibility gates remain required before a
public concurrent embedding contract.

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
