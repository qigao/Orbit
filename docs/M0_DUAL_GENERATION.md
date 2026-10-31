> Historical source/evidence record. The current native SDK requirement and
> Provider ABI migration are documented in [M0 ABI 2](M0_ABI2_MIGRATION.md).

# M0g — two live generated Room instances across Plugin generations

**Status: Draft implementation; exact-head CI is required before any PASS
claim.** This is stacked on the qualified M0f Orbit PR #17 and uses its
unchanged SaltsUtils #729 compiler artifacts. Do not merge or release.

## Why this gate is separate

M0e proved two isolated Rooms with different state owners, but without
Plugin DSO ownership. M0f proved the canonical generated Context-v1
Dispatcher invoked the original N Provider's real DSO vtable after N+1
publication, but its new N+1 Scope did not own a generated Actor.

M0g combines these two properties **without** treating their independent
passes as proof of dual-generation behavior. It uses two live CFlow
Actor/Executor/Scheduler/CallPort owners from the **same generated**
`Game.Room` service and the existing two real Provider DSOs.

## Existing in-repo CTest: `orbit.ace.dso_native`

With `ORBIT_IDL_CONFORMANCE=ON`:

1. Load original Salts ComponentProvider DSOs for generation 21 (N,
   provider value 37) and generation 22 (N+1, provider value 83).
2. Publish N, acquire its original ServiceHost Scope and create one
   generated Room with the Context-v1 receiver borrowed from N's
   canonical CMeta Provider projection.
3. Publish N+1; acquire the new Scope and create a **second** generated
   Room with its own pre-bound receiver, Actor, Executor and CallPort.
   Both Scope guards must return BUSY while the corresponding
   Actor/CallPort pair remains live.
4. Submit the same generated Ping(5) and Pong(7) from each Room in
   interleaved order. Drive both original executors; N must produce
   **42, 144**, and N+1 must produce **88, 190**. Both Provider
   invocation counters must be exactly two, with no cross-generation
   result or callback.
5. Cancel an accepted N Ping(1) *before* CFlow owner progress.
   It must produce exactly one CANCELLED terminal, a stale token on
   post-commit observation, and no further business invocation on
   either Provider. Cancellation does not generally roll back work
   that has already executed.
6. Stop/destroy N's Actor and CallPort, then release its original
   Scope. N must remain BUSY for early release/drain/Plugin unload
   and become quiescent only after those owners retire. Perform the
   real unload of N.
7. **After the actual N unload**, invoke generated Ping(1) on still
   active N+1. It must return **84**, increment only N+1's invocation
   counter to three, and reject an old stale call token.
8. Close the host, prove N+1 drain remains BUSY while its generated
   Room/Scope is active, stop/destroy its Actor and CallPort, release
   N+1's original Scope, drain and actually unload N+1.

The obsolete manually bound non-IDL DSO CTest path has been removed.
With IDL conformance OFF, the generated DSO test target is not
constructed; the normal Core and original ServiceHost/Scope tests remain.
No new CI workflow, CTest executable, installer/consumer project, Plugin
loader, method ABI, reflection registry or runtime engine is introduced. `Orbit::Core`
remains independent of `Orbit::ACE`.

## Qualification boundary

The generated Dispatcher still invokes canonical application-owned
C business symbols with the one opted-in
`int method(void *receiver, Request const*, Response*)` signature.
Those symbols call a pre-acquired, Scope-pinned CMeta Provider vtable,
which truly executes in the original DSO. The compiler **does not**
emit a DSO implementation body, and this test is not proof of general
per-provider method discovery or deployment.

The staged, deterministic cancellation exercise and interleaved
executor progress do **not** establish simultaneous cross-thread
admission/stop/cancel/reply race safety. Managed VALUE, Streaming,
HTTP/2 RPC, persistent Directory, arbitrary schemas and cross-process
terminal semantics are out of scope.

Exact-head Linux Debug+Release ACE=ON 7/7 and Core-only 4/4, plus
Windows x64 Release ACE=ON 7/7 and Core-only 4/4, must PASS on the
unchanged pinned Salts3/Core0.1 SDK and canonical Linux/Windows
`salts-idlc` host artifacts. SaltsUtils old ComponentProvider DSO
workflows remain independent RED under issue #727 until separately
repaired.
