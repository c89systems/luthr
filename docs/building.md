# Building and testing

During the runtime overhaul, use `make unit` for the early implementation loop.
It builds the products and a small set of unit programs, then runs eleven short
facade, root, wait-registration and export checks. This sample is intentionally
non-exhaustive. Add/select focused invariant tests for the surface being changed;
do not expand this loop into the full matrix. See the
[delivery plan](implementation-plan.md) for the large-surface sequence.

Full CTest, official Lua suites, E2E, benchmarking, broad memory/race checks and
package certification are deferred until the integrated runtime is useful.
Their commands below describe final stabilization, not per-edit prerequisites.

Initial development supports `x86_64-linux-gnu` and `x86_64-linux-musl` on an
x86_64 Linux host. Each uses the complete pinned Bootlin collection selected by
the `pkt-systems-cmake-lifecycle` skill. There is no host compiler/libc fallback,
source download or patch application during configure and build.

Required workstation tools are Bash 4.4 or later, Make, CMake 3.25 or later,
Ninja, Python 3 for the declared C89 source generator, pkg-config, rg, and
clang-format. Valgrind client headers are required to build the private stack
experiments; CMake copies that header into the owned build tree, so target
compilation does not search host system headers. Valgrind is required for
`make memcheck` and `make racecheck`;
clangd is used for editor verification. The activated lifecycle skill supplies
its toolchain resolver.
Run operations serially against a stable checkout and stable prepared tools.

Early implementation:

```sh
make unit                            # GNU Debug, short non-exhaustive sample
make format-check
```

Final stabilization, after integration:

```sh
make prepare                         # pinned toolchain and pristine Lua test reference
make test                            # GNU Debug, one build worker
make test TARGET=x86_64-linux-musl    # native musl, default worker stacks
make memcheck                        # GNU Debug behavioral memory checks
make racecheck                       # GNU ThreadSanitizer; libc panic case uses Helgrind
make format-check
make install                         # local SDK under build/sdk/<target>/<config>
```

`TARGET`, `CONFIG` (`Debug` or `Release`), and `JOBS` select the build. The
`LIFECYCLE_SCRIPTS_DIR` override points to the activated skill's actual `scripts/`
directory; its default is `~/.codex/skills/pkt-systems-cmake-lifecycle/scripts`.
Configure only discovers prepared tools. `make prepare` is the explicit
acquisition entrypoint; missing prerequisites fail instead of falling back.

Luthr builds its runtime from the vendored Lua sources. Standard tests also
build unmodified upstream Lua 5.5.1 as the compatibility reference. `make prepare`
prepares its checksum-pinned official source and test-suite archives in
`.cache/vendor/`; the reference source is extracted into each owned build tree.
Configure and build never download them. `bash tools/prepare_lua_reference.sh`
prepares those archives when the compiler is already available. Product-only
CMake builds can disable `BUILD_TESTING`; Make's development targets enable it.

Independent Lua module projects are not build or test dependencies. Generic
native-binding fixtures and relocated installed-SDK consumers establish the
runtime boundary; downstream projects own integration tests against that SDK.

`ctest --test-dir build/x86_64-linux-gnu/Debug -L compatibility --output-on-failure`
runs identical portable Lua E2E code with Luthr and the pristine interpreter,
compares stdout/stderr, then compiles with each bytecode compiler and executes
the result with the other runtime. Both collector modes cover language/library
semantics, coroutines, close/unwind, GC, native callback reentry, original error
objects, references, bounded streaming, binary file IO and module loading.
The native fixture is built against upstream headers and loaded unchanged by
both interpreters. These cases establish a growing regression baseline, not
an exhaustive proof for all Lua programs. Concurrency-specific tests separately
verify capabilities upstream Lua does not provide.

The same compatibility label includes `lua_official_upstream` and
`lua_official_luthr`. Each runs the official Lua 5.5.1 suite in a
fresh working directory, including large programs, CLI subprocesses, I/O and
native-module loading. The suite's five native modules are compiled against
upstream headers and loaded unchanged by both interpreters. Stdin is an empty
pipe, as required by the suite's seek test. Each run must exit successfully and
print the final completion marker; random seeds, addresses, memory counts and
timings make its full text unsuitable for an exact stdout comparison. No `_U`,
`_soft` or `_port` exclusions are set. GNU uses unchanged scripts. The pinned
musl fails the suite's Portuguese-locale assumptions even with pristine Lua;
both musl working copies disable only those two locale blocks. The portable
`locale.lua` test compares actual libc collation, character classes and numeric
behavior between runtimes instead. CLI, native-module and I/O tests remain
enabled. The upstream `testC` instrumentation is absent in both builds, and the
suite reports those internal tests as skipped.
Source and checksum details are in [Lua provenance](lua-source.md).

`make benchmark CONFIG=Release` runs the same Lua workloads on both runtimes
sequentially, with two warmups and five measured samples per workload and
collector. Source loading and warmup are outside the timed region. The seven
workloads cover integer loops, tables, closures, strings, allocation/GC, native
callbacks and bounded streaming. Each sample checks its result, and results
must match between runtimes. CPU time uses `os.clock`; wall time uses the same
small native monotonic-clock fixture loaded by both interpreters.
Each timed sample repeats its bounded workload in a batch; both runtimes use
the identical repetition count, recorded in the CSV output. This increases
timing duration without requiring a correspondingly large live heap.

Raw samples, median summaries and run metadata are written under
`build/<target>/<config>/benchmarks/lua/`. Ratios are Luthr/upstream, so values
above one mean slower. Timings are observations, not correctness thresholds;
host load and CPU policy can affect them. The CTest smoke case verifies the
benchmark path with reduced work.

`make benchmark-parallel CONFIG=Release` measures shared-VM execution using a
fixed, reusable host thread pool. The same C host, upstream public headers and
portable `tests/e2e/parallel.lua` payload run against both libraries. Upstream
runs with one worker; Luthr runs with one and increasing worker counts. Each
batch has 32 rooted coroutines, so workers execute multiple jobs. Integer loops,
independent tables, a shared table with disjoint partitions, closures and
allocation/GC run under both collectors. Every job and shared-table partition is
checked; checksums must match all samples, widths and runtimes. No downstream
module projects participate.

The harness records its actual CPU affinity and the most restrictive visible
cgroup v1/v2 quota through the ancestor hierarchy. An unreadable or unresolved
quota is recorded as unknown. Default measured widths are powers of two plus the
rounded-up effective CPU bound, limited by task count and the fixture's 64-worker
ceiling. Unknown quota selects only one worker. These are benchmark choices,
not a public scheduler sizing policy. Workers stay within inherited affinity;
the fixture supports affinity masks up to `CPU_SETSIZE` and fails on larger masks.

Raw samples, medians and metadata live under
`build/<target>/<config>/benchmarks/parallel/`. They include process CPU time,
batch wall time, job service and release-to-completion latency (including queue
wait), Lua live/peak/retained allocation bytes, process resident high-water KiB,
actual worker stack/guard bytes and dispatch counts. Job percentiles use nearest
rank; summaries take the median across samples, averaging the middle two for an
even count. `wall_ratio` is Luthr/upstream (above one is slower), while
`scaling_speedup` compares Luthr's one-worker wall time with the measured width
(above one is faster). Resident high-water comes from this executable's
`/proc/self/status` and includes setup/warmup; zero means unavailable. Lua peaks
are reset before each timed batch. The same atomic counting allocator instruments
both runtimes and adds measurement overhead, especially during parallel
allocation. Loading, preparation, full collection and result verification are
outside timing; dispatch, job execution, automatic collection and worker
rendezvous are inside it. Metadata records capacity, settings, source revision,
dirty state and hashes of the Lua payload, host binaries and Luthr library.

This measures the free-threaded VM under a host pool: jobs run to completion and
do not request application yields. It does not measure the private fiber
scheduler's preemption, native-blocking compensation or cancellation. Those
remain separate feasibility and performance gates before public scheduler
interfaces or automatic worker limits can be selected. The parallel smoke test
checks saved samples, checksums, medians and artifact identities using reduced
work; direct tests exercise both collectors and an actual shared VM.

The native build publishes `build/x86_64-linux-gnu/Debug/compile_commands.json`,
which `.clangd` discovers normally. Run `make build` once before using the editor
to prepare generated facade headers. The database uses the real selected
compiler, sysroot, language and defines from this checkout.

The products are `libluthr.a`, `libluthr.so.0`, `luthr`, and `luthrc`.
Both libraries contain the native Lua API, strict C89 Lua API and embedding
policy. The interpreter links the shared runtime. The compiler links the static
runtime so it can use Lua's private bytecode-writing implementation.

Installed CMake consumers use `find_package(luthr CONFIG REQUIRED)` and link
`luthr::static` or `luthr::shared`. Strict C89 headers are
`<luthr/lua.h>` and `<luthr/lua_runtime.h>`. Upstream headers are also
installed for native bindings built as C99 or newer. pkg-config metadata is
`luthr.pc`; `--static` supplies the static system/thread closure. Metadata
records the selected target and Lua baseline.

When using the project toolchain for an installed consumer, put the SDK prefix
in both `CMAKE_PREFIX_PATH` and `CMAKE_FIND_ROOT_PATH`. Package lookup remains
restricted to target roots. Dynamic consumers need their ordinary deployment
library-search configuration; pkg-config intentionally adds no absolute rpath.

CTest covers both library variants, instruction limits, tracing, errors,
allocation failure, coroutine cleanup, finalizers, native-stack exhaustion and
migration, interpreter/compiler behavior, public symbols and relocated CMake and
pkg-config consumers. The shared consumer loads a native binding linked to the
same shared runtime. Initial private `parallel_vm_*` cases additionally exercise
shared captures, independent preallocated tables and blocking native work in one
VM with allocation/collection disabled during overlap. Synchronization failure
tests check cleanup after native mutex/condition initialization errors. See the
[experiment's boundaries](parallel-state-design.md#initial-gated-stack-experiment);
these checks do not establish parallel scheduler behavior.

The private `gc_rendezvous_*` cases exercise coordinated collection while a
coroutine runs or remains blocked in native code, including competing requests
and finalizers outside collection pauses. `world_unwind_*` verifies cleanup when
errors abandon collector/native scopes, also after forced allocation failure.
Ordinary workers still cannot allocate concurrently in these fixtures. Both
Valgrind commands select its fair thread scheduler for concurrent test progress.

The `parallel_vm_allocate*` cases extend this to allocating coroutines, shared
closed/live captures, short-string interning, long keys and automatic collection.
Disposable userdata verify collection during execution and exactly-once cleanup.
They cover a forced allocation failure while another carrier waits for heap
metadata, and allocating progress while an unchanged native binding is blocked.
These are [constrained VM tests](parallel-state-design.md#allocating-shared-vm-experiment),
with shared-table mutation and the full scheduler still outstanding. Static/shared
C89 facade checks also cover external string keys and exactly-once buffer release.

`parallel_hook_*` exercises asynchronous hook replacement/inheritance, native
debug inspection, hook reentry, collection while a hook is blocked, hook yields,
and a targeted signal interrupting a carrier that owns its coroutine gate.
Hook configuration uses lock-free scalar atomics; this does not establish
foreign debug transactions or the scheduler's execution-ownership contract.

`parallel_policy_*` starts shared-VM workers from one C89 host invocation. It
checks exact aggregate instruction accounting, trace replacement/removal and
blocked-observer progress, coroutine wrapper/context access, warning pairs,
allocation caps, and external-string failure cleanup. Constructor mutex failures
have separate `synchronization_failure_facade_*` cleanup/recovery cases.

`fiber_vm_*` verifies eight Lua tasks over two reused carriers, forced migration
without Lua yields, CPU fairness, parked-stack relocation, register/FPU state,
error recovery, allocation failure, native stack exhaustion and binding affinity.
`fiber_stack_guards` faults both inaccessible guard pages in child processes.
`native_scheduler_*` uses one managed permit, two native-call slots and three
reused carriers. Unchanged blocking bindings retain their carrier/TLS while
queued Lua work and a nested callback complete before either binding is released.
Cases cover saturated admission, observer mutex ordering, instruction budgets,
GC roots, errors and yielded continuations. `native_scheduler_wait_*` additionally
checks task parking at coroutine, object, metadata and collection waits; one
carrier must remain reusable, and two-carrier variants force ownership migration.
Early notifications precede outgoing context publication and collection.
`native_scheduler_resource_*` keeps native callbacks pinned while their runtime
waits release execution capacity. These are private experiments; foreign-thread
routing now has focused registered-entry coverage, while general admission,
native resource affinity and the installed scheduler remain open.

`callback_entry_*` tests protected registration/invocation, GC roots, constructor
and allocation failures, close/revocation, errors, native-stack exhaustion and
bounded diagnostics on default worker stacks. `native_scheduler_foreign_*`
keeps owned native slots saturated while provider threads invoke registered Lua
callbacks, including nested calls, C89 budgets, live captures and queued/late
revocation. Provider threads are distinct from the bounded owned carrier pool.

`pipe_contract_*` covers private runtime closure classification and bounded
chunk-pipe construction, identity-preserving transfer without body allocation,
errors and finalization. `native_scheduler_stream_*` moves 1 MiB through a
one-slot pipe with a 128 KiB Lua-heap cap, withholding further input until the
first chunk reaches a consumer. Cases cover GC while parked, one-carrier reuse,
forced migration, native saturation, a foreign producer, and close/finalization
of entered waits. These are private feasibility gates, not installed APIs or
proof of the required modules' streaming behavior.

`control_snapshot_*` replaces a wrapped coroutine's capture and forces GC before
the child's first API entry. Raw and C89 wrappers must retain and execute the
original snapshot. `pipe_contract_identity_*` verifies canonical zero-capture
runtime functions during concurrent creation and GC. The
`native_scheduler_stream_control_*` cases add protected calls, yielded
continuations, nested coroutine wrappers and to-be-closed handlers to the
bounded pipeline, including migration and native saturation. C89 variants keep
instruction policy active. Other library/module migration remains unproven.

`coroutine_operation_*` adds controlled overlap at argument/result transfer,
close cleanup and yield recovery. Busy calls must preserve the target stack;
GC, nested/error-handler failures, allocation denial and nested VM construction
must retire ownership correctly. Raw and C89 entrypoints use both collectors
on default workers. Reentrant callback chains must reject self-close across
another state's operation. Migrated control pipelines also assert scope retirement
before their retained stacks can be destroyed.

`task_ownership_*` checks persistent task admission and roots before execution,
during native/cleanup waits and after the owner thread has exited. Lua attempts
to resume, wrap or close an owned coroutine must preserve its stack. Other cases
cover driver identity, yields, self-close, exactly-once cleanup, memory errors,
competing collectors, stack-growth retry, cross-VM rejection and execution reuse.
The scheduler's Lua tasks use the same records through migration, native-call
saturation, callbacks and streaming. Public task lifecycle remains unfinished.

`task_ownership_*await*` adds completed-result waiting, failed-stack retry and
waiters queued behind an active collector, including retry notification.
`native_scheduler_task_*` runs eight tasks over four carriers with two managed
permits and two occupied native slots. It checks simultaneous VM participation
on selected allowed CPUs, task parking, forced migration, early notification,
unrelated-task progress, nested native callbacks, self-wait rejection and cleanup.
Driver variants await before their first Lua entry. CPU IDs are printed by the
fixture; throughput, automatic sizing and public task lifecycle remain open.

`math_library_*` checks pre-change seed/draw vectors, invalid-call advancement,
reopening identity, concurrent draws and reseeding, capture replacement with GC,
invalid userdata/captures and allocation failure. A gated holder/waiter case
proves exclusive payload access and roots after both function captures change.
The `native_scheduler_task_*library*` variants run math/table helpers and large
buffer cleanup while both native slots are occupied, including a sort comparator
that awaits through math comparison callbacks, migration, GC and error unwinding.

`utf8_library_*` covers Unicode boundaries, strict/lax errors, start/end offsets,
canonical function identity, allocation failure and rooted inputs after debug
argument replacement. `native_scheduler_task_*utf8*` requires checkpoints in
every long-scan branch, migration within each helper and collection with live
buffers/results while both native slots are occupied. Foreign callback variants
verify carrier pinning through the same operations.
Moving-allocator callbacks also exercise relocation and errors after partial
encoding/decoding. `api_ownership_vm_*` forces stack relocation at unlocked VM
allocation handoffs and verifies subsequent table, string and closure results.

`pattern_library_*` covers find/match/gmatch/gsub semantics, malformed patterns,
replacement errors, canonical functions, allocation denial and bounded native
recursion. Input snapshots survive debug argument/capture replacement and GC;
substitution callbacks also replace their caller's arguments and return external
strings retained across copying checkpoints. Two concurrent consumers share one
iterator and must return each position exactly once, including collection during
consumption. `native_scheduler_task_*pattern*` checks bounded work in literal
search, class parsing, balanced matches, repetition/backtracking, backreferences,
replacement copying and iterator scans while both native slots are occupied.
Cases require migration inside each matching helper, GC with saved C contexts,
unrelated-task progress, callbacks that await a scheduled result, pinned nested
native callbacks, moving allocators and error cleanup.

`string_library_*` checks slicing, reversal, case/byte conversion, repetition,
formatting and pack/unpack behavior, invalid conversions, canonical functions,
and allocation-failure cleanup with moving storage. Snapshot cases remove
debug arguments during each long operation and force collection; formats,
input strings, separators, `__tostring` owners and returned external strings
must remain alive. `native_scheduler_task_*strings*` requires checkpoints and
migration inside eleven long helpers with two occupied native slots. Cases
include collection with saved buffers, literal and quoted formatting,
zero-terminated scans, padding, format-only loops, an `__tostring` callback
awaiting another task's result, pinned native reentry and error cleanup.

`dump_library_*` verifies bytecode round-trips, stripped output, stable function
identity, writer status/end signaling, allocation denial and private roots after
debug argument replacement. A fixed full/stripped bytecode fixture from the
verified preceding runtime checks exact Lua 5.5.1 x86_64 bytes independently of
the current decoder. Public writers retain native admission and carrier
identity; the runtime's bounded buffer writer stays managed. Writer failure must
stop further callbacks and allocations and preserve the returned status, even
when the allocator begins denying growth. A number-conversion boundary test
replaces a string arithmetic operand and forces collection before parsing it.
Deep dumping near a worker's native-stack limit must report a Lua error and
retire its roots, with successful dumping after recovery. GNU Debug and musl
use default worker stacks; GNU Release requests 128 KiB for this stack fixture.
`native_scheduler_task_*dump*` checks byte-identical output under native-slot
saturation, forced migration, saved-context GC, pinned native reentry and string
arithmetic callbacks that await task results. Arbitrarily long numeric parsing
still uses the existing parser and needs its own bounded-work feasibility gate.

`load_library_*` checks text/binary loading, environment selection, reader errors,
allocation denial and retention of source/name/mode/environment snapshots after
debug argument replacement. Reader closures and their current external fragment
remain rooted independently of Lua stack temporaries. Owned memory readers and
Lua-reader trampolines remain managed; public C readers and Lua C bindings called
by a reader retain native admission. A 1 MiB comment stream is compiled under a
128 KiB Lua-heap cap with at most two live 2 KiB external fragments. Reader
fragments feed the parser directly; compilation still constructs complete
prototypes and literal values as required by Lua's load API.
`native_scheduler_task_*load*` compiles and executes text/binary functions with
both native slots occupied, requires migration during input consumption, collects
with saved parser state and exercises nested loads and reader callbacks awaiting
another task's result. File loading and other library/module gates remain open.

`package_library_*` checks cache/loader-data semantics, nested loads, numeric and
embedded-NUL names, table metamethods, callback errors, allocation denial and
native admission. Snapshot cases replace the package capture, registry cache and
debug-visible name during a loader, then collect the selected tables, loader,
external name and loader data. Concurrent misses must reach both loaders without
a module-wide lock; cache lookup, loading and publication remain separate table
operations. The result may reflect a peer's cache publication. File searchers,
path probing and dynamic-library loading must still consume native admission.
`native_scheduler_task_*package*` requires completion under native saturation,
nested preloads, task-result waits, moved contexts, GC and pinned native reentry.
This is package orchestration evidence, not native-module compatibility proof.

`file_library_*` checks text/binary files, result counts, explicit environments,
stdin, BOM/shebang handling, empty files, I/O/syntax/mode errors, yielded and
closed continuations, native admission, snapshots and moving-allocation denial.
Opening and closing are counted through both x86_64 stdio entrypoints, including
every failed allocation attempt. Snapshot cases replace filename/mode/environment
arguments during parsing and collect before the original environment is applied.
Recursive file execution must report stack overflow, close every opened file and
leave the VM usable on a 128 KiB worker stack.
`native_scheduler_task_*file*` first parks inside two `dofile` chunks, verifies
neither retains native admission, then saturates both native slots. The loaded
Lua bodies must execute in parallel, migrate, park, collect and return or fail
normally. Native callback variants load another file on the original carrier.
File reading/compilation remains native; this does not prove bounded work or
resource affinity for every I/O path.

`task_stop_*` checks the private stop-request experiment. Explicit gates hold a
task before start, in a CPU loop/parser, after yielding, inside an unchanged
native call/continuation or inside native close cleanup. Independent tasks must
still finish, and collection must retain active native/cleanup owners. Other cases cover repeated
and concurrent requests, protected calls, protected/unprotected native callbacks,
another VM, normal completion racing a request and cleanup errors. Denial checks separately
prove request/delivery make no allocation attempts; explicit host GC runs outside
the denial interval. A request/delivery flag never substitutes for completion.
These tests do not establish shutdown.

`stop_wait_*` checks interruptible owned pipe/result waits, the driver STOPPED
outcome, pre-publication requests, queue head/tail removal, notification/close/
explicit-finalization races and concurrent notifier borrows. A stopped waiter
must leave its peer and the target outcome intact. Moving-allocation denial
requires request, registration retirement and delivery to make no allocation
attempts. Pinned callers retain native owners after catching the stop; close
cleanup waits remain exempt until ordinary notification.
`native_scheduler_stop_*` requires those paths to finish on four reused carriers
while two unchanged bindings occupy both native slots. Forced migration includes
early notification before context publication and driver waits. Independent Lua
work and the uncancelled result target must still return 42. Raw/C89 and both
collectors include moving allocators and GC with saved wait stacks.

Debug GNU test processes use 128 KiB default worker stacks and preserve guards,
explicit stack-size requests and main-thread limits. The stack cache is disabled
for those processes, including child applications. Musl uses its defaults.
Native recursion reserves 32 KiB for error handling, including unoptimized
traceback frames, and preserves the last 8 KiB for emergency unwinding. This
guards Lua-owned recursion; arbitrary native modules still own their stack use.
Debug builds also enable Lua's internal invariants and C API argument assertions.

Only non-shipped test programs contain private loader and sysroot search paths.
Their loaded libc comes from the selected collection. Installed payload uses
relative runtime paths. Build logs, fixtures and local SDKs belong under `build/`;
`make clean` removes all of `build/`, `dist/` and local `.cache/`, including hidden
entries, while preserving the declared global cpkt caches and workstation tools.
