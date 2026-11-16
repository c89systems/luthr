# Luthr

Luthr is Lua 5.5.1 without a global interpreter lock, with shared-VM parallel
execution and runtime-owned scheduling. Upstream language/library and supported
native API behavior are the compatibility contract. Internal data structures,
compiler lowering, object/capture representation, allocation, GC and scheduling
may all be replaced to meet it. Independent module projects are consumers,
not implementation targets or runtime dependencies.

## Governing documents

- [Runtime specification](coroutine-multiplexing-concept.md): required behavior,
  no-global-gate contract, native/lifecycle guarantees and products.
- [Runtime architecture](parallel-state-design.md): distributed ownership and
  the connected memory, execution and native surfaces.
- [Delivery plan](implementation-plan.md): large coherent cutovers, short early
  unit/invariant checks, and final exhaustive verification.
- [Building and testing](building.md): commands and prepared toolchain usage.
- [Embedding policy](embedding-policy.md): current C89 policy interface.
- [Lua provenance](lua-source.md) and [C89 facade provenance](cpkt-source.md):
  owned source origins and compatibility inputs.

## Current state

The project is undergoing a runtime overhaul. The prior object/heap ownership
protocol uses VM-wide coordination on ordinary operations and is rejected as
the final architecture. Existing tests, native fixtures, SDK machinery and
portable upstream comparisons remain useful inputs. They do not establish that
the current implementation meets the no-GIL product contract.

Implementation proceeds in substantial connected surfaces with selected short
unit and invariant checks. Full E2E, official suites, benchmark matrices,
exhaustive stress, broad memory/race checks and artifact certification follow
once the integrated runtime is useful. This defers verification cost, not the
final compatibility and correctness requirements.

The products are libluthr static/shared libraries, strict C89 headers under
include/luthr, the luthr interpreter and luthrc compiler, and relocatable CMake/
pkg-config metadata. Initial supported targets are x86_64 Linux GNU and musl.
Source is owned directly under lua/; documentation belongs under docs/.
The project follows the selected C/CMake lifecycle. No sibling-project changes,
additional architectures, pushes or release publication are authorized by this
implementation plan.
