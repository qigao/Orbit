> Historical source/evidence record. The current native SDK requirement and
> Provider ABI migration are documented in [M0 ABI 2](M0_ABI2_MIGRATION.md).

# Orbit M0b — bounded unary CFlow call-port admission

**Status: Draft source, not yet compiled against an installed Salts 3 SDK.**
**Stacking:** M0a `feat/m0-cflow-service-admission` / PR #10 is the review base.

## What exists in this branch

The new `Orbit::Core` `orbit/call.h` + `core/src/call.c` provide a **bounded response-credit/correlation port** on top of the existing CFlow Actor. It does not introduce a second Actor, queue, Scheduler, Executor, timeout engine or network transport.

1. `orbit_call_port_init(&port, &service, capacity)` allocates exactly `capacity` u64 call slots and one Salts Platform mutex, and **retains a complete Orbit producer reference**, thereby pinning both canonical CFlow Actor identity and Core-owned method catalog. No allocation in `orbit_call_u64_async`, reply observation, cancellation or close.
2. `orbit_call_u64_async` atomically reserves one slot, records callback/request and submits a 64-bit **generation-checked token** to one existing CFlow `uint64` Event with `cflow_actor_ref_try_send()`. That Event must be declared `ORBIT_METHOD_CALL_TOKEN`: ordinary `orbit_service_ref_try_send()` explicitly rejects it, so a caller cannot accidentally inject raw `send` traffic into the call path. A CFlow FULL/STOPPING/etc. rejection releases the slot **without invoking a callback** and returns the exact upstream send status.
3. The CFlow Machine Action is a deterministic token projection: it constructs a new Machine state and copies the token into its u64 observation. It neither calls arbitrary business code nor touches the mutable call-port; its output must satisfy the CFlow `DETERMINISTIC | NO_ALIAS` admission contract.
4. On the CFlow `on_value` observation *after Machine commit*, the typed business adapter copies the request from the call slot, executes the native handler and stages its result. The same observer then calls `orbit_call_u64_observe()` to claim one terminal outside the pool mutex. Missing staged reply -> `TERMINAL_NO_REPLY`.
5. Terminal claim transitions `ACTIVE -> COMPLETING`. The slot **cannot be reused while its terminal callback is executing**, and `callbacks_inflight` still holds the associated credit. Only after the callback returns normally is the slot released back to `FREE`; if it blocks, new calls must get FULL instead of creating unbounded callback concurrency.
6. `orbit_call_u64_cancel()` claims a local cancel terminal; while `ADMITTING`, it records intent and waits for the CFlow send admission outcome to avoid callbacks on rejected requests. Cancellation does **not** roll back business actions that already started or committed.
7. `orbit_call_port_close()` seals admissions and settles all active calls as STOPPED or ACTOR_FAILED. Calls still in `ADMITTING` settle after their exact send admission status is known. It is safe for the Actor's done/error callback to invoke close.
8. 32-bit per-slot generation increments and **never wraps**. A fully exhausted slot is retired rather than silently accepting a very late reply.
9. Action `expected_method` must equal the method of the original admitted call. A token from another method cannot read or stage an unrelated result. The port compares both retained Actor identity and original immutable method table; a foreign Actor/catalog is rejected.

## Minimal supported API

```c
orbit_call_port calls = {0};
/* After spawning a real Orbit Service owner: */
orbit_call_port_init(&calls, &service, 16u);

uint64_t token = 0u;
cflow_actor_send_status send;
orbit_call_u64_async(
    &calls, &actor_ref, METHOD_ID, scalar_request,
    on_terminal, user, &token, &send);

/* Existing deterministic CFlow Machine action of METHOD_ID: */
*(uint64_t *)out_observation = *(const uint64_t *)event;

/* Existing Actor on_value callback, after CFlow Machine commit: */
uint64_t token = *(const uint64_t *)value;
orbit_call_u64_request(&calls, token, METHOD_ID, &request);
response = business_method(request);
orbit_call_u64_stage_reply(&calls, token, METHOD_ID, 0, response);
orbit_call_u64_observe(&calls, token);

/* Existing Actor on_done/on_error callback or external shutdown: */
orbit_call_port_close(&calls, ORBIT_CALL_TERMINAL_STOPPED);

/* After Actor/Executor/producer quiescence and CFlow Actor destruction: */
orbit_call_port_try_destroy(&calls);
```

This is a **u64/u64 experimental first slice**. It is not a generic CMeta type system, owned-string serializer, direct RPC or finished `service.call` interface. Those need explicit `salts-idlc` generated typed bindings with exact native Value lifecycle.

## Correctness and ownership

- The token is `(generation << 32) | (index + 1)`, and an older generation never matches a reused slot.
- The port **retains a full Orbit producer reference** through destruction, pinning both the CFlow Actor control identity and the Core-owned method snapshot. It refuses a different Actor or catalog and returns BUSY until the original Actor owner has actually been destroyed.
- Request/reply are plain inline u64. No pointer, callback-scoped CSerde view or nontrivial resource crosses the Actor mailbox.
- Each producer/CallPort reference owns a Core catalog-snapshot credit. The original input method table can be overwritten after spawn. Provider-owned callback code/CMeta type descriptors must still remain leased until **Actor and Port terminal/quiescence**, not just until local callback fires. Old stale producer refs retain only Core catalog bytes/CFlow control identity; full ServiceHost generation pinning is a separate open gate.
- Terminal callbacks are invoked outside the mutex. Their thread is whichever CFlow observation or cancel/close caller wins. A callback can be delivered **before `call_async` returns** when a worker finishes rapidly; consumers must synchronize callback outputs.
- Cancelling a call does not undo an already committed CFlow Machine transition or a business handler already running in the post-commit observer. Successful RPC/DB COMMIT is a different, stronger fact and is **not claimed**.
- An actor that fails/stops before emitting an observation must explicitly call `orbit_call_port_close` on on_error/on_done; otherwise no terminal is justified. An application must never free the port before the Actor and all completion/producer callers quiesce.
- The port mutex uses `Salts::Platform`; the CFlow Executor and Scheduler still belong to `CMeta::CFlow`. No global mutable registry, hidden progress thread or additional mailbox exists.
- Pre-bound `ORBIT_METHOD_CALL_TOKEN` and the CFlow producer ref are **accidental-use isolation**, not an authorization sandbox: callers with raw `cflow_actor_ref` access are already within the trusted native integration boundary.
- Terminal callbacks must return normally and must not perform port or Actor destruction themselves. `occupied` counts `COMPLETING` slots, and `callbacks_inflight` measures the credits awaiting callback return.
- This M0b post-commit business adapter requires a **single serialized CFlow Subscriber/Actor execution owner**, as in the native owner-affine test configuration. A generic worker Scheduler that can invoke observers on unrelated threads is not automatically an Actor state owner; broader execution-policy admission is still pending.
- `orbit_call_port_try_destroy()` is nonblocking, returns BUSY while the bound Actor is not destroyed or while submitting/closing/callback credits remain, and requires quiescence of all external API users as an explicit precondition. It releases both the retained upstream CFlow producer identity and Core catalog snapshot only on successful destruction. This is not a magic cancellation or stream-owner wait.

## Existing native conformance source

`tests/test_orbit_call.c` runs a real CFlow Machine Actor with a single owner-affine CFlow Executor and Scheduler, no mocked send path. Its cases exercise:

1. One Event accepted, second rejected by CFlow Mailbox FULL, no callback on rejected admission.
2. Post-commit success, duplicate/stale stage/observe rejection, different method-ID rejection and an observed method with no staged reply.
3. Two accepted calls at fixed capacity, third rejected by response-credit FULL; cancel one before action, observe a typed business error for the other and prove stale-token reuse cannot overwrite new result.
4. Close while a token is queued; STOPPED exactly once even when later Actor observation arrives.
5. A **real second native thread** cancels an accepted call while the CFlow owner is executing its deterministic Machine action. The later observation refuses the stale token and invokes no business handler; only CANCELLED is delivered.
6. Callback reentrancy: a terminal callback can safely re-enter port stats/cancel because no callback runs while its port mutex is held.
7. Action-output contract: CFlow's actual Machine validation requires `CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS`; arbitrary native business effects belong only in the post-commit observer, not Machine Actions.
8. A blocked terminal callback holds the last response credit; concurrent CFlow producer admission receives `ORBIT_CALL_FULL`, then succeeds after terminal callback return.
9. Call-only token methods reject generic `send`; a foreign Actor, forged method catalog, unknown flag or non-u64 call-token event is rejected before publishing work.

All CTest checks remain active under `NDEBUG`. Tests must be built/run with an **actual installed Salts 3 SDK**; source tests are not evidence of a passing build.

## Open M0/M1 gates

- Resolve present Salts 3 immutable SDK/SaltsCore versions and compile the exact PR HEAD, Debug/Release, Windows/Linux, with the real public ABI. No old Salts fallback or synthetic substitute headers.
- Full pointer/owned-buffer request/response generated from canonical `salts-idlc` and DataBind lifecycle; **do not** use u64/pointer casts to smuggle managed state into CFlow.
- Integrated ServiceHost Scope pinning across local Actor lifetime and replies remains open. Borrowed provider code/descriptors must outlive Actor+CallPort; Core-owned method-catalog bytes remain separately retained until the final stale producer ref settles. The old borrowed input table is no longer a producer-lifetime dependency.
- Additional ABA stress and callback reentrancy/stop/admit race tests, bounded deadlines (no new timer), stats and exhaustive cleanup.
- Full `Orbit::Service` unified `spawn/send/call/reply/stop` contract, H2 integration and Directory remain pending.
