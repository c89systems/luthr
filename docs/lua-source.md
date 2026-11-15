# Lua source provenance and ownership

| Field | Value |
| --- | --- |
| Location | Top-level `lua/` |
| Upstream | https://www.lua.org/ |
| Baseline version | 5.5.1 |
| Source archive | https://www.lua.org/ftp/lua-5.5.1.tar.gz |
| SHA-256 | `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce` |
| Release listing | https://www.lua.org/download.html |
| Retrieved and verified | 2026-11-14 |
| License | MIT; upstream notice in [`lua/doc/readme.html`](../lua/doc/readme.html#license) and [`lua/src/lua.h`](../lua/src/lua.h) |

The initial import contains every file from the verified source archive, with
only its top-level `lua-5.5.1/` directory renamed to `lua/`. No upstream file was
modified. The archive checksum identifies this baseline, not future modified
source trees.

The top-level `lua/` directory is Luthrum-owned runtime source, maintained
directly as part of this project. Use Git history to track changes; do not add a
patch series or regenerate the source during builds.
Upstream updates are deliberate source changes that retain the license notice
and update this provenance record. Builds must use this checked-in source.

The current tree includes direct native-stack protection and opt-in finalizer
instruction-accounting changes extracted from cpkt. See the
[facade provenance](cpkt-source.md) and [embedding policy](embedding-policy.md).
It is no longer byte-for-byte upstream source.

Private temporary roots, per-coroutine stack gates, VM poll points and
synchronized Lua capture transfers, managed/native pins and collection rendezvous
are also maintained directly in this tree, along with heap-metadata ownership,
coordinated barriers and long-string hash completion before publication.
The [parallel-state investigation](parallel-state-design.md) records the limited
execution evidence and remaining shared-heap requirements.

Keep generated objects, binaries, and scratch installations outside the source
tree, under the repository's ignored `build/` directory. Dependency acquisition
scratch belongs under the ignored `.cache/` directory.
