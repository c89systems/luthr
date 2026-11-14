# Parallel shared-VM design investigation

Status: research recommendation, 2026-11-15. This selects the candidate to
investigate first, not a proven implementation or a frozen public API.

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
