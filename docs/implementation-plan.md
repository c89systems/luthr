# Luthr delivery plan

Status: active overhaul plan, 2026-11-16.
The [product specification](coroutine-multiplexing-concept.md) and
[architecture](parallel-state-design.md) are authoritative. Compatibility means
observable upstream behavior; existing internal mechanisms may be replaced.

## Working cadence

Land substantial, coherent surfaces. A surface may span compiler, VM, allocator,
collector, scheduler, C API and tests when those pieces share one invariant.
Do not turn the overhaul into a sequence of small fixes to the rejected global
ownership design. Temporary integration rough edges are acceptable while moving
toward the specified architecture; report them rather than adding legacy paths.

Use one focused development branch, serial commands and short feedback loops.
Implement the surface, build its affected targets, run selected unit/invariant
checks, inspect the worktree and commit the coherent result. Keep Conventional
Commits factual and describe the surface delivered. Preserve useful tests and
provenance without treating their old implementation assumptions as requirements.

Early checks are intentionally non-exhaustive. Do not run full CTest, official
Lua suites, E2E comparisons, benchmark matrices, broad Memcheck/Helgrind sweeps,
packaging or release gates during the initial surfaces. A focused reproduction
or diagnostic for an actual defect is appropriate; repeated broad verification
is not the implementation loop. Keep sources stable while any check runs.

## Surfaces and exit evidence

| Surface | Coherent scope | Early exit evidence |
| --- | --- | --- |
| 0. Specification and identity | Replace the preservation-first spec, define this cadence, rename the project/SDK/build surfaces to luthr and move the repository. | Readback, rename/export checks, build and a short unit sample. No exhaustive or E2E run. |
| 1. Shared-memory foundation | Replace central object ownership together with value transfer, captures, allocation publication, barrier work and collection participation. Change compiler/frame/object layout where needed. | Units and selected invariants for independent access, complete snapshots, publication, lifetime, close and allocation failure. No per-operation VM-wide gate. |
| 2. Runtime execution engine | Integrate tasks, execution slots, local runnable work, safepoints, automatic fairness, park/wake and safe movement over bounded reusable carriers. | Small deterministic many-task/few-carrier cases, no-request polling, lost-wake and root-transfer invariants. |
| 3. Native and lifecycle integration | Integrate unchanged blocking bindings, managed reentry, nested/foreign callbacks, affinity, cancellation/shutdown and genuine bounded streaming. | Focused blocked-native progress, callback-at-capacity, ownership retirement and backpressure checks. |
| 4. Product cutover | Carry the replacement runtime through all Lua VM/API/standard-library paths, C89 embedding policy, public task operations, interpreter/compiler and SDK metadata. Remove superseded code in the same cutover. | A small unit/invariant sample for each touched boundary and successful product builds. A partial implementation is not final acceptance. |
| 5. Final stabilization | Exercise the integrated useful runtime comprehensively, repair failures, measure costs and select public limits/defaults, then verify SDK and packages. | Every final gate below passed against the actual final implementation. |

These are architectural delivery boundaries, not a requirement to keep broken
interfaces between surfaces. Move tightly coupled work into the same surface
when splitting it would preserve the rejected design or create throwaway APIs.
Do not freeze public scheduler details before the integrated mechanism works.

## Final gates, deferred until surface 5

1. Upstream compatibility: identical portable Lua source on pristine Lua 5.5.1
   and Luthr, official suites, source/bytecode cross-execution and generic native
   modules. Keep documented platform locale accommodations symmetric.
2. Shared-VM concurrency: tables, live/joined captures, metatables, registry,
   allocation, weak tables, GC/finalizers, reentry, roots and allocation failure.
   Prove ordinary-object parallelism, not only arithmetic or native overlap.
3. Scheduling/lifecycle: more tasks than carriers, automatic fairness, safe
   movement, bounded native compensation, nested/foreign callbacks, affinity,
   budgets/tracing, cancellation, shutdown and late completion.
4. Streaming: genuine incremental progress, bounded retained memory, both-way
   backpressure and cleanup through native saturation/cancellation/shutdown.
5. Platforms/tools: GNU/musl Debug/Release, default-stack checks, targeted and
   broad memory/race verification, formatting and required lifecycle checks.
6. Performance: upstream single-worker overhead, independent/shared object and
   allocation/GC scaling, actual scheduler/mixed-native throughput, tail latency,
   retained memory and carrier counts. Record affinity/quota. Use the results to
   choose sizing and compensation; do not normalize severe regressions away.
7. Artifacts: static/shared libraries, C89/native headers, CLI/compiler,
   relocatable CMake/pkg-config metadata, installed consumers and verified GNU/
   musl packages. Inspect actual shipped bytes, not only build manifests.

## Scope and reporting

Stay in this repository. Independent downstream projects, other architectures,
pushes and release publication remain outside the active goal. The Go research
source is reference material only, not an external implementation dependency.

A passed early unit sample proves only that sample. Report surface status,
current limitations and final gates still deferred. Never call the runtime
complete because a milestone, compatibility sample or benchmark harness passed.
The long-running goal completes only when the final product specification and
all final gates are satisfied.
