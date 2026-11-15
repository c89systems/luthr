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
implemented with explicit limits, including allocating coroutines and live
captures. Shared-container synchronization and the complete scheduler remain
unfinished.

The [coroutine multiplexing concept](coroutine-multiplexing-concept.md) records
the scheduling requirements and feasibility gates. Documentation belongs under
`docs/`; the root README is a navigation entrypoint.
