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

## M0i: live terminal callback pin (separate strict race gate)

The M0g/ M0h deterministic admission proofs cannot by themselves prove
that an outstanding **generated callback** pins the original Plugin
generation during concurrent release attempts.

The **same existing** \`orbit.ace.dso_native\` test now blocks the first
generated N Ping terminal callback *after* the actual N DSO method returns,
while the original Salts CFlow observer is still delivering that terminal
and holds its preallocated CallPort callback credit. An independent native
thread (using the existing \`Salts::Platform\` threading API) verifies:

- \`callbacks_inflight=1\`, \`occupied=2\` with N's Ping terminal blocked and
  N's second Pong call accepted but not processed;
- \`orbit_ace_scope_guard_try_release(N)\` returns BUSY;
- \`salts_service_host_generation_drain(N)\` returns BUSY;
- \`cmeta_plugin_registry_unload(N)\` returns BUSY;
- the original Scope still reports generation 21, never N+1.

Only after this witness may the worker release the terminal callback
barrier. The normal generated N/N+1 dispatch test then resumes and still
requires the original N to drain and truly unload **only after** Actor,
CallPort and Scope obligations retire, while N+1 continues and later
unloads independently.

The worker does not mutate or destroy the Actor, CallPort, Scope, or Plugin
while reading their state. All owner-side teardown remains serialized
after the worker joins, consistent with the published Scope API.

This qualifies a **specific live-callback vs release/drain/unload race**,
not concurrent destruction of the same owner objects, all possible
scheduler interleavings, full cross-thread CFlow business progress,
managed values, Streaming, remote RPC or ThreadSanitizer. It adds no
runtime callback queue, alternate Provider ABI/loader, test executable
or CI workflow. Prior legacy Candidate and handwritten DSO paths stay
deleted.

## M0j: cancellation after native function entry

The M0g test cancelled *before* CFlow dispatch; M0i blocked a terminal
callback *after* the real DSO method returned. Neither proves the
in-between case where a generated native business method has started
but a local cancellation arrives before its borrowed DSO vtable call.

M0j extends the **same** \`orbit.ace.dso_native\` CTest (no new executable
or workflow) with an opt-in test barrier within the **existing canonical
Native Context-v1 Ping business symbol**, immediately before its actual
\`orbit_dso_value_get\` DSO call.

- One N Ping(1) is accepted while N=21 and N+1=22 generated Rooms remain
  alive; the CFlow owner enters the real generated Dispatcher and pauses
  *inside* the original application-owned Ping native method.
- A separate Salts Platform thread sees \`occupied=1\`, then cancels the
  accepted token. It verifies **exactly one** CANCELLED callback, stale
  repeat cancellation, old Scope release BUSY, generation drain BUSY and
  real old Plugin unload BUSY. The callback gives no response.
- Only after these checks is the business function allowed to call the
  pinned **real N DSO** vtable. The native function completes and records
  its side effect, but \`stage_reply\` must return STALE and **must never
  publish a SUCCESS callback** on that already-settled call.
- Resulting old-N totals: four admitted calls, two SUCCESS and two
  CANCELLED local terminals, zero occupied credits; three real N provider
  calls (the cancelled-before-dispatch call does not execute business,
  the cancelled-after-entry call does). New N+1 provider remains
  isolated, and existing true N unload/N+1 continuation is preserved.

This is intentional: a local cancel is a **terminal decision**, not a
transactional rollback or a guarantee that already-started application
logic is never executed. Therefore the original ServiceHost Scope must
continue to pin the DSO until Actor/CallPort and all native work drain.
No new provider implementation, dynamic ABI, Registry, loader,
alternative dispatcher, transport fallback or scheduler is introduced.

The test does not establish all possible stop/close/cancel races while
two threads are executing user business callbacks, arbitrary managed
values, remote exactly-once or durable commit/ACK. Both fixtures keep
explicit address-stable owner/Scope lifetimes until all workers join.
