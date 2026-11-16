# C89 facade provenance

The strict C89 Lua API generator, embedding-policy facade, native-stack handling,
and their regression tests were extracted from cpkt at commit
`f51ceac900ac39db63a018576c5329a2b2244e5c`.

The copied source was clean relative to that commit. Public identifiers now use
the `luthr_` prefix, headers use `luthr/`, and build integration is owned by
this repository. There is no build-time dependency on a cpkt checkout or SDK.
Lua changes are maintained directly in `lua/src/`.

The original [MIT notice](licenses/cpkt-MIT.txt) applies to the extracted code.
