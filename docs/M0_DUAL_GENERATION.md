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

M0h independently qualifies bounded native producer/close/cancel
competition in the existing Core CallPort. The following M0i/M0j gates
also run in this same original generated DSO native CTest, using the
installed Salts4/ServiceHost ABI-2 owner and its exact Native Context-v1
SaltsUtils generator host. The historical experimental ComponentProvider
candidate and manual DSO fallback are deleted, not used as test fallbacks.

### M0i: old-N terminal callback still executing

The first generated N Ping terminal callback is deliberately held open.
A different native thread confirms the original CallPort still holds
`callbacks_inflight=1` and `occupied=2`, verifies N's Scope release,
N generation drain and actual Plugin DSO unload all return BUSY, and
checks the Scope generation remains N=21 even after N+1 publication.
Only after these checks may it release the callback. N and N+1 complete
their original generated calls and separately unload after true quiescence.

### M0j: cancelling a native method already running

The canonical original N Ping native business method pauses *after*
post-commit dispatch enters but *before* its borrowed original N DSO
Provider method invocation. A separate native thread cancels its
still-accepted call exactly once; attempts to release original Scope,
drain N or unload the DSO must all return BUSY. The original business
method subsequently invokes the genuine old N Provider and records its
side effect, but stage_reply returns STALE and no second SUCCESS terminal
may escape. Local cancellation is not transaction rollback. Old N totals
are four admitted, two SUCCESS plus two CANCELLED, three actual Provider
invocations and zero residual CallPort credits, with N+1 isolated.

Owner object destruction remains serial and only after all test helper
threads join. No forced concurrent owner teardown, second scheduler,
fallback generator, new CI/test executable or feature-detect CMake.
These tests do not establish managed VALUE *generated* ABI, Streaming,
remote HTTP/2 RPC, durable COMMIT/ACK or exhaustive ThreadSanitizer proof.

**New exact-HEAD CI gate:** Linux Debug/Release ACE=ON 7/7 each,
Core-only 4/4; Windows x64 Release ACE=ON 7/7 and Core-only 4/4.
No pass claim until these real CTests execute on the Salts4/ABI-2
branch. All PRs stay Draft/unmerged.
