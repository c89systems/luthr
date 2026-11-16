# Building and testing

## Prerequisites

Supported targets are `x86_64-linux-gnu` and `x86_64-linux-musl` on an x86_64
Linux host. Builds use pinned Bootlin toolchains, with no host compiler/libc
fallback. The `pkt-systems-cmake-lifecycle` skill supplies the toolchain resolver;
set `LIFECYCLE_SCRIPTS_DIR` to its `scripts/` directory when needed. The default
is `~/.codex/skills/pkt-systems-cmake-lifecycle/scripts`.

Required host tools are Bash 4.4+, Make, CMake 3.25+, Ninja, Python 3, pkg-config,
ripgrep, clang-format and Valgrind with client headers. Python runs the declared
C89 interface generator. Configure copies Valgrind's client header into the
build tree, keeping host include directories out of target compilation.
Clangd uses the GNU Debug compilation database.

`make prepare` acquires the selected toolchain and checksum-pinned upstream Lua
source/test archives. Configure and build only discover prepared inputs.
Reference sources are extracted into the build tree; product sources are the
checked-in files under `src/`.

## Commands

`TARGET`, `CONFIG` (`Debug` or `Release`) and `JOBS` select the build. The
defaults are GNU, Debug and one worker; benchmark targets default to Release.
Run operations serially, keeping sources
and prepared tools stable until each operation finishes.

```sh
make prepare
make build
make unit
make test
make test CONFIG=Release

make prepare TARGET=x86_64-linux-musl
make test TARGET=x86_64-linux-musl
make test TARGET=x86_64-linux-musl CONFIG=Release

make format-check
make memcheck
make racecheck
```

`make unit` builds the affected products and runs a short, non-exhaustive unit
selection. `make test` builds all test programs and runs the complete CTest
suite. Select an existing test directly for a focused check:

```sh
ctest --test-dir build/x86_64-linux-gnu/Debug -R '^public_tasks_' \
  --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
```

The format targets check SDK and test code; `src/runtime/` retains Lua's source
formatting. All generated files, binaries, local SDKs and logs belong under
`build/`. `make clean` removes `build/`, `dist/` and local `.cache/`, including
hidden entries, while preserving installed workstation tools and global
toolchain caches.

## Verification coverage

The full suite covers the raw and C89 APIs, shared tables and captures,
allocation/GC, weak references, finalizers, error/unwind behavior, coroutine
ownership, scheduler fairness, blocking native calls, nested/foreign callbacks,
affinity, budgets, tracing, cancellation, shutdown and bounded chunk pipes.

Portable E2E programs run unchanged on pristine Lua 5.5.1 and Luthr, with output
comparison in both collector modes. Bytecode is compiled by each compiler and
executed by the other runtime. Generic native modules built against upstream
headers are loaded unchanged by both interpreters.

The official Lua suite also runs on both runtimes in fresh working directories.
GNU uses unchanged scripts. For the pinned musl, both copies disable only the
two Portuguese-locale blocks whose assumptions fail on pristine Lua as well.
The portable locale test compares actual libc behavior. CLI, native modules and
I/O remain enabled; the upstream internal `testC` instrumentation is absent.
See [source provenance](lua-source.md) for the exact archives and checksums.

GNU Debug test processes use 128 KiB default worker stacks with guards and the
glibc stack cache disabled. Explicit stack requests and main-thread limits are
preserved. Musl workers use their libc defaults. Debug enables internal
invariants and C API assertions.

`make memcheck` runs the registered memory checks under Valgrind.
`make racecheck` uses the pinned GNU ThreadSanitizer runtime and verifies its
detector with an intentional native race. The pinned sanitizer cannot recover
libc jump buffers, including a standalone reproduction; the foreign-error
panic/longjmp case runs under Helgrind instead. It is not suppressed or omitted.

## Benchmarks

```sh
make benchmark
make benchmark-parallel
make benchmark-scheduler
```

The serial benchmark compares identical Lua workloads on pristine Lua and
Luthr. It prints a table of median wall-time ratios for both collectors and
saves `comparison.txt`, `samples.csv`, `summary.csv` and `metadata.json` under
`build/<target>/<config>/benchmarks/lua/`. A ratio above 1 means Luthr takes
longer; CPU-time medians are retained in the CSV summary. `CONFIG` can override
the Release default for diagnostic runs.
The parallel benchmark measures shared-VM execution with host workers.
The scheduler benchmark exercises the public task API, including mixed native
work, completion latency, carrier limits and allocator/RSS observations.
Drivers validate results and retain raw samples and summaries under
`build/<target>/<config>/benchmarks/`. Interpret results with the recorded
affinity, CPU quota and configuration; automatic sizing remains provisional.

## SDK installation

```sh
make install CONFIG=Release
make install TARGET=x86_64-linux-musl CONFIG=Release
```

SDKs are staged under `build/sdk/<target>/<config>/`. Each contains static/shared
libraries, native and C89 headers, the interpreter/compiler, CMake/pkg-config
metadata, documentation and license notices. Installed consumers use a matching
target toolchain and libc. See the root [README](../README.md#embed-luthr) for
a complete embedding example and CMake linkage.

The full suite installs and relocates an SDK, then builds and runs strict C89
consumers through both CMake and pkg-config with static/shared linkage. It also
loads a native module and checks public exports and installed documentation links.

Only non-shipped test executables use private loader/sysroot paths to run
against the selected libc on the build host. Installed payloads use relative
runtime paths. Local installation checks are separate from final package
verification; no release or package certification is implied by a successful
build.
