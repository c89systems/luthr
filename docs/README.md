# Documentation

Luthr provides shared-VM parallel Lua execution, runtime-owned scheduling and a
C89 embedding SDK. Its implementation lives under `src/`; public headers live
under `include/luthr/`.

- [Build and test](building.md): prerequisites, commands, test coverage and SDK installation.
- [Embedding policy](embedding-policy.md): invocation limits, tracing, task ownership and cleanup.
- [Source provenance](lua-source.md): Lua baseline, checksums and license notices.
- [Runtime specification](coroutine-multiplexing-concept.md): required product behavior.
- [Runtime architecture](parallel-state-design.md): memory ownership, scheduling and native boundaries.
- [Implementation and verification plan](implementation-plan.md): delivery scope and final acceptance gates.
- [Lua manual](lua/manual.html): upstream language and standard-library reference.

The runtime, scheduler and public APIs are implemented. Final performance
tuning, automatic sizing and package verification remain open. Supported
build targets are x86_64 Linux GNU and musl; local test results do not constitute
a published release.
