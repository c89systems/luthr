# Luthrum

Luthrum is the shared Lua runtime being developed for the C89 stack, including
Vectis and libknurl. It will own the Lua implementation, the strict C89 facade
currently in cpkt, and coroutine scheduling over OS threads.

The project follows the C/CMake lifecycle: Make exposes commands, Bash handles
sequencing, CMake owns the build graph, and CTest owns verification. Initial
implementation and verification focus on x86_64; other architectures are deferred
until the runtime is established on this architecture.

The initial products are static and shared runtime libraries, a strict C89 SDK,
an interpreter, and a bytecode compiler. This phase changes only Luthrum and
verifies consumers against its installed SDK. Cutover changes in cpkt, Vectis,
and libknurl follow later.

The execution model separates a task, its carrier OS thread, and permission to
execute Lua. Lua bytecode must execute in parallel across cores within one
shared VM, without a global interpreter lock. Blocking native work must release
managed execution capacity so runnable tasks can progress on another carrier.
Runtime-managed waits park tasks, and CPU-bound Lua tasks must hand off execution
without application-authored yields. Safe migration, bounded carrier growth,
and reserved progress capacity are required. Automatic carrier sizing starts
from usable logical CPUs; the cap and native compensation policy require
workload measurements.

## Source baseline

The complete Lua 5.5.1 release is checked in at [`lua/`](../lua/).
Its source, headers, documentation, license notice, and upstream Makefiles are
unmodified in the initial import. [Provenance and ownership](lua-source.md)
record the source archive and verified checksum.

Lua source changes belong directly in this repository; there is no patch series
or build-time source download. The current build includes the native runtime,
the extracted C89 interfaces, interpreter, compiler and installed-SDK tests.
Native-stack protection and finalizer instruction accounting are direct Lua
source changes. The parallel scheduler and release packaging remain unfinished.

See [building and testing](building.md), [embedding policy](embedding-policy.md)
and [C89 facade provenance](cpkt-source.md).

The [parallel-state investigation](parallel-state-design.md) compares
free-threaded CPython and Go and records the first shared-VM candidate and its
required evidence. Private shared-VM execution and collection experiments are
implemented with explicit limits, including allocating coroutines, live and
joined Lua/C captures, shared-table/metatable and registry operations, and forced
GC at API ownership boundaries. Cross-state transfers, parent-call snapshots,
carrier-local error forwarding and callback configuration have focused parallel
regressions. Asynchronous native hook publication avoids walking live frames or
waiting for the interrupted coroutine's gate. Parallel C89 policy experiments
cover aggregate instruction budgets, observer dispatch, coroutine context and
allocator ownership. A private retained-stack experiment proves Lua-task
migration and CPU handoff over two reused carriers, including GC and moved
error boundaries. A bounded native-call experiment proves queued Lua and nested
callback progress while unchanged bindings occupy both native slots, including
instruction budgets and observer admission. Runtime wait experiments also cover
task parking at coroutine, object, metadata and collection waits, ownership on
reused carriers, forced migration and early notification. A private registered
callback prototype adds protected foreign-thread entry, nested calls at native
saturation, live captures and revocation with retained roots. General external
admission, native-resource affinity, shutdown and installed scheduling remain unfinished.
Audited runtime closures and bounded chunk pipes now have private streaming
proofs for backpressure, migration, native saturation and foreign callbacks;
they do not establish streaming compatibility for the required modules.
Owned base/coroutine control functions and C89 policy wrappers now support these
parking/migration paths, with stable function identity on reopening and rooted
wrapped-coroutine snapshots. Math/table helpers and auxiliary-buffer cleanup now
also support managed execution, with scoped ownership for shared PRNG state.
UTF-8 helpers also support managed execution, with bounded work checkpoints
and private roots for input strings across parking and debug argument changes.
String pattern helpers also support managed execution, with invocation-local
matching scratch, rooted inputs/replacements and synchronized shared iterators.
String slicing, case conversion, repetition, byte conversion, formatting and
packing helpers now also support managed execution and bounded work checkpoints.
Dumping and string-arithmetic callbacks also support managed execution, with
private roots and native-writer admission preserved. Long numeric-input parsing
still needs its separate fairness proof.
Loading from memory and Lua reader callbacks now supports managed execution,
with private parser/fragment roots and checkpoints during input consumption.
Public reader callbacks and file I/O retain native admission.
Package `require` and its preload searcher now support managed orchestration,
with private snapshots across loader callbacks and bounded search-error copying.
File and dynamic-library searchers keep their native boundaries.
Other libraries still require their audit.
Resume/close operation scopes now reject overlap through argument/result
transfer and cleanup, retaining both states through GC and protected unwinding.
Private scheduled-task records now reserve coroutines and retain their results
through collection, including allocation-failure retry and error cleanup.
Private awaiting now parks managed tasks or preserves a native caller's carrier,
including simultaneous Lua execution with two managed permits while two native
slots are blocked. The public task API, cancellation and shutdown remain unfinished.

The [coroutine multiplexing concept](coroutine-multiplexing-concept.md) records
the scheduling requirements and feasibility gates. Documentation belongs under
`docs/`; the root README is a navigation entrypoint.
