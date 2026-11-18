# Luthr runtime architecture

Status: replacement architecture boundaries, 2026-11-17.
The [runtime specification](coroutine-multiplexing-concept.md) governs behavior.
This design replaces the earlier preference for preserving Lua's collector,
stack-backed captures and central ownership bookkeeping. Internal compatibility
with that prototype is not required.

## Starting state and architectural rejection

The existing implementation contains useful behavior tests, native boundary
fixtures, SDK tooling and compatibility inputs. It is not an accepted no-GIL
architecture. Object entry/exit in laccess.c takes world.mutex, heap metadata
uses one global owner, and allocation, capture writes and barriers participate
in that protocol. Independent objects therefore contend through shared state.
Arithmetic overlap and green serial tests do not validate this architecture.

The fixed-host-pool measurements at commit 782baee demonstrated the defect:
integer work scaled, while independent tables, shared tables, captures and
allocation degraded with more workers. The profile also shows substantial
ownership and mutex cost without contention. These are baseline findings,
not measurements of a finished scheduler or permission to retain the protocol.
Pre-rename evidence is retained under ignored .cache/pre-overhaul-build/;
the former design diary is in Git history. Fresh builds use the moved checkout.

Do not repair the design one mutex at a time while leaving the surrounding
global metadata transaction intact. Replace ownership, publication, allocation,
barriers and scheduler participation as coherent connected surfaces. Keep one
implementation after each cutover; no legacy path or refactoring feature flag.

## Shared-memory cutover in progress

Mutable objects now carry an atomic execution-owner token and recursion depth.
Uncontended acquisition/release touches the object; only real contention enters
the shared wait queue. Tokens identify logical executions across carrier movement
and permanent thread-local storage for unmanaged callers. Waiter publication and
the release-side observer check order notification against the predicate recheck.
Concurrent scanners use nonblocking object ownership. Global boundaries publish
roots and change collection phases.

Lua captures now store their complete value in a stable shared cell throughout
their lifetime. A live binding records a stack offset, never a pointer into its
owner's registers. Operand access refreshes only captured registers that an
instruction reads, and publishes its actual writes. A conservative frame filter
avoids cell-list searches for unrelated registers. Call-result movement, yielded
operations and debug local access use the same cell representation. Closing
unlinks a binding; it does not copy an obsolete register shadow over a foreign
writer's value.
Bytecode remains portable; the interpreter lowers its register accesses.
Prototypes with captured locals carry one immutable dispatch byte per instruction;
other prototypes need no dispatch metadata. Only instructions that can access
captured registers enter the cell-aware helper; ordinary dispatch uses registers
directly. Both use one set of instruction definitions and execute the original
bytecode, keeping program locations, dumping and fixed-buffer loading unchanged.
Root publication traces cells as bindings and uses the suspended instruction to
retain only register shadows still needed as operands or partial results. An
obsolete shadow cannot keep a replaced value alive while its owner is parked.

Pointer barriers atomically claim gray objects and publish them on the current
coroutine's work queues. Each object uses its dedicated trace link; repeated
claims need no additional storage. Publication cannot allocate, wait or request
a global rendezvous. Collection drains every queue before tracing or reclaiming
objects, including queues on otherwise unreachable threads. Parallel scanners
publish private work before parking; weak processing joins them at phase
boundaries. Automatic requests and allocation assists return after a marking
slice; VM safepoints advance the same job even without further allocation.
Unpaid marking work survives an unavailable batch. Allocation assists share
capacity with physical reclamation and drain marking when its allocation
runway is spent. Explicit full collections and emergency retries drain it.
Ordinary logical sweep classification and physical destruction also overlap
application workers. Sweepers publish bounded batches before yielding, and
cycle boundaries join their work under quiescence.
Major marking rotates a three-state stamp; young marking has independent parity.
Sweepers own disjoint list ranges and splice survivors back as ranges, leaving
ordinary major survivors' headers and interior allocation links untouched.
Majors selected for generational execution stamp objects and new allocations old
during marking. The quiescent transition enables remembered barriers before
young allocation resumes, avoiding a separate conversion or promotion scan.

Allocation consumes owner-local page lists and batches atomic remote returns.
Nonempty caches retain their pages across ordinary collection. Slotless task
drivers share one task cache across nested coroutines. Changed pages form finite
shared cleanup jobs: bounded batches borrow idle caches and retire empty pages
without stopping workers. A first remote return pins a page until its notice is
claimed; publication is the producer's last page access. Destruction completion
drains these jobs; refills assist abandoned pages while retaining warm caches.
Emergency retries recall clean caches;
shutdown drains the complete directory. Cold chunks start small and grow with
demand instead of reserving an OS-page-sized chunk per cold size class.
Pacing counts live storage, including runtime records and black allocations,
separately from pending destruction. Physical completion preserves the cycle's
fixed allocation goal. Allocations after marking consume the following
allowance without enlarging its baseline until the next cycle.

Backing allocation and page handoff, logical sweep/finalizer classification,
general metadata scopes and managed admission still need further scaling work.
Capture access and operation/root metadata remain part of the performance gate.

## State ownership boundaries

| Owner | State and responsibility |
| --- | --- |
| VM | Shared object identity, immutable program data, interned symbols, global lifecycle and collection phase coordination. Shared mutable services are partitioned or reached through explicit slow paths. |
| Execution slot | Local runnable work, allocation/publication records, barrier/remembered work, stable root publication and managed participation. A carrier borrows this resource while executing Lua. |
| Task | Frames/coroutine state, logical roots, errors, hooks, budgets, cancellation, result and wait registrations. Migration transfers the task's ownership coherently. |
| Object/shared cell | Mutable contents, version/lifetime state and the synchronization required by actual sharing. Independent objects have independent ownership. |
| Carrier/native scope | Physical stack, opaque C activations, thread-local foreign state and explicit affinity reservations. |

Do not use slot ownership as a renamed interpreter lock. Multiple slots execute
ordinary Lua work simultaneously. Do not attach all slot-local state permanently
to OS threads; native blocking and task migration separate those lifetimes.

## Integrated memory surface

Replace the existing global object ownership protocol throughout object/value
operations, Lua API helpers, captures and standard-library operations. Complete
value snapshots and safe storage reclamation are part of the protocol, not a
follow-up optimization. Per-object locks, atomics, versioned access and reader
epochs are available mechanisms; select them with a clear ownership proof.

Captured variables may move to stable shared cells. All owner and foreign
access, compiler lowering, debug mutation, joining, close and stack movement
must agree on that representation. Preserving direct pointers into an owner's
register array is not required. Do not merely protect one accessor while
owner-local writes bypass the same synchronization.

Allocation accounting, new-object publication, interning and barriers must
avoid one global transaction per ordinary operation. Use slot-local state,
partitioned services and batched publication/refill as appropriate. Shared
backing allocation may have a slow-path lock; this must not become a lock held
through object construction, ordinary writes or arbitrary callbacks.
Honor the public allocator's ownership and failure contract.

Collection operates on a coherently published graph and root set. The design
must state when tasks publish, when mutation may proceed, how pending barriers
are drained, and when storage becomes reclaimable. Global phase changes may
coordinate participants; a successful no-request poll stays local/read-only.
Collection work, weak references, ephemerons, finalization, resurrection and
emergency collection must fit this model. Preserve their observable Lua behavior
while freely replacing lists, mark metadata, algorithms and storage layouts.

No arbitrary user code executes while the runtime holds a global phase gate or
an unrelated object's ownership. Errors unwind roots and ownership on the
executing carrier. Retained snapshots, not borrowed slots, cross waits and
callbacks unless lifetime and revalidation are explicitly guaranteed.

## Execution and scheduling surface

Implement the task/slot/carrier state machine as production runtime machinery.
Use local work queues and bounded work exchange rather than central scheduling
on each instruction or object access. Managed admission is separate from
native occupancy. An idle or parked task does not reserve a permanent carrier.

Safepoints preserve a complete task state and service fairness, collection,
budgets and cancellation through one coherent protocol. No-request polling
must not acquire the VM-wide ownership mutex. Park/wake registration is ordered
with its predicate, including notification before context publication. Native
or affinity-pinned scopes constrain migration explicitly.

Resource limits are distinct and observable: managed slots, total carriers,
native admission and reserved progress. Finite limits require stated saturation
behavior. Keep provisional policies private until the final workload results
support public defaults; implement the mechanism before tuning its constants.

## Native, callback and lifecycle surface

Conservative entry into unchanged Lua/C bindings retains the activation and
roots, releases managed capacity and allows unrelated Lua to execute. Lua API
calls and callbacks reattach under the same slot/memory protocol. Synchronous
callback progress must remain possible with native slots occupied.

Native affinity, registered callbacks, native-root ownership, protected error
translation and revocation have explicit lifetime state. Shutdown/cancellation
retire waits and notifier borrows before releasing their owners. In-flight
opaque native work remains alive until completion even after cancellation.
Streaming transports retain only bounded chunks and preserve backpressure.

The C89 policy facade, raw Lua API, CLI and standard libraries must all use the
same runtime. Do not let a wrapper silently restore global serialization or
keep a second implementation with different semantics.

## Lessons from the downloaded Go source

Go 1.27.4 source was downloaded to the ignored research cache. The official
source archive digest is
73cfb8571aec0c01177687db87163d46aa20e9779672772891aa17654d9a6099.
Go is a design reference, not a build dependency or a language-semantics target.

- [proc.go](https://go.dev/src/runtime/proc.go) and
  [runtime notes](https://go.dev/src/runtime/HACKING) separate tasks, carriers
  and per-P execution resources, with local queues and work exchange.
- [malloc.go](https://go.dev/src/runtime/malloc.go) uses per-P allocation caches
  and amortized shared refill, rather than a shared lock per small allocation.
- [mwbbuf.go](https://go.dev/src/runtime/mwbbuf.go) buffers barrier work per P;
  [mgc.go](https://go.dev/src/runtime/mgc.go) coordinates collection phases.
- [cgocall.go](https://go.dev/src/runtime/cgocall.go) releases managed execution
  capacity across C execution and reacquires it for callbacks/return.
- The compiler's escape, closure and write-barrier passes cooperate with the
  runtime. Luthr may likewise change its compiler, interpreter and representation
  together. Existing Lua internal conventions are not an architectural limit.

Go's ordinary maps require caller synchronization and its native stack/pointer
rules differ from Lua's. Preserve Lua's public behavior while borrowing the
distributed state and explicit ownership principles. Do not assume Go's exact
collector or map implementation can be transplanted without a Lua-specific proof.

## Focused implementation invariants

Short deterministic checks must falsify the architecture, not merely exercise it:

1. Independent table/value/capture operations do not acquire or update a VM-wide
   ownership gate. Test actual VM/API paths, including no-request polling.
2. Shared operations transfer complete values, retain storage and publish roots
   correctly across resize, capture mutation, task movement and collection.
3. Publication/refill/barrier boundaries preserve the graph and allocator failure
   contract without an ordinary-operation global metadata transaction.
4. Many tasks reuse fewer carriers; an unchanged blocked binding does not stop
   unrelated managed work, including a callback at native saturation.
5. Park/wake, cancellation, error unwind and close retire each owner exactly once.

During the implementation phase, select a small relevant sample of these checks
and unit tests per coherent surface. Exhaustive stress, differential suites,
performance/sizing and artifact certification belong to the final stabilization
surface in the [delivery plan](implementation-plan.md).
