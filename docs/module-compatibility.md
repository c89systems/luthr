# Native module acceptance

Required module workflows include curl, MDF/libmdf, Softline, LoneJSON and
liblockdc. Tests must exercise operations, callbacks, streaming, ownership and
cleanup on Luthrum. Loading a module alone does not establish compatibility.
The public scheduler and general resource adapters remain unfrozen.

## Prepared LoneJSON inputs

The current fixtures use the unmodified LoneJSON 0.50.0 Lua source archive and
its x86_64 GNU/musl C SDKs. Exact artifact names and SHA-256 values are declared
in `tests/modules/lonejson-0.50.0.sha256`. Prepare the three release archives:

```sh
make prepare-modules MODULE_ARCHIVE_DIR=/path/to/lonejson-0.50.0-artifacts
```

Preparation verifies all inputs before replacing the selected extracted trees
under `.cache/modules/`. Archives remain in `.cache/modules/archives`; repeating
`make prepare-modules` restages those verified bytes and invalidates native
dependency timestamps. Configure/build only discover prepared inputs. They do
not download sources, invoke sibling builds, or change another checkout.
`make clean` removes this owned cache. Product-only CMake builds can select
`BUILD_TESTING=OFF`; Make's development commands enable the full test set.

## Receiver and callback gates

`native_scheduler_lonejson_*` compiles the released binding and C SDK into the
private scheduler fixture. Two independent receivers use one shared VM and two
native slots on four reusable carriers. Mapping, fixed-capacity strings, record
reuse, array cursors, callback encoding, parsing errors and recovery use the
ordinary module API. The released schema decoder's error pair includes the
original input and an error table; this behavior is preserved by the tests.

Both encoder callbacks remain live while another entrant collects their Lua
stacks. Each callback must use its own task and coroutine. Error/OOM callbacks
must retire registry references and allow subsequent receiver use. Stop and
shutdown leave actual binding frames rooted until they return and acknowledge
completion. Migration occurs only after opaque native frames unwind.

These cases exposed a shared free-list transaction in `luaL_ref`/`luaL_unref`.
Both operations now own the referenced table across allocation/free-list update
and publication, including roots through allocation failure. `reference_*`
separately gates competing reference calls, GC, failure/retry and debug hooks
through native and C89 entry points. References stay unique until released.

## Prepared MDF inputs and constructor integration

MDF acceptance uses the unmodified libmdf 0.23.1 Lua archive and its released
x86_64 GNU/musl C SDKs. Prepare the three archives declared in
`tests/modules/libmdf-0.23.1.sha256` separately:

```sh
make prepare-modules MODULE=libmdf MODULE_ARCHIVE_DIR=/path/to/libmdf-0.23.1-artifacts
```

The same helper validates all inputs before changing the selected module trees.
`MODULE` defaults to `lonejson`; repeating preparation without an archive path
uses the selected module's archives already staged in this checkout.

The released binding lazily initializes `libmdf.owner_map` from both `new` and
`document_stream`. Two unchecked constructors can replace that map concurrently
and lose an existing receiver's callback. The deterministic `mdf_*_init_*` gate
pauses the first constructor after its missing-map read. Removing constructor
serialization makes the second missing-map read fail the gate.

Private integration metadata wraps these two ordinary module entry points with
one task-aware recursive gate. Contenders park without holding a VM object lock
or an execution permit. Calls on independent receivers continue concurrently.
The adapter protects native construction and releases its admission before
rethrowing the original Lua error, including OOM and stop. Tests cover recursive
construction, a queued constructor's cancellation, and stop of the constructor
owner while its unchanged native frame remains live. An unrelated Lua task and
GC must progress before the paused owner is released.

Migration stress requires every unpinned park to resume on a different available
carrier. A forced fixed-carrier swap reproduced a cycle with a gate owner
queued behind its waiting native caller; routing must leave the owner eligible
for an available carrier. Module waits report task routing and admission state
after a stalled test condition so this failure is falsifiable.

`native_scheduler_mdf_*` also exercises independent receiver rendering, source,
sink and write-trace callbacks, exact output, error-object identity, reset/reuse,
document lifecycle, geometry, callback ownership after a creating coroutine is
collected, close/finalization and migration outside opaque native frames. The
source callback's native reads and sink callback's one-slot output pipe provide
bounded streaming and backpressure. The host withholds later paragraphs until
validated word bytes arrive; deferred paragraph spacing is checked against the
complete stream as later input arrives. Source and expected-output scratch stay
record-sized. Both complete input and output exceed the 256 KiB Lua heap cap.
This does not impose a new allocation limit on MDF's own C allocator.

This constructor adapter is private feasibility evidence, not a public module
registration API or a general same-handle serialization policy. No released
module source is patched. Native receiver ownership and finalization adapters
for other libraries remain separate requirements.

## Incremental input and callback output

The streaming gate supplies one bounded item and its delimiter through a native
pipe while withholding the rest of a selected JSON array. A fixed-capacity
record receives each item; callback serialization sends each transformed item
through a one-slot Luthrum pipe to a consumer that validates bytes incrementally.
The enclosing JSON document is never assembled in Lua, a staging file or a host
buffer. Input generation and expected-output checking use bounded record-sized
scratch. Native input buffers and the output pipe provide backpressure.

The consumer must observe the first completed item before the host supplies the
remaining input. Unrelated Lua must finish while the real binding and an
unchanged native fixture occupy both native slots. Collection also runs while
a sink callback waits on output capacity. Native library frames stay on their
carrier, and the callback's roots and borrowed data stay live.

The Lua heap is capped at 256 KiB while complete input and output each exceed
that limit. LoneJSON receives a 64 KiB native allocation limit per runtime.
The constrained-heap fixture sets GC pause to 100, step multiplier to 1000 and
minor multiplier to 10, reserving ordinary finalizer headroom before the hard
cap. Both collector modes and moving allocations are exercised. The streaming
loop contains no explicit yields or per-item collection calls. Stop during a
native read stays pending until input acknowledges EOF; already delivered
partial output remains visible.

## Installed SDK gate and remaining scope

Relocated-SDK consumers compile the same released binding using installed Lua
headers, then load it from strict C89 hosts linked to static and shared Luthrum.
The extension resolves Lua symbols from that one embedding runtime; its ELF
dependencies and definitions must not introduce another Lua implementation.
The serial consumer covers mapping, cursors, sink callbacks, errors and reuse.
Concurrent scheduling still uses private test interfaces, not a public ABI.

MDF receives the same relocated-header/static-host/shared-host checks and uses
its released shared C library for the loadable extension; the released static
archive is not PIC. The scheduler fixture links its static C library into the
executable. The existing simple native fixture and both module hosts remain
distinct consumers. Installed hosts run serial module operations; they do not
provide the private constructor or scheduler integration.

Shared-handle contention policy, optional LoneJSON integrations, remaining module
initialization contracts and the other required modules retain their acceptance
gates. Softline owner-thread callbacks/finalization and liblockdc's native
owning-thread checks need explicit integration. Scoped carrier pins alone do
not certify those contracts. Public scheduling, resource transfer/finalization
policy and performance/sizing measurements remain open.
