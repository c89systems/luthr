# Building and testing

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

```sh
make prepare                         # explicit pinned toolchain preparation
make test                            # GNU Debug, one build worker
make test TARGET=x86_64-linux-musl    # native musl, default worker stacks
make memcheck                        # GNU Debug behavioral memory checks
make racecheck                       # GNU Debug private VM paths under Helgrind
make format-check
make install                         # local SDK under build/sdk/<target>/<config>
```

`TARGET`, `CONFIG` (`Debug` or `Release`), and `JOBS` select the build. The
`LIFECYCLE_SCRIPTS_DIR` override points to the activated skill's actual `scripts/`
directory; its default is `~/.codex/skills/pkt-systems-cmake-lifecycle/scripts`.
Configure only discovers prepared tools. `make prepare` is the explicit
acquisition entrypoint; missing prerequisites fail instead of falling back.

The native build publishes `build/x86_64-linux-gnu/Debug/compile_commands.json`,
which `.clangd` discovers normally. Run `make build` once before using the editor
to prepare generated facade headers. The database uses the real selected
compiler, sysroot, language and defines from this checkout.

The products are `libluthrum.a`, `libluthrum.so.0`, `luthrum`, and `luthrumc`.
Both libraries contain the native Lua API, strict C89 Lua API and embedding
policy. The interpreter links the shared runtime. The compiler links the static
runtime so it can use Lua's private bytecode-writing implementation.

Installed CMake consumers use `find_package(luthrum CONFIG REQUIRED)` and link
`luthrum::static` or `luthrum::shared`. Strict C89 headers are
`<luthrum/lua.h>` and `<luthrum/lua_runtime.h>`. Upstream headers are also
installed for native bindings built as C99 or newer. pkg-config metadata is
`luthrum.pc`; `--static` supplies the static system/thread closure. Metadata
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
