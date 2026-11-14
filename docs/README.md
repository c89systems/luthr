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
execute Lua. Lua bytecode may execute serially while native calls overlap.
Blocking native work must release the execution permit so other runnable tasks
can progress on another carrier. Runtime-managed waits park tasks, and CPU-bound
Lua tasks must hand off execution without application-authored yields. Safe
migration, bounded carrier growth, and reserved progress capacity are required.

## Source baseline

The complete Lua 5.5.1 release is checked in at [`lua/`](../lua/).
Its source, headers, documentation, license notice, and upstream Makefiles are
unmodified in the initial import. [Provenance and ownership](lua-source.md)
record the source archive and verified checksum.

Lua source changes belong directly in this repository; there is no patch series
or build-time source download. The C89 facade extraction, scheduler, project
build and test entrypoints, and release packaging are not implemented yet.

The [coroutine multiplexing concept](coroutine-multiplexing-concept.md) records
the scheduling requirements and feasibility gates. Documentation belongs under
`docs/`; the root README is a navigation entrypoint.
