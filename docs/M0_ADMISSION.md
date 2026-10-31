> Historical source/evidence record. The current native SDK requirement and
> Provider ABI migration are documented in [M0 ABI 2](M0_ABI2_MIGRATION.md).

# M0a — verified source boundary, open executable gates

**This document describes a new source implementation, not a successful build, CI run, installed SDK or completed M0 service.**

## Implemented in this branch

- `Orbit::Core` is a static C11 library. Its entire native execution depends on `Salts::CFlow`.
- `orbit_service_spawn()` validates a sorted method-ID catalog against the already-built `cflow_machine` Event schema, allocates **one Core-owned immutable snapshot** of that catalog, then creates and starts **one genuine CFlow Machine Actor**. This snapshot outlives the originating input array.
- `orbit_service_ref_acquire/retain/release` retain the existing `cflow_actor_ref` **and** an independently refcounted Core-owned catalog snapshot. A retained producer can safely return `STALE` after the Service root and original input table have gone away without dereferencing unloaded method-table storage.
- `orbit_service_ref_try_send()` resolves a stable method ID through a bounded binary search and makes **one** `cflow_actor_ref_try_send` attempt. It allocates nothing and does not build a second mailbox/dispatcher. Payload admission is the CFlow trivial copy and rejection retains caller ownership.
- Stop, wait, stats and destroy forward to the real CFlow actor owner. After owner destruction, producer refs retain the CFlow generation-stale mechanism. The final Core catalog allocation is freed only when all root/producer/CallPort references settle.
- A native in-repo CTest (not a separate consumer/install project) uses one real owner-affine CFlow Executor and one CFlow Scheduler, and tests wrong/unknown methods, copied input, FULL, completion, stop and stale refs.

## Exact compatibility boundary

The upstream currently inspected is Salts 3 `master` with `Salts::CFlow`/CMeta/ServiceHost and a public `cflow_actor_ref_try_send` that accepts only CMeta trivially-copyable and trivially-destructible Event payloads. The new CMake requires `find_package(Salts 3.0 CONFIG REQUIRED)`. It must not silently compile with the previous Salts 2.2/2.3 epoch.

No installed Salts 3 SDK is present in the current authoring environment; **the branch has not yet been linked or run against Salts 3**. No GREEN CI claim is made. The first real executable gate is configure/build/CTest with an immutable Salts 3 SDK and its transitive SaltsCore/Crypto dependencies. Missing SDK is an explicit blocker, not permission to add a fake proxy header, second runtime or compatibility switch.

## Stacked M0b call implementation

A separate [M0b Draft PR #11](https://github.com/qigao/Orbit/pull/11)
builds on this M0a baseline: fixed-credit u64 unary calls over the exact
CFlow Actor event, post-commit CFlow observation, generation-checked
correlation and callback-credit fencing. Its call-only method-kind metadata,
retained upstream CFlow Actor identity and COMPLETING terminal-credit
ownership are separate source changes; they are not merged into M0a or
verified with a real installed Salts 3 SDK. See
[the M0b contract](M0_CALL_ADMISSION.md).

## Deliberately not implemented

- `call/reply/timeout` correlation, generation-checked reply slots or exactly-once **per-call** terminal callbacks. CFlow Actor's `on_value` observation is not a replacement for these obligations.
- `salts-idlc` generated typed Orbit stubs and dispatcher. The method table in this slice is a C11 initializer backed by an existing Machine schema; IDL0 must produce it upstream.
- A dynamic general-purpose Actor class. This slice borrows a *pre-built* CFlow Machine and its bound action callbacks. This preserves CFlow's actual executor/state semantics.
- Provider/Plugin Scope acquisition and N->N+1 ServiceHost generation retire. Caller must hold providers through live Actor/CallPort work, including CMeta type descriptors. The immutable Core catalog snapshot itself no longer keeps the Plugin module borrowed after Actor destruction; optional ACE Scope pinning is tracked separately.
- Owned strings, lists, buffers or other managed Event payloads; all such shapes must fail admission instead of pretending `memcpy` is a lifecycle operation.
- CHttp RPC, Streaming, Directory, FlowMQ, MQTT, TurboRaft or TurboDB dependencies.

## Native qualification gate

1. Build with the real Salts 3 SDK and exact headers, not an abbreviated hand-written stub.
2. Run `orbit.core.native` from the repository test graph with `BUILD_TESTING=ON` in Debug and Release; no assertion removal under `NDEBUG`.
3. Compare verified CFlow status/owner semantics for FULL, STOPPING, STOPPED and STALE.
4. Exercise multiple methods, concurrent producers and stop/destroy races. This slice snapshots validated method rows and separately retains their lifetime; caller remains responsible for CFlow Actor and CMeta provider quiescence.
5. Add ABI/ownership negative tests and, later, real downstream app acceptance; do not create install/consumer probe projects or duplicate CI workflow.
6. Keep this PR **Draft and unmerged** until a real native green build/test trace matches its exact HEAD.

## Next implementation steps

- Define the canonical CMeta native operation ABI and reserve a bounded call response slot *before* Actor admission.
- Reconcile accepted but cancelled/queued calls without silently dropping terminal obligations; do not make callbacks before authoritative CFlow commit.
- Attach Salts ServiceHost Scope and Plugin generation to accepted calls and callbacks; call/stream old generation may retire only after it settles.
- Extend `salts-idlc` upstream with the typed Orbit stub/adapter projection, without a second parser or custom source generator.
