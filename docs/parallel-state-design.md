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

The relevant precedent is CPython's free-threaded shared-object runtime from
PEP 703. Its separate per-interpreter-GIL work isolates interpreter state and
does not provide Luthrum's required shared globals and object identity.
[PEP 684](https://peps.python.org/pep-0684/)
Free-threaded CPython became officially supported, while remaining optional,
in Python 3.14; this is not universal removal of the GIL from Python builds.
[PEP 779](https://peps.python.org/pep-0779/)

Free-threaded CPython replaces the GIL's shared-state protection with several
coordinated mechanisms: container/object locks; biased and deferred reference
counting plus immortal objects to reduce accounting contention; coordinated
collection pauses; and attached/detached thread states. Optimistic container
reads also require safe reclamation and allocator support. Removing the
execution mutex alone would leave these responsibilities unsatisfied.
[PEP 703](https://peps.python.org/pep-0703/)

The current implementation also uses quiescent-state-based reclamation (QSBR)
to defer freeing storage that optimistic readers may still access. This is a
lifetime protocol, not merely an atomic-load optimization. It can increase
retained memory. Luthrum's initial object-protected accesses and explicit roots
avoid taking on this additional reclamation mechanism without measured need.
[CPython free-threading memory management](https://docs.python.org/3.14/howto/free-threading-python.html#qsbr-can-delay-freeing-of-memory)

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

Free threading permits multiple OS threads to execute Python simultaneously;
it does not supply Go-style coroutine multiplexing or migrate arbitrary native
C activations. Luthrum therefore needs both the shared-heap safety protocol
informed by CPython and the separate task/carrier/permit protocol informed by
Go below. A native wait must release managed execution capacity even when its
C activation must remain on its original carrier. Object-level safety also
does not make a sequence of application operations an atomic transaction.

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

The world coordinator also owns a short heap-metadata scope, now reentrant on
the logical execution (or carrier for unmanaged entries). Allocation callbacks,
debt accounting, object-list insertion, short-string interning/cache updates,
open-upvalue registration and collector barriers use it. It is not retained
during ordinary bytecode dispatch or native binding execution. A collection
request first acquires this ownership, so it cannot interrupt another
execution's partially completed metadata operation. An allocation failure can
collect while retaining its own enclosing metadata scope; emergency collection
preserves the existing no-shrink constraints.

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

The next experiment addresses cross-coroutine move/resume and callback
configuration. Signal/debug-hook boundaries and native payload/module-global
safety remain separate obligations.

## Cross-state API and callback configuration experiment

Stack moves reserve both coroutine gates in address order before joining
managed execution. A reservation waiting for the second gate does not count as
an active mutator, so the other owner can collect. Neither stack is inspected
until both reservations and managed participation are established. Collection
may relocate an inactive reserved stack in the meantime. The transfer copies
complete values while owning both stacks and ends participation only after
releasing both physical gates. Host handles must remain rooted, as for other
embedding calls.

Resume and close snapshot their parent's native-call depth under the parent
gate before acquiring the target gate. This avoids retaining target execution
participation while waiting for a parent that needs collection. It does not
authorize simultaneously executing two protected/native call chains on the
same coroutine; logical task exclusivity and foreign callback routing remain
scheduler obligations.

Protected error boundaries record their carrier and logical execution. An
unprotected child error can forward to a main-thread handler only when it
belongs to the same execution on the current carrier, inspected under the main
coroutine's gate. Otherwise it uses the configured panic path on the error's
carrier. Attempting to unwind a foreign execution's or carrier's protected
boundary aborts with a diagnostic instead of
performing an invalid cross-thread native-stack jump. Error cleanup pins the
source coroutine through allocating close methods and roots the error while
transferring it. Abandoned scopes and pins retire before panic or forwarding.

Allocator callback/context and warning callback/context are read and replaced
as pairs under heap metadata ownership; panic replacement and selection use
the same ownership. Warning callbacks run after releasing that ownership and
detaching managed execution. This protects configuration, not arbitrary callback
payloads: embedders retain callback contexts until in-flight calls finish and
provide synchronization for their mutable native resources.

Eight `cross_api_*` cases check opposite-direction single/bulk transfers,
incremental/generational collection, exact value identity and finalization,
and parent/move waiters during forced stack relocation. A foreign-carrier error
must reach panic while the unrelated main protected call completes normally.
A same-carrier error forwards through an allocating Lua close method after
all ordinary child references are removed; actual collection must occur and
the child, error and resource must survive until their required cleanup.
Two `callback_config_*` cases require managed overlap, coherent callback/context
pairs and continued allocation/collection while a warning callback is blocked.
Fixtures retain contexts through VM closure and check allocation balance.

The following experiment addresses asynchronous native hook configuration.
Native payload/module-global safety and simultaneous execution of one
coroutine's native frames remain separate obligations.

## Asynchronous hook experiment

Upstream hook installation traverses active call frames to set trap flags. That
walk cannot run concurrently with frame insertion, reuse or collection. Simply
taking the coroutine gate would also deadlock a signal handler that interrupted
the gate's owner: the interpreter installs its interruption hook from a signal.
[Lua's interpreter signal path](https://www.lua.org/source/5.5/lua.c.html#laction)

Hook pointer, mask, base count and remaining count now use sequentially
consistent compiler atomics. Compile-time checks require these accesses to be
lock-free on the selected x86_64 targets. Installation and getters neither
allocate nor wait for a coroutine gate. Remaining-count arithmetic uses unsigned
values, including concurrent resets, to avoid signed overflow. The public count
getter still returns the configured signed integer.
[GCC atomic operations](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html)

Setters no longer inspect or modify call frames. The owning interpreter reads
the mask at branch/call boundaries and its existing bounded instruction polls;
stack relocation still sets frame traps under the existing ownership protocol.
Hook dispatch, finalizer policy and child-coroutine inheritance use atomic reads
too. No global bytecode lock or signal-time allocator/locking path is introduced.

Each configuration component is atomic; installation and separate getters do
not form a transactional settings tuple. As with upstream asynchronous hook
changes, event selection already in progress can overlap replacement, and an
in-flight callback can finish after replacement. Callers retain callback code
and its native resources until such calls finish. This does not make borrowed
activation records valid after their frames return; foreign stack inspection
and multi-call debug-library operations still need task execution ownership.

Six `parallel_hook_*` cases require actual managed overlap while replacing
settings, calling/returning through Lua frames, allocating child coroutines and
collecting in incremental and generational modes. Hooks inspect and modify a
live local and reenter Lua with recursive hooks suppressed. A blocked hook must
allow another coroutine to replace it, allocate and collect before release.
A targeted OS signal installs an error-producing count hook while its target
owns the stack gate; the handler must finish before that owner is released.
Yielding count hooks resume correctly, and inherited positive, zero and negative
count settings retain their public values. Progress handshakes ensure required
callbacks, local modification and inheritance occur before fixture shutdown.
The replacement phase continues through at least eight new child creations.

The next experiment extends these boundaries through the C89 embedding policy.
Shared native payloads/module globals, foreign debug transactions and task
execution ownership remain separate obligations.

## Parallel embedding policy experiment

The C89 facade's instruction counter now consumes its shared budget with atomic
compare/exchange, saturating at zero. Exhaustion remains sticky across protected
calls and coroutine switches; only the next host invocation refreshes it.
Configuration and exhaustion reads use the same atomic protocol. This changes
neither the configured instruction boundary nor the strict C89 declarations.

Line observers execute serially as one native resource per runtime. Waiting for
that resource retains no Lua stack gate, heap ownership or collection
participation. Callback/context selection occurs after acquiring the observer
resource, so a queued event sees a preceding callback's replacement or removal.
A short policy mutex protects configuration and callback-owner identity, and
ends before Lua API calls or user callbacks. Reentry restrictions apply to the
carrier inside the observer; another carrier does not acquire observer privileges
merely because a callback is active. Unobserved work can allocate and collect
while an observer is blocked. Tasks requiring that observer share its backpressure.

Warning callback/context snapshots and replacement use the short policy scope;
warning payload synchronization and in-flight context lifetime remain the
embedder's responsibility. The lazy native coroutine-wrapper cache also uses
that scope. Embedder context is published once in main-thread extra space and
inherited by child creation; coroutine resume/wrap/close no longer rewrite the
same pointer while another carrier can read it.

The allocation-cap experiment exposed auxiliary-library buffer calls outside
the VM allocator protocol. Buffer allocation/resizing/free now rejoin managed
access and select/call the allocator under heap ownership. Buffer bytes retain
their existing GC accounting and ownership transfer into external strings.
External-string header failure also invokes its release callback under heap
ownership. No extra allocator mutex or public allocator API is introduced.
External strings retain their release callback/context until their data is
released; replacing the VM allocator does not shorten that lifetime.

Nine `parallel_policy_*` cases start two genuinely overlapping VM dispatchers
inside one C89 host invocation and join both before that invocation returns.
Budget cases count successful policy-hook returns and require exactly 3,999
accepted instructions for a budget of 4,000 across both coroutines, including
observer replacement/removal. They require exhaustion on both workers and fresh
budget recovery on the next invocation. Other cases cover observer serialization,
blocked-observer progress/collection and foreign setter rejection, concurrent
coroutine wrappers/context reads, warning pairs, and a shared allocation cap
with a legacy allocator that rejects overlapping callbacks. A forced external
string-header failure requires two rejected allocations and exactly-once release
under metadata ownership. Four `synchronization_failure_facade_*` cases require
zero retained memory or synchronization objects after constructor failures,
followed by successful construction/destruction.

These tests do not authorize concurrent host operations on the facade handle or
runtime destruction while tasks remain active. Native payload/module globals,
foreign debug transactions and task execution ownership still require audit.
Scheduling, affinity, carrier limits, performance, streaming and module
compatibility gates remain required before a public concurrent embedding
contract.

## Retained-stack migration experiment

`tests/private/context*` implements a private System V x86_64 stack switch.
It saves the callee-saved registers, stack pointer, MXCSR and x87 control word.
Each task has a 128 KiB stack reservation with inaccessible guard pages at both
ends. Context publication occurs on the outgoing carrier's own stack, after
the complete task context has been saved. Another carrier may resume it only
after taking ownership through the fixture's synchronized runnable set.
[System V x86_64 ABI](https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/x86-64-ABI/low-level-sys-info.tex)

Lua's native-stack context can now be bound to a runtime-owned stack for an
entry, then restored to the carrier's ordinary stack context. The override is
looked up through a function boundary; a task does not retain an address into
an old carrier's TLS. At an eligible poll, the task detaches from managed
execution and releases its Lua stack gate before returning the carrier. Its
native pin and roots remain live while parked. After another carrier resumes
the same retained stack, it rebinds its execution-owned protected error chain
before reacquiring Lua ownership. The native-stack context follows that stack.

The supported C build uses the pinned compiler's nonlocal-goto buffers for Lua's
private protected boundaries, avoiding libc jump-buffer thread encoding. The
throw is explicitly kept out of its receiver's function, as the compiler
requires. These are not replacements for a native binding's own jump buffers.
Foreign C activations remain attached to their carrier.
[GCC nonlocal-goto contract](https://gcc.gnu.org/onlinedocs/gcc/Nonlocal-Gotos.html)

The two `fiber_vm_*` cases run eight task functions on two reused, affinity-bound
carriers in one shared VM. They alternate carriers at every eligible poll,
allocate, share a mutable table and closed capture, and execute without a Lua
yield. Two CPU-bound tasks wait for work queued behind them, making continued
handoff necessary for completion. A managed barrier proves overlapping VM
execution. Before further execution, all tasks park and a forced collection
must relocate every Lua stack; execution and subsequent collection then resume.
An otherwise unreachable userdata on each task must survive through a sole
native root on its retained stack, including after errors unwind inner frames.

Every migration checks independent register sentinels and directly reads both
hardware rounding-control words. Lua errors, allocation failure and native stack
exhaustion must recover through the moved protected boundary. Nested Lua inside
a C binding records skipped migrations and preserves its OS thread/TLS. All
states must remain usable afterward on the host's ordinary stack. Removing
error-owner rebinding makes the fixture abort at the foreign-boundary check.
`fiber_stack_guards` separately requires actual faults at both task-stack guards.
Valgrind client requests register/deregister the test stacks.
[Valgrind stack requests](https://valgrind.org/docs/manual/manual-core-adv.html)

The stack switch and runnable-set driver are test support, not an installed
scheduler or frozen ABI. Two carriers and 128 KiB reservations are fixture
parameters, not the automatic sizing policy. The current proof covers baseline
System V context state, not CET shadow-stack continuation or sanitizer fiber
adapters. The bounded native-call experiment below extends this proof; foreign
callback routing, task lifetime/cancellation, resource affinity, streaming and
module compatibility remain required.

## Bounded native-call experiment

`lua/src/lexec.*` adds a private logical execution context to the retained-stack
binding. It counts managed entries and foreign native activations across all
Lua coroutines entered by that task. Managed admission precedes stack-gate
reservation and world participation; release follows the final world leave and
does not access a possibly reclaimed state. Foreign admission precedes the
actual C activation. This allows parking while admission is unavailable without
migrating a foreign C frame. Function stack locations are saved as offsets across
that admission point because collection can relocate a detached stack.

Ordinary C calls, continuations, hooks, readers, writers and warnings report
native entry/return. Protected errors restore the enclosing execution depth,
including a nested callback which retains its outer native slot. Protected jump
buffers belong to the logical execution as well as the physical carrier; moving
one task updates only its own buffers, including across nested Lua states. A
different task on the same reused carrier cannot take over an error boundary.
`fiber_execution_boundary` tries that transfer between two retained stacks on
one OS thread and requires rejection before a jump into the parked owner.

The C89 instruction-accounting dispatcher is registered by function identity as
runtime-owned code. Counting instructions does not consume a foreign slot. Its
user line observer still requires native admission, before taking the observer
mutex. A removed observer returns before admission even if a stale line mask
remains. Arbitrary replacement hooks retain ordinary foreign-call accounting.

Six `native_scheduler_*` cases use one managed permit, two native-call slots and
three prestarted reusable carriers. These are fixture parameters. A FIFO queue
serves both runnable task entries and pinned native callbacks. A pinned callback
waits for a permit on its existing carrier; a task without foreign activations
parks its retained stack and returns the carrier. Outgoing contexts are fully
saved before publication. Nested native calls reuse the task's admitted slot.

Two unchanged bindings block behind native gates. While both slots remain
occupied, a queued Lua computation and a nested callback from one blocked
binding must finish. The callback resumes a child Lua coroutine, catches a Lua
error, dumps/reloads bytecode, and issues a warning while preserving OS thread
and native TLS. A third task waits for native admission. Observer variants make
that wait occur before an observer mutex; unobserved Lua work must still finish.
Budget variants require instruction accounting to progress at saturation, then
exhaust the aggregate budget after the bindings return. Classifying accounting
as foreign work instead makes the saturation test stall (negative control).

Both GC modes collect while the bindings retain raw userdata pointers, then
check exactly-once finalization after retirement. Native errors, an erroring
foreign hook, and a yielded C continuation must leave no native slots, managed
permits or protected buffers behind. Limits and required parking are asserted.

This is private feasibility support, not an installed scheduler or a measured
default policy. The experiments below add runtime waits and registered callbacks
on provider-owned threads. General external-thread admission and arbitrary
native resource affinity remain open. Allocator callbacks keep their existing
short, nonblocking metadata contract. Cancellation, shutdown, streaming and
backpressure, module acceptance and performance/sizing gates remain.

## Runtime wait parking experiment

Coroutine gates, heap metadata, shared-object access and collection rendezvous
now register task waiters under the same mutex that protects their predicate.
Each registration lives on the task's retained stack and requires no allocation.
The waiter releases managed execution capacity before yielding its carrier.
Notification removes registrations under that mutex and arranges resumption;
notification callbacks cannot enter Lua or switch the notifier's stack. The
driver distinguishes notification before and after outgoing context publication.
It only dispatches a fully saved context, once for each registration.

On resumption, a waiter reacquires execution capacity before taking the predicate
mutex and rechecking its condition. Shared world notifications can be spurious;
the original predicate loops and FIFO coroutine tickets remain authoritative.
Metadata and object waiters retain their existing roots, collection pins and
stack-shrink exclusions. A foreign activation releases its managed permit while
waiting, but waits on its physical carrier; its C stack and TLS do not migrate.
Embedding entries without a task wait handler retain ordinary condition waits.

Logical execution identity now owns coroutine gates, object locks, metadata and
the collection pause. An unmanaged entry uses its physical carrier instead.
This prevents a newly dispatched task on a reused thread from treating another
task's retained lock as recursive ownership, and lets the owner resume on a new
thread. Physical mutexes still protect short bookkeeping operations; ownership
of the protected runtime resource can outlive those mutex acquisitions.

Twelve `native_scheduler_wait_*` cases exercise both GC modes. A single carrier
must run queued Lua computation while earlier tasks wait on a shared object or
coroutine gate behind held metadata. Collection while they wait must preserve
their operation roots and borrowed stack locations, followed by exactly-once
userdata finalization after retirement. Collection-request cases park a real
collector, reuse its carrier for a distinct task and a stopped-world entrant,
then finish collection and unwind through the protected receiver. Forced
migration variants use two carriers and require the waiting owner to change
carriers, including while it owns metadata for collection.

Early-notification cases release metadata and collect before the outgoing task
context has been published, then require continued execution. Two additional
`native_scheduler_resource_*` cases saturate native slots, make a nested native
callback wait for metadata, and require queued Lua computation to finish before
the metadata owner releases it. OS thread/TLS checks remain in the binding.
Restoring physical-thread-only ownership makes the shared-object fixture read
the wrong value; restoring ordinary blocking waits stalls its only carrier.
Both negative controls fail the fixture as intended.

The waiter and driver interfaces remain private. Registered waits cannot yet be
abandoned by cancellation; task and VM lifetimes must retain all queued storage.
The callback experiment below adds a registered entry path. Application and
resource synchronization, streaming, shutdown, compatibility, performance and
installed-API gates remain required.

## Registered foreign-callback experiment

`lua/src/lcallback.*` supplies a private registered boundary for a binding
adapter. A registration retains a Lua function and a dedicated coroutine in
the same VM. It shares tables, closures, captures and globals; it does not copy
the application world. The collector marks registered native roots at cycle
start and atomic remark. Registration storage is caller-owned and must remain
valid until provider acknowledgment and destruction. Creation is protected,
including partial thread/stack allocation and synchronization initialization.

An incoming provider thread attaches a fresh logical execution and uses its
existing native stack. Its supplied scheduler hooks acquire managed permits at
Lua entry; native callbacks and resource waits stay on that thread. It does not
consume another owned carrier or wait for a free owned native-call slot. The
existing native call and provider-owned callback stacks are different resource
categories. This proof does not provide a general external-thread pool or make
unregistered foreign Lua API access supported.

A registration serializes independent invocations. Zero configuration permits
one outer entrant and returns an explicit capacity result for a competitor;
an explicit higher limit permits bounded queuing on provider threads. Nested
entry on the active execution's own carrier shares its outer admission and
uses a nested protected receiver. Foreign nesting through another registration
has its own admission. A provider must not create a dependency cycle through
queued entries on the same registration; a larger queue does not resolve that
cycle. Affinity routing and the final public admission policy remain open.

Argument/result marshalling and Lua invocation run inside a protected boundary
on the executing thread. Errors, allocation failures, native-stack overflow and
to-be-closed failures return a Lua status plus a bounded diagnostic copy. A
synchronous callback cannot yield. Closing its registered coroutine while it
is active returns a runtime error using a preallocated message; it cannot
abandon the registration's lease. Ordinary child-coroutine self-close remains
supported. Successful self-close now restores native execution depth even when
the protected jump reports `LUA_OK`.

Runtime stack descriptors are bound at task-aware gate entry and forgotten on
the final unprotected leave, including for child coroutines used by a callback.
Nested protected frames retain their descriptor. This prevents state from
keeping pointers to temporary foreign-entry storage. Unmanaged embedding retains
lazy physical-thread stack discovery at the existing VM boundary.

Revocation stops new admission and wakes queued callers with a revoked result.
An active invocation may finish; roots remain until it acknowledges completion.
Destruction returns pending without waiting for active or queued calls. Final
retirement enters normal VM synchronization. Before destruction, the native
provider must acknowledge that
no future callback can begin. A revoked registration remains valid for late
calls until that acknowledgment. Destruction is not provider acknowledgment.
The VM must remain alive until every registration has been retired.

Ten `callback_entry_*` cases exercise constructor mutex/condition errors,
allocation failure at thread, stack, message and invocation setup, both GC modes,
arbitrary registration retirement order, protected errors, yield rejection,
nested entry at the default limit, close failures, native-stack exhaustion,
bounded diagnostics, cross-VM rejection and self-revocation. These run on default
worker stacks with an allocator that always moves successful reallocations.
An overflow-handler entry must inherit its caller's reserved stack headroom.

Eight `native_scheduler_foreign_*` cases keep two unchanged bindings blocked at
the native limit while registered foreign callbacks and queued Lua work finish.
They check serialization, overflow admission, same-carrier and foreign nesting,
revocation of queued/late callers, retained roots through collection and exactly
once finalization after acknowledgment. C89 budget variants exercise instruction
accounting during attachment. Open-capture variants update a local on the
blocked task's live Lua stack; it must observe six updates before completion.
All actual foreign invocations preserve OS thread/TLS and acquire managed rights.
Removing registration marking, execution attachment or successful-self-close
depth restoration makes the corresponding negative control fail.

This remains a private binding-author prototype. It does not freeze a public
callback API, promise automatic adaptation of arbitrary modules, or satisfy
cancellation/shutdown, affinity, streaming, compatibility, sizing and installed
scheduler gates. Providers still own their threads and must synchronize their
native payloads and acknowledge unregistration.

## Managed runtime closures and bounded chunk transport

`lua/src/lexec.h` now provides private `luaQ_pushmanaged` for audited runtime C
closures. Its immutable closure flag covers both the initial call and yielded
continuations. Those calls keep the normal coroutine pin/native-stack lifetime
protocol while bypassing foreign-call admission. Ordinary C functions, including
closures with captures, remain conservatively foreign. A managed closure must
not retain carrier TLS across a Lua boundary or perform opaque blocking work.
This classification is private; the owned control-call audit below extends it
to selected library functions and C89 policy wrappers.

The resource-wait boundary also accepts a detached runtime activation with zero
managed-entry depth. It still registers/wakes atomically under the predicate
mutex, parks a task with no foreign activation, preserves a foreign caller's
carrier, and balances any execution permit granted for dispatch. No physical
pipe mutex remains held across a park or VM reentry.

`lua/src/lpipe.c` is a private bounded chunk pipe. Capacity zero selects one
slot; the prototype permits at most 64 slots and 1,024 bytes per string chunk.
Uservalue slots retain immutable strings, and receive transfers the original
string without allocating/copying its body. Full/empty waits apply backpressure.
Close drains queued chunks before EOF; finalization discards queued chunks,
wakes entered operations and defers synchronization-object destruction until
their leases retire. Debug mutation of internal userdata/metatables is outside
the contract; invalid queued values are rejected and the pipe is closed.

Payload retained by one pipe is bounded by its slots times the maximum chunk
size. Each admitted sender can additionally retain its pending input chunk on
its task stack. Task/admission limits must account for that memory separately;
this is no claim that a pipe bounds an unlimited number of producers.

`pipe_contract_*` checks ordinary/managed classification, captures, tail calls,
yielded and protected continuations, errors, chunk identity, empty/maximal and
oversized chunks, close/EOF, explicit finalization and corrupted storage. It
denies allocations throughout a warmed send/receive/close transfer and injects
construction allocation and mutex/condition failures with a moving allocator.
GC cases fill/drain every slot after aging the pipe and reuse the metatable
after all pipe instances have been collected.

Sixteen `native_scheduler_stream_*` cases exercise real VM calls in incremental
and generational GC modes. The producer generates 1 MiB as 1,024 chunks under a
128 KiB hard Lua-heap cap, through a one-slot pipe. Consumers must process the
first chunk before the remaining input is released, with forced collection
while producer and consumers wait. A single reused carrier supports three
waiting consumers and the producer without any foreign slot. Other variants
force two-carrier migration, keep both ordinary native slots blocked until the
stream finishes, or produce from a registered foreign callback while preserving
its carrier/TLS. Explicit finalization and ordinary close wake parked senders
and receivers; a moved sender's error must reach its protected receiver.
Disabling managed classification, metatable-root marking or zero-depth waits
makes the corresponding negative control abort; all changes are restored before
the normal checks.

These gates establish bounded producer-to-consumer chunk flow, not complete
streaming module compatibility or a public channel API. General cancellation,
shutdown, resource affinity, standard-library control-call migration, measured
sizing and installed scheduler/module acceptance remain open. The control-call
work below addresses the standard-library control paths in that list.

## Owned library control calls

Owned base functions now use managed closures, except `print`, `load`, `loadfile`
and `dofile`, whose I/O or reader paths still require their separate audit.
Coroutine-library functions and the closure returned by `coroutine.wrap` are
managed, as are the generated `pairs`/`ipairs` iterators. Actual foreign bindings,
warning handlers and trace observers retain their native transitions. Calling
owned code from a foreign activation preserves the enclosing carrier constraint.
This does not classify the table, string, math, debug, package, I/O and OS
libraries as safe for migration.

The C89 policy now calls validated owned entrypoints declared privately in
`lua/src/lcontrol.h`. It no longer caches arbitrary function pointers from the
first library table it sees. Protected-call/coroutine wrappers and their error
handlers use the same managed classification, while keeping instruction checks
and child-hook propagation. Public SDK interfaces and installed symbols are
unchanged.

Zero-capture runtime functions have one canonical closure per implementation
per VM. A metadata-owned chain roots these constants through collection and
preserves function identity when libraries are reopened. Its implementations
and code must live for the VM's lifetime. Captured closures retain independent
identity and ordinary GC lifetimes. Concurrent construction, collection and
reopening regressions verify those distinctions.

`coroutine.wrap` first copies its mutable capture onto the caller's Lua stack.
That root retains the chosen child before any child API entry or C89 hook
configuration, and remains below the transferred arguments/results. The shared
resume helper uses that snapshot throughout the operation instead of reading
the capture again. A bad replacement capture raises a Lua error.

Four `control_snapshot_*` cases pause immediately after reading the child but
before it starts. Another carrier replaces the wrapper's capture and forces
collection twice. The original child must remain alive, return its original
token/result, and finalize its token exactly once after release. Raw and C89
entrypoints run in both collector modes on default worker stacks.

Sixteen `native_scheduler_stream_control_*` cases wrap the bounded pipeline in
protected calls, error handlers, coroutine creation/resumption/wrapping and
yielded continuations. Fresh nested protected calls remain on the retained C
stack while the pipe parks. Close variants execute the pipeline from a yielded
coroutine's to-be-closed handler. Single-carrier, forced-migration and saturated
native variants require completion without consuming another native slot.
C89 variants also retain instruction accounting while these operations run.
Each pipeline checks the audited pure base operations, iterators, metamethod
callbacks, coroutine introspection and GC operations under the same limits.

Making `pcall` foreign again, reading either wrapper capture before rooting it,
or omitting canonical-function root marking makes its negative control fail.
These changes are restored before normal verification. This proves the tested
control paths; task admission/lifecycle ownership, cancellation, resource
affinity and the remaining libraries/modules still require their own gates.

## Whole coroutine operations

`lua/src/lcoroscope.c` owns each standard-library resume/close operation from
before argument transfer until results and error cleanup have been withdrawn.
An overlapping `resume` returns false without changing the target stack;
`wrap` and `close` raise the existing busy-coroutine errors on their caller.
`coroutine.status` reports an owned target as normal, including the otherwise
ambiguous preparation and result-transfer intervals. Raw resume/close execution
flags also cover internal yield and close-handler intervals.

Scopes use logical native-stack contexts, so ownership survives parking and
migration without retaining a physical mutex. A short world-mutex scope protects
admission, the target owner, source-operation counts and an intrusive GC root
list. That list retains both states independently of application references.
Protected receivers snapshot the context's scope chain. Errors retire abandoned
claims before handlers run and unlink their roots before native storage unwinds;
an inner protected failure preserves surviving outer claims. A held operation
cannot be abandoned by self-close, and a close handler cannot recursively close
the state it is already closing. A reentrant callback cannot self-close an older
state across another state's operation or another execution's protected receiver.
Busy self-close uses a fixed error string without allocating.

Seventy-two `coroutine_operation_*` cases run raw/C89 entrypoints in both GC
modes on default worker stacks. Controlled barriers pause before argument
transfer, after results are ready, before/after close, inside a close handler,
and during raw resume's yield recovery. Competing calls must leave stack values
and status unchanged. The original operation must finish, allow subsequent
legal use and close exactly once. Other cases force collection when an operation
alone owns its child, exercise error handlers that recover or themselves fail,
deny allocation during busy reporting or abandonment, and reject destructive
self-close without allocating. A callback chain through three states rejects
self-close across the middle state's operation even when the original state
owns no operations. Nested VM construction injects allocation errors while
preserving an outer VM's operation.

The existing migrated/saturated control pipelines now exercise these scopes
and assert that completed execution contexts retain none. Removing the claim,
root marking, unwind retirement, nested-boundary preservation or self-close
guard makes its negative control fail; those edits are restored for normal
verification. The callback-chain regression also fails without the self-close
boundary check.

This is ownership for the tested synchronous adapters. The task records below
extend reservation through result collection. Raw host API sequences still
require caller serialization. Cancellation, affinity, module acceptance and
public scheduler lifecycle gates remain outstanding.

## Persistent scheduled-coroutine ownership

`lua/src/ltask.c` adds private task records with lifetimes independent of their
retained call stacks. Admission reserves a fresh/yielded child and binds it to
one logical execution. A world-mutex-protected list roots the coroutine and its
pending outcome until collection. The caller supplies zeroed record/execution
storage, retains a Lua reference until admission and keeps the VM and record
alive through every entered call. Registration needs no Lua allocation.

Only the owning driver can enter the task. It must return from the previous
entry before resuming a yield or finishing a terminal outcome. A task cannot
finish before it has returned, finish a yield or resume its terminal result.
Uncaught errors close the coroutine before publishing completion, retaining the
resulting error/status and running to-be-closed handlers once. Cleanup can park,
enter native code or fail while the record keeps its roots and reservation.
Self-close remains supported within the existing protected-boundary rules.

Standard Lua resume/wrap/close calls reject the reserved coroutine through the
queued, executing, returned, finishing, completed and collecting phases.
Queries from other coroutines report normal while that reservation remains.
The reservation also prevents a foreign callback from borrowing the task's execution descriptor;
registered nested callbacks on the owning carrier retain their normal path.
An atomic execution binding prevents a second task from taking the same identity
before collection. Collection releases that binding for later reuse once all
entered calls have returned and the driver is quiescent. Collection does not
authorize destroying storage still in use by an entered call.

One collector reserves a transfer attempt under the metadata mutex, releases it
before stack growth or value movement and withdraws the roots only after the
outcome reaches the receiving stack. A failed receiving-stack growth restores
the completed phase and leaves the outcome available for retry. Other collectors
receive pending during that attempt and collected after success. Collection
does not wait for an executing task and cannot move values across VMs or into
the task's own stack. Pending/invalid results leave the caller's output fields
unchanged. Raw embedding API sequences still require owner serialization.

Sixty `task_ownership_*` cases use raw/C89 entrypoints, both collectors and
default worker stacks. They force collection with application references dropped
before execution and after the owner pthread exits, preserve target values under
competing Lua calls, cover native and cleanup waits, errors and memory denial,
retry failed collection, serialize competing collectors, reject wrong owners/VMs
and reuse an execution after collection. The scheduler's native/callback and
streaming cases now carry records for their Lua tasks, including migration and
native saturation. Low-level resource probes keep their separate raw-API
contract. The tests fail when ownership, root marking, collector
reservation, error cleanup, retry rollback, return-state validation or foreign
entry exclusion is removed. Those negative edits are restored for final checks.

These interfaces remain private. Public task waiting, cancellation (including tasks
that never started), shutdown, affinity, module acceptance and the public task
API still require their own lifecycle gates. Hosts must collect all admitted
records before destroying their storage or closing the VM.

## Task waiting and parallel execution permits

`luaTK_await` first crosses a balanced managed boundary, releasing capacity that
an initial driver dispatch may already own. It attempts ordinary result
collection, then checks the readiness predicate and registers a waiter under
the same world mutex. Completion, collection and receiving-stack failure notify
the record's waiters. Collection remains exclusive and preserves object identity;
an unsuccessful stack-growth attempt keeps its outcome available to another
waiter or a later retry. Caller status/result fields remain unchanged on an
admission failure. Awaiting one's own execution is rejected, including from a
different Lua child entered by that execution's native callback.

Waiting uses the existing VM condition variable and a per-record logical waiter
list. A scheduled managed caller parks its retained stack through the existing
resource-wait hooks. A caller inside foreign C keeps its physical carrier while
releasing managed execution capacity; a plain host uses the condition-variable
fallback. The caller keeps its receiving coroutine rooted and all entered calls
retain the record's storage. No coroutine gate, collector participation or heap
metadata ownership remains held across the wait. Public cancellation, deadlines,
cyclic dependency handling and shutdown still need their lifecycle contract.

Twenty await variants of `task_ownership_*` check completed outcomes, allocation
denial/retry, waiting behind another collector, plain-host and logical waiters,
and notification when the first collector's stack growth fails. The logical
waiter tests require an explicit notification even if unrelated condition
variable broadcasts occur.

Thirty-six `native_scheduler_task_*` cases combine raw/C89 entrypoints and both
GC modes with two managed permits, two blocked native slots and four owned
carriers. Eight task records retain their Lua states after application references
are dropped. Two carriers select distinct CPUs from the process affinity mask
when available; their observed CPU IDs are printed. A barrier inside VM
safepoints observes two active mutators simultaneously. Forced migration starts
after that observation so the fixture cannot pin both barrier participants to
one carrier. This establishes simultaneous managed participation; measured
throughput and default carrier sizing remain outstanding.

Producers then wait on bounded pipes while other tasks await their outcomes.
An unrelated allocating Lua task completes before either native holder or
producer is released. Forced GC preserves the queued values. Competing awaiters
consume each outcome once; the others observe collection without changing their
outputs. Other cases cover notification before outgoing-context publication,
error cleanup, a same-carrier nested native callback awaiting at saturation and
a driver awaiting before its first Lua entry. Foreign C retains its TLS/carrier;
eligible managed waiters migrate between the two managed carriers.

The driver checks Lua-state invariants before publishing completion, because
another task can collect its final root immediately afterward. It performs no
further Lua-state access before saving its completed context. The tests reject
missing completion/collection/retry notifications, a retained initial execution
permit, callback self-wait and serialized managed capacity. All negative edits
are restored for normal verification. These remain private runtime/driver
interfaces; affinity, module compatibility and the public scheduler remain open.

## Managed math/table helpers and shared random state

Math/table library entries and auxiliary-buffer close/finalizer functions now
use the owned managed-call path. Their audited bodies use local computations,
leaf libm calls and synchronized Lua APIs; Lua callbacks can park their retained
frames. The pinned x86_64 seed generator uses a local address and a time read.
Zero-capture functions keep canonical VM identities when libraries reopen.
Captured random functions retain their original per-open shared-state model.

The PRNG userdata now has a private identity and scoped object ownership.
An invocation snapshots the mutable closure capture under its own guard, then
roots/reserves that userdata independently. Type, size and identity validation
reject missing, replaced or stripped captures before payload access. Initial
draws, rejection sampling and reseeding use one payload reservation without Lua
APIs, allocation or callbacks inside it. Numeric-bound conversions occur first;
invalid random calls still consume their original single draw before reporting
the ordinary error outside the reservation. Invalid explicit seeds leave the
state unchanged. Result values and seed integers are pushed after release.

Thirty-two `math_library_*` cases use raw/C89 entrypoints, both GC modes and
default worker stacks. Literal seed/draw vectors and mixed results were captured
from the verified `ecb61f7` build before replacing its Release library. Error
cases verify advancement, handler reentry and allocation denial/recovery.
Concurrent full/bounded draws compare with a sequential outcome multiset;
concurrent reseeding verifies its returned seeds and exercises synchronization.
A gated invocation reserves the old userdata, makes a peer wait, drops both
function captures and forces collection. The two calls must then receive the
first and second seeded results, and finalization must occur once afterward.

Sixteen `native_scheduler_task_*library*` cases extend the two-managed-permit,
two-blocked-native-slot pipeline. They exercise math entries, table creation,
packing/unpacking, insertion/removal, overlapping moves, sorting and concatenation.
Proxy metamethods and comparators execute Lua. A sort comparator enters math
comparison callbacks that await another task, preserving both C frames through
parking, GC and migration. Large concatenations exercise auxiliary-box cleanup
on success and error while native admission is saturated. Buffer cleanup keeps
the existing VM-serialized allocator path. Uncaught task errors and nested
native callbacks keep the earlier ownership/cleanup requirements.

The gated tests fail when userdata ownership or its root is removed. The
saturation test stops progressing if math, table or buffer cleanup is registered
as foreign; invalid-call advancement fails if validation throws before the draw.
Those six negative edits are restored for final verification. Other libraries,
module compatibility, cancellation/shutdown, affinity, sizing and the public
scheduler still require their own acceptance work.

## Owned UTF-8 helpers

UTF-8 functions and their strict/lax iterators use canonical managed closures.
The iterators carry no mutable shared cursor: the source and position are call
arguments. Decoding is bounded to six bytes and retains no foreign TLS. Long
length, offset, encoding, decoding and continuation-skip loops call the private
`luaQ_work` checkpoint after 1,024 work units. The counter belongs to the C
activation. Checkpoints enter stack ownership only for the existing VM poll;
they hold no object lock and do not charge the Lua bytecode instruction budget.
The interval is an initial fairness bound, not a measured performance policy.

String readers snapshot and root their immutable input independently of stack
slots. Conversion and the protected snapshot are separate steps, so the latter
rechecks the slot before borrowing bytes. Roots are retired on normal return
or by protected unwinding on errors. This protects a scan when native debug
access replaces its argument during a checkpoint. It does not establish a
general contract for arbitrary mutation of C-frame temporaries or auxiliary
buffer internals through debug access; that remains part of the API audit.

Twenty `utf8_library_*` cases cover raw/C89 hosts, both collectors, Unicode
boundaries, strict/lax behavior, Lua 5.5 start/end offsets, errors, reopening
identity and allocation denial/recovery. Snapshot cases remove the only normal
reference to an external input string at a scan checkpoint, force collection
twice, and require its release exactly once after completion or error cleanup.
Twenty `native_scheduler_task_*utf8*` cases exercise every long-scan branch with
two blocked native slots and two managed permits. Each operation must reach a
C-work checkpoint; migration cases must switch carriers inside every helper.
Both producers first park inside length scanning. While their retained C
contexts are saved, collection and an unrelated allocating task must complete
before either scan resumes, with both native bindings still blocked.
Collection occurs with retained arguments, partial results and encoding buffers.
Nested foreign callbacks must remain pinned throughout these same operations.
Errors after migrated partial decoding/encoding exercise protected unwinding
and buffer cleanup. Moving-allocator callback variants relocate every resized
block and require balanced storage at shutdown.
Negative controls fail after removing the source root, disabling work polls,
omitting only the final offset continuation-skip checkpoint, or registering
either the helpers or iterators as foreign. All five edits are restored before
normal verification.

The extended callback stress exposed a stale VM stack base after an allocation
handoff. Another carrier's collection could relocate the stack after the local
GC/trap check, before dispatch resumed. Allocation handoffs now always publish
the saved PC and live stack extent, then reload the base and hook/trap state
after reacquiring ownership. Six `api_ownership_vm_*` regressions collect at the
real unlocked boundary after table creation, concatenation or closure creation,
in both GC modes. A moving allocator poisons the retired stack. All six fail
without the repair and pass with it; this makes the rare stress failure
deterministic without depending on scheduling luck.

## Managed string-pattern helpers

`string.find`, `match`, `gmatch` and `gsub` now use managed runtime closures.
Matching scratch belongs to each invocation. Shared `gmatch` userdata retains
its source and pattern as user values, with short object-ownership scopes for
cursor snapshots and version-checked commits. Matching and result construction
run outside those scopes; a competing consumer retries after a cursor change.
Private roots retain the selected userdata and immutable inputs independently
of debug-replaceable captures or argument slots. Substitution also retains the
replacement and each callback result independently of the caller's stack slots.
UTF-8 helpers reuse the same private rooted-input operations.

Long searches, pattern classes, balanced matches, repetition, backreferences and
replacement copies have bounded work checkpoints. Character classification
returns from its libc TLS-using leaf before a checkpoint can move the frame.
Recursive matching checks the task's native-stack limit, including on default
128 KiB workers. Iterator payload identity and its retained user values are
validated before each snapshot and commit; malformed state raises a Lua error.

`pattern_library_*` verifies Lua behavior, exactly-once concurrent consumption,
GC after argument/capture changes, callback-result retention, allocation failure
and native-stack recovery. `native_scheduler_task_*pattern*` runs the helpers
with two managed permits and two blocked native slots over four carriers. It
requires unrelated progress while both matching frames are saved, collection,
migration inside each helper, callback result waiting, pinned native reentry,
moving allocators and error cleanup.

## Managed string buffers, formatting and packing

The remaining string-table functions except `dump` now use managed closures.
Slicing, reversal, case conversion, repetition and byte conversion checkpoint
bounded byte/result work. Pack/unpack retains the format and each selected
string field, including padding and zero-terminated scans; long format-only
loops also checkpoint. Formatting retains its format, selected objects and
`__tostring` results, and checkpoints literal/quoted output and large copies.
Unpacking and slicing construct their ordinary string results through bounded
copies. These APIs still return complete strings, as their Lua contract requires.

Libc character classification completes inside bounded leaves before the
activation can migrate, keeping locale TLS from crossing a checkpoint. Numeric
format specifications are bounded before copying; numeric conversion uses its
ordinary bounded libc call. Normal, error and allocation-denial paths retire
temporary roots and auxiliary buffers through the existing protected boundaries.

`string_library_*` exercises semantics, invalid inputs, function identity, moving
allocation denial and external-input lifetimes after debug argument replacement.
A formatting callback replaces its caller's arguments and forces collection
before returning an external string; another collection during copying must
retain both the owner and result. `native_scheduler_task_*strings*` requires
migration in each of eleven long helpers, collection with saved contexts,
unrelated progress at native saturation and an `__tostring` callback that awaits
a task result. Native reentry remains carrier-pinned.

## Managed dumping and string-arithmetic callbacks

All string-library functions and arithmetic metamethods now use managed
closures. Dumping explicitly distinguishes the runtime's bounded buffer writer
from public/native writers. The latter retain native admission, carrier pinning,
writer return codes and the final NULL-buffer signal. Serialization roots the
prototype and its private string-index table independently of Lua stack slots;
the public API and string helper also retain their selected closure. The public
writer's stack remains stable throughout the callbacks. The private table is no
longer exposed as an internal stack temporary.

Writer failure now stops producer work before another string-index allocation
can replace its error with an allocation failure or reach a failed alignment
assertion. Recursive dumping checks the executing task/carrier's native-stack
limit. String arithmetic snapshots both operands before numeric conversion or
fallback, and its fallback callbacks can park and migrate without borrowing
replaceable argument slots.

`dump_library_*` compares re-dumped and stripped bytecode, changes arguments
and collects during native and managed writing, checks writer admission and
status propagation, and denies allocations at successive construction points.
Exact full/stripped bytes for a fixed source are also retained from the verified
`b703a61` Release runtime, providing a check independent of the current decoder.
The numeric-conversion test collects after replacing the original operand at
the real public API entry. A near-limit worker fixture requires a Lua stack
error, scope retirement and successful subsequent dumping; it fails when only
the dump-recursion guard is removed. `native_scheduler_task_*dump*` compares
bytes while both native slots are occupied and requires migration inside the
buffer writer, GC with saved contexts, pinned native callbacks and result waits
inside arithmetic fallback callbacks.

The shared numeric parser still contains whole-string scans and libc floating
conversion. Bounded work for arbitrarily long numeric inputs remains unproven;
managed arithmetic callback support does not establish that fairness gate.

## Managed loading and incremental readers

`load` and the owned memory-reader path now use managed execution. A private
reader classification distinguishes memory access and the Lua-reader trampoline
from the public `lua_Reader` callback, which remains native. An actual Lua C
binding invoked by the trampoline also retains its ordinary native boundary.
Foreign activations remain pinned while their nested parsing can release managed
capacity at runtime waits.

Source or reader, chunk name, mode, environment and current reader fragment have
private roots through parsing. Parser anchor tables and temporary anchored
objects also use native roots, and the resulting closure is published only
after its upvalues are initialized. Consumed source bytes and bounded binary
copies checkpoint with GC-visible constructor state. Fixed-buffer address
borrowing preserves its existing caller-owned lifetime contract.

`load_library_*` exercises semantics, errors, allocation denial, explicit native
reader admission and retention after debug argument replacement. The reader
snapshot includes a captured finalizable owner and an external fragment. A
1 MiB comment stream must compile under a 128 KiB Lua-heap cap while retaining
at most two 2 KiB external fragments. Fragments are not concatenated into a full
source buffer; prototypes and individual literal values are still materialized
by the compiler. `native_scheduler_task_*load*` requires native-saturation
progress, migration during text/binary consumption, collection with saved
constructor state, nested loads, callback waits and native pinning.

These checks do not establish bounded work for every parser/metadata pass or
arbitrarily long numeric conversion. File I/O, other libraries, required modules
and the remaining scheduler acceptance gates remain unfinished.

## Managed package orchestration

`require` and the preload searcher now use managed closures. The module name,
C-string lookup key, selected package capture, loaded/searcher tables, loader,
loader data and result use private roots. Capture selection obeys C-closure
ownership, and registry selection obeys heap ownership. Rooted table lookups and
publication preserve metamethod behavior without using mutable debug-visible
stack slots as long-lived ownership. Searcher error fragments are copied in
bounded chunks with work checkpoints; the ordinary final error is materialized.

The name passed to the loader and the C-string key passed to searchers/cache
retain upstream numeric and embedded-NUL behavior. Truthy cache hits return one
value; fresh loads return the module and loader data. Nil and false results,
loader cache assignment, nested loads and error identity retain their ordinary
rules. Lookup, loader execution and cache publication remain separate operations:
concurrent misses can run two loaders and a returned value can reflect another
call's cache publication. No module initialization transaction is introduced.

`package_library_*` covers those semantics, table metamethods, raw function
reconstruction, invalid captures, moving allocation and denial, native admission,
and concurrent misses. A snapshot replaces the active package capture, registry
cache and debug-visible argument during the loader, forces collection and checks
the original objects remain live until their private snapshots retire.
`native_scheduler_task_*package*` requires progress with both native slots held,
bounded-copy migration, saved-context collection, nested loaders, Lua loader
waits and pinned foreign reentry in raw and C89 modes under both collectors.

Actual native loaders and searchers keep ordinary native admission. File and
dynamic-library searchers, `package.searchpath` and `package.loadlib` remain
native. Shared module globals, dynamic-library lifetime/initialization policy,
resource affinity and installed module workflows still require their own gates;
this change does not establish their compatibility or bounded work throughout
every package/core formatting and string operation.

## Managed file execution

`dofile` is now a managed closure. It calls the owned native `loadfile`
implementation with its private filename snapshot, retires temporary roots,
then invokes the loaded chunk with the ordinary yield-aware continuation. File
opening, reading, compilation and closing retain native admission. The Lua chunk
and its callbacks do not inherit that file-loading activation. An enclosing
foreign caller still pins the entire nested operation to its original carrier.

`loadfile` retains filename, mode and explicit environment snapshots privately
through parsing and applies the selected environment after success. Its result
handling is shared with `load`, including an explicitly supplied nil environment.
Public signatures, fixed-buffer rejection and file/error behavior are unchanged.

`file_library_*` covers text/binary files, stdin, BOM/shebang and empty input,
return counts, environments, file/syntax/type/mode errors, yields, close, native
admission, argument replacement and allocation denial with a moving allocator.
The stdio fixture counts opens/closes, including parser failures, and creates
only files under its build directory. Input snapshots retain external filename
and mode strings plus a finalizable environment through collection.
Recursive file execution checks recoverable stack overflow, balanced file cleanup
and a usable VM afterward on default constrained GNU/native musl worker stacks
(an explicitly sized 128 KiB worker in GNU Release).

`native_scheduler_task_*file*` parks inside two actual loaded chunks before
admitting two unchanged blocking bindings. Neither chunk may retain a native
slot. After those slots are occupied, both Lua bodies must continue on two
managed permits over four bounded carriers, with forced migration, collection,
task waits, errors and retained result ownership. Nested native callbacks also
load and execute a file while retaining the original carrier. Raw/C89 and both
collector modes use the default constrained GNU/native musl worker stacks.

This establishes the file-execution boundary, not complete I/O-library/resource
affinity or compiler fairness. Debug/OS/I/O libraries, required native modules
and the remaining scheduler lifecycle/performance gates remain open.

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
