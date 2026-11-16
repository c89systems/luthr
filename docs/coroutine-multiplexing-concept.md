# Luthr runtime specification

Status: governing product specification, revised 2026-11-16.

This specification supersedes the earlier incremental synchronization candidate
and its feasibility diary. [Runtime architecture](parallel-state-design.md)
defines the implementation boundaries; [delivery plan](implementation-plan.md)
defines the order and verification depth. Git history retains the superseded work.

## Product and compatibility contract

Luthr is Lua 5.5.1 with shared-VM parallel execution and runtime-owned scheduling,
without a global interpreter lock. Ordinary upstream Lua programs must run with
compatible observable behavior. The compatibility boundary is the language,
standard libraries, interpreter/compiler behavior and supported Lua/native APIs.
It is not Lua's existing implementation of those behaviors.

Object representation, value transfer, capture storage, stack representation,
allocation, memory accounting, garbage collection, barriers, compiler lowering,
interpreter dispatch and scheduler internals may all be replaced. Preserving
Lua's present invariants or internal structures is not a project constraint.
The replacement must establish the invariants needed to deliver the same
observable semantics. An overhaul comparable in scope to free-threaded CPython
is authorized; retaining an implementation merely because it already passes
tests is not a reason to preserve it.

The baseline remains checksum-pinned Lua 5.5.1, owned directly under lua/.
Upstream source and license provenance remain in [lua-source.md](lua-source.md).
No patch application or source acquisition occurs during configure/build.

Compatibility includes values and identity, lexical scope and mutable captures,
metatables, errors and protected calls, coroutines, hooks/debug APIs, weak tables,
userdata, finalization, close/unwind behavior and the public allocator contract.
The incremental/generational collection controls remain compatible public
operations; their underlying algorithms and metadata need not remain upstream's.
Existing Lua bytecode interoperability remains a final compatibility gate.
Concurrent task interleavings are additional behavior absent from upstream;
ordinary serial behavior must not be weakened to implement them.

There is no application-specific Lua subset, downstream-module specialization,
required application rewrite, copied world per task, or hidden serialized mode.
The same portable Lua E2E and benchmark payloads run on pristine upstream Lua and
Luthr. Independent module projects own their integration suites; they are not
runtime build dependencies or implementation targets.

## No global interpreter lock

Independent tasks in one VM must execute ordinary Lua work on separate cores,
including tables, mutable captures, object creation and collection activity.
Arithmetic overlap alone does not establish this requirement.

Ordinary object/value operations must not acquire a VM-wide mutex or compete
for a VM-wide ownership token. Replacing that mutex with a global spinlock,
semaphore, atomic ownership word or common admission gate is the same defect.
Per-operation global accounting must not recreate that serialization indirectly.

Synchronization follows the object, shared cell or actual resource being shared.
Independent objects must not serialize through common ownership bookkeeping.
Immutable reads and owner-local work should use local state where possible.
Multi-step application operations are not implicitly atomic; task-aware
synchronization supplies application coordination.

Global coordination has named, explicit purposes such as collection phase
transitions, batched publication/refill, global work exchange and lifecycle
changes. Those are slow paths with bounded work or a documented reason to wait.
They must not become the default path for each access, allocation, pointer write
or successful safepoint poll. Actual contention on the same object may serialize
that object's operations; it must not stop unrelated objects or runnable tasks.

## Execution and resource model

- A task owns a coroutine's execution state, results/errors, cancellation state,
  roots and logical identity. Tasks share one VM and ordinary object identity.
- A carrier is a reusable OS thread. Tasks outnumber carriers; a permanent
  thread per task is excluded.
- An execution slot carries permission and local resources for managed Lua
  execution: local queues, root publication and allocation/barrier bookkeeping.
  It is distinct from both the task and its current carrier.

Slots permit simultaneous managed execution. Task parking makes capacity
available for other tasks. CPU-bound Lua receives automatic safe scheduling
handoffs without application-authored yields. Migration occurs only at safe
boundaries with complete ownership, stack, roots and error-context transfer.

Managed parallelism, total carriers, occupied native carriers and reserved
callback/progress capacity are separate bounded resources. Defaults account for
usable CPUs, affinity and visible CPU quotas, with explicit overrides. Choose
automatic caps and native compensation from the final workload measurements;
do not freeze them from the rejected prototype or a thread-count guess.

The embedding host can submit work and observe completion without joining a
blocked native activation. Timers, readiness and completion notifications have
a coherent host integration seam; application transports remain outside scope.

## Memory and collection invariants

Every shared value transfer is complete and correctly ordered; a payload and
its type cannot be torn. Storage remains valid until all readers and retained
native users have retired. Pointers into resizable objects cannot survive a
wait, callback or resize without a lifetime guarantee and revalidation.

Captures have one coherent identity and mutation protocol across owner-local
access, foreign access, closing, stack movement, debug mutation and upvalue
joining. Stack-backed captures are optional. Stable shared cells, heap-backed
frames or other representations may replace them throughout the compiler/VM.

Root publication, allocation, barriers and reclamation must be designed
together. Local allocation/publication records and local barrier buffers avoid
a VM-wide metadata transaction per ordinary operation. The collector observes
a coherent published view, with safe-point or epoch coordination as required.
The collector may be replaced; concurrent collection is an implementation
choice, not a requirement to reproduce Go's algorithm literally.

Native roots, parked tasks, callbacks, errors, finalizers and resurrection remain
safe during collection and allocation failure. Finalizers and arbitrary user
callbacks cannot run under a stopped-world ownership gate. The public Lua
allocator contract must be honored; silently bypassing it or assuming an
unchanged custom allocator became thread-safe is not an acceptable shortcut.

## Native calls, callbacks and affinity

Ordinary eligible Lua/C bindings enter conservatively as possibly blocking
native work. Their task, arguments, roots and native activation remain alive on
the carrier while managed capacity becomes available to unrelated tasks.
The runtime must not require a per-library adapter to discover blocking calls.
Native Lua API access and callbacks reenter managed execution under the same
memory and slot protocol as ordinary tasks.

A live opaque C activation remains on its carrier until it unwinds. Runtime-
owned activations may park/migrate when their representation supports it.
Go's movable stacks and compiler support do not establish safety for arbitrary
C frames. Default musl worker stacks and constrained GNU Debug stacks remain
supported; increasing stack sizes is not a correctness repair.

Same-carrier, nested and registered foreign-thread callbacks have explicit
entry, retained context, revocation and acknowledgment. A coroutine cannot run
on two carriers at once. Synchronous callbacks must progress at native
saturation using bounded reserved capacity. Errors and cancellation never
long-jump across carrier stacks.

Native resource serialization and thread affinity are explicit runtime-owned
mechanisms. Create/use/close follows the resource's rule. Runtime memory safety
does not make a foreign library or handle reentrant. Binding-specific policies
remain in the owning projects; generic local fixtures prove runtime guarantees.

## Lifecycle, policy and streaming

Instruction budgets, tracing, errors, hooks and cancellation are task-local or
explicitly shared through a defined policy. Nested calls and callbacks preserve
the correct context without one mutable invocation slot for the whole VM.

Cancellation is a request delivered at safe boundaries. Shutdown closes new
admission and retains roots, results and native owners until acknowledgment.
An uninterruptible native call cannot be safely killed by a coroutine operation.
Late results, callback revocation, close/use races and repeated shutdown have
defined exactly-once cleanup. A timeout does not mean native work has stopped.

Streaming means bounded producer-to-consumer flow with backpressure end to end.
No full-message materialization or undisclosed staging file may hide behind a
streaming API. Chunk and callback ownership survive parking, cancellation,
native saturation and shutdown.

## Products, identity and scope

The product name is luthr: interpreter luthr, compiler luthrc, static/shared
libluthr, CMake package/namespace luthr, pkg-config luthr.pc, and strict C89
headers under include/luthr. Project-owned facade/policy names use luthr_/LUTHR_;
upstream Lua API names and Lua's reported version remain upstream-compatible.
The checkout is /home/mike/g/luthr and the repository is c89systems/luthr.
This pre-release rename is a clean cutover without old-name API shims.

Initial supported targets are x86_64 Linux GNU and musl. Follow the selected
C/CMake lifecycle, keep documentation under docs/, and keep generated outputs
under the checkout's ignored build/ and .cache/. Do not modify sibling projects,
expand architectures, push changes or release without a separate instruction.

## Delivery and acceptance

Implement coherent, substantial surfaces in the [delivery plan](implementation-plan.md).
The implementation phase uses short unit and invariant checks selected for the
surface being changed. These checks are intentionally non-exhaustive. Do not
run long matrices, official suites, full E2E, benchmarks, broad memory/race
sweeps or package certification after every patch or surface.

Once the architecture is integrated and useful, run the final stabilization
phase: exhaustive invariants, upstream differential E2E and official suites,
concurrency/native/lifecycle scenarios, GNU/musl Debug/Release, stack checks,
memory/race tools, performance/sizing, SDK/relocation and final artifact checks.
Deferral changes the order of verification, not the final compatibility or
correctness bar. Focused checks and completed surfaces must not be reported as
proof of the complete runtime. Commit coherent surfaces after their phase-
appropriate checks, and state outstanding final gates explicitly.
