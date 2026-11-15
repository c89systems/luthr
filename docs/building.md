# Building and testing

Initial development supports `x86_64-linux-gnu` and `x86_64-linux-musl` on an
x86_64 Linux host. Each uses the complete pinned Bootlin collection selected by
the `pkt-systems-cmake-lifecycle` skill. There is no host compiler/libc fallback,
source download or patch application during configure and build.

Required workstation tools are Bash 4.4 or later, Make, CMake 3.25 or later,
Ninja, Python 3 for the declared C89 source generator, pkg-config, rg, and
clang-format. Valgrind is required for `make memcheck` and `make racecheck`;
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
