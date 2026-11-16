# Luthr

Luthr is an embeddable Lua 5.5.1 runtime with parallel execution in a shared VM,
without a global interpreter lock. Tasks share ordinary Lua objects and run on
a bounded pool of OS threads. The runtime owns scheduling, garbage-collection
coordination and native-call transitions.

The compatibility target is upstream Lua's language, standard libraries, C API,
interpreter/compiler behavior and bytecode. The implementation is maintained
directly in this repository.

## What it provides

- Shared-VM tasks with automatic scheduling handoffs for CPU-bound Lua,
  cancellation, retained results and a completion descriptor for host event loops.
- Separate limits for managed parallelism, native calls, carriers and callbacks.
  Blocking native bindings release managed capacity while their C activations
  remain on the original thread.
- Nested and registered foreign-thread callbacks, explicit carrier affinity,
  instruction budgets and invocation-local tracing.
- Bounded chunk pipes with incremental producer/consumer progress and backpressure.
- Static and shared libraries, native Lua headers, a C89 embedding interface,
  the `luthr` interpreter and the `luthrc` bytecode compiler.

Multi-step operations on shared application data still need application
coordination. Native libraries retain their own thread-safety and affinity rules.

**Status:** pre-1.0 development. The runtime and public task API are implemented;
final performance tuning, automatic sizing and package verification remain open.
Supported build targets are x86_64 Linux with GNU libc or musl.

## Build and test

Use Bash, Make, CMake 3.25+, Ninja, Python 3, pkg-config, ripgrep,
clang-format and Valgrind with its client headers. Python generates the C89
interface; the runtime is C and x86_64 assembly.

Builds use pinned Bootlin toolchains through the
`pkt-systems-cmake-lifecycle` toolchain resolver. Set `LIFECYCLE_SCRIPTS_DIR`
to its `scripts/` directory if it is installed somewhere other than
`~/.codex/skills/pkt-systems-cmake-lifecycle/scripts`.

```sh
make prepare                         # acquire the pinned GNU tools and Lua test inputs
make build                           # GNU Debug; one build worker by default
make unit                            # short implementation checks
make test                            # full GNU Debug suite
make test CONFIG=Release              # GNU Release

make prepare TARGET=x86_64-linux-musl
make test TARGET=x86_64-linux-musl
make test TARGET=x86_64-linux-musl CONFIG=Release

make format-check
make memcheck                        # GNU Debug under Valgrind
make racecheck                       # ThreadSanitizer plus the libc panic Helgrind case
```

Preparation is the acquisition step; configure and build use prepared inputs.
Run build and test commands serially against a stable checkout. Outputs live in
`build/<target>/<config>/`. The GNU Debug build also supplies the compilation
database used by the repository's clangd configuration.

The full suite includes identical portable programs on pristine Lua and Luthr,
the official Lua suite, bytecode cross-execution, generic native modules,
concurrency/lifecycle tests and relocated SDK consumers. Benchmarks use the same
Lua workloads for both runtimes:

```sh
make benchmark
make benchmark-parallel
make benchmark-scheduler
```

Benchmark targets default to Release builds. `make benchmark` prints a comparison
table and saves it alongside raw samples and CPU/wall medians under
`build/<target>/Release/benchmarks/lua/`. Ratios are Luthr/upstream elapsed time:
`2.000x` means Luthr takes twice as long. `CONFIG` overrides the build mode.

## Embed Luthr

`make install CONFIG=Release` stages the SDK under
`build/sdk/x86_64-linux-gnu/Release/`. Use `TARGET=x86_64-linux-musl` for a musl SDK.
The SDK exports `luthr::static` and `luthr::shared` CMake targets and a `luthr`
pkg-config package. Consumers must use a matching target toolchain and libc.

The C89 interface offers a policy-controlled runtime for synchronous execution:

```c
#include <luthr/lua_runtime.h>
#include <stdio.h>

int main(void) {
  static const unsigned char source[] = "print(6 * 7)";
  luthr_lua_runtime *runtime;
  luthr_lua_runtime_status status;

  if (luthr_lua_runtime_new(&runtime) != LUTHR_LUA_RUNTIME_OK)
    return 1;
  status = luthr_lua_runtime_run_buffer(
      runtime, source, sizeof(source) - 1, "=example", 0, NULL,
      LUTHR_LUA_RUNTIME_OPEN_LIBS);
  if (status != LUTHR_LUA_RUNTIME_OK)
    fprintf(stderr, "%s\n", luthr_lua_runtime_error(runtime));
  luthr_lua_runtime_free(runtime);
  return status == LUTHR_LUA_RUNTIME_OK ? 0 : 1;
}
```

Link an installed SDK from a CMake project, setting `CMAKE_PREFIX_PATH` to its
installation directory:

```cmake
find_package(luthr 0.1 REQUIRED CONFIG)
add_executable(example example.c)
set_target_properties(example PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED ON)
target_link_libraries(example PRIVATE luthr::shared)
```

`<luthr/lua.h>` exposes the C89 Lua API. `<lua.h>`, `<lauxlib.h>` and `<lualib.h>`
provide the native Lua interface. `<luthr/tasks.h>` adds schedulers, task
submission, completion, cancellation, callbacks and pipes. A scheduler uses one
VM; tasks retain their outcomes until collected, and task handles must be freed
before the scheduler and VM. See the public headers for ownership contracts and
`tests/consumer/main.c` in the checkout for a complete task example.

## Source and license

All implementation lives under `src/`: `src/runtime/` contains the VM, compiler,
standard libraries and internal execution engine; the other source files
implement the embedding API, public task API, policy and CPU sizing.
`include/luthr/` contains public SDK headers, `tests/` contains executable
verification, and `docs/lua/` retains the upstream manuals.

Luthr is distributed under the [MIT license](LICENSE). The checked-in Lua
baseline is checksum-pinned 5.5.1; its original notices are retained. Detailed
[source provenance](docs/lua-source.md), [build notes](docs/building.md) and
[runtime architecture](docs/parallel-state-design.md) are available alongside
the implementation.
