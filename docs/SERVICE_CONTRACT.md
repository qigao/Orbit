# Orbit M0 Service contract — proposed native C11 facade

**Status:** contract proposal, not an installed API. `orbit_*` identifiers in this document are illustrative and must be finalized with the real CFlow/ServiceHost ABI before merging implementation.

## Goal

Match the user-facing ergonomics of Skynet `newservice/send/call`, without introducing Skynet's own scheduler, mailbox, timer, loader or remote transport. A standalone host must be usable without TurboRaft, CHttp, FlowMQ, TurboDB or a network.

## Naming and identity

A logical Service name is resolved to one of:
- a local *instance ref* with `{slot, generation}`; an expired ref returns STALE, never an address reused for a different Service;
- an explicit remote endpoint chosen from an immutable Directory snapshot in the future optional integration.

A ServiceHost **provider generation** is not a Service instance generation. A caller may pin both, but they may not be conflated. A Service's contract fingerprint is immutable for its lifetime; mismatched native FunctionAbi/ownership fails activation, not at first message.

## Proposed public operations

```c
/* Illustrative surface only; no corresponding compiled Orbit library yet. */

typedef struct orbit_host orbit_host;
typedef struct orbit_service_ref {
    uint32_t slot;
    uint32_t generation;
} orbit_service_ref;

typedef struct orbit_call_ref {
    uint32_t slot;
    uint32_t generation;
} orbit_call_ref;

int orbit_service_spawn(orbit_host *, const orbit_service_descriptor *,
                        const orbit_service_config *, orbit_service_ref *out);
int orbit_service_try_send(orbit_host *, orbit_service_ref,
                           uint32_t method_id, const void *owned_message);
int orbit_service_call_async(orbit_host *, orbit_service_ref,
                             uint32_t method_id, const void *owned_request,
                             orbit_call_terminal_fn terminal, void *user,
                             orbit_call_ref *out_call);
int orbit_service_call_cancel(orbit_host *, orbit_call_ref);
int orbit_service_stop(orbit_host *, orbit_service_ref);
```

The exact type of `owned_message` must be replaced by generated typed adapters or a versioned owned buffer. It is **not** a blanket permission to pass arbitrary `void*` across threads. Generated contracts should expose typed wrappers and no runtime Native ABI reconstruction.

`spawn` is application-level Service instance creation. It borrows a preactivated/validated provider capability from Configurator/ServiceHost; it does not build a new Component graph per Actor.

## Critical invariants

### Admission

- All fixed capacities are configured before start and charged before publication.
- A successful `send` transfers ownership of the admitted message to an existing CFlow mailbox; FULL leaves all caller resources owned by the caller.
- A successful `call_async` reserves both message and response-credit accounting (including a terminal outcome). No accepted call silently disappears on mailbox/response-capacity pressure.
- Trivial-Copy requests may use the existing CFlow actor admission path. Owned string, array, union or native resource values require explicit canonical DataBind/CMeta copy/move/destroy lifecycle and an admitted size bound; reject until supported.
- No reentrant blocking invocation while on the target Actor owner. The synchronous facade, if later offered, must be caller-driven or restricted to a compatible external owner.

### Scheduling

- Per-service message delivery is serialized by the selected CFlow Actor execution contract. Serial message handling is **not** a promise of atomicity across coroutine suspension or asynchronous awaits.
- Global fairness, worker ownership and queue progress are delegated to CFlow's existing Scheduler. No private `orbit_worker`, unbounded callback queue or alternate mailbox.

### Terminal

- `call_async` accepted => **exactly one local terminal callback**; the callback may report success, native error, cancellation, timed-out or shutdown.
- A cancellation attempt is a request to settle the local call; it does not imply the business handler never ran or reversed side effects.
- Reply correlation IDs contain an incarnation/generation; a late reply cannot settle a reused slot.
- Terminal callbacks use a precisely specified owner/thread; borrowed response views expire on callback return unless explicitly moved/retained.
- Stop first prevents new admission, then drains or cancels outstanding accepted work, waits for callback/Scope/lease quiescence, and only then permits Service/Provider destruction.
- Provider/plugin scope is acquired before any reference to provider-owned code and is retained until the last accepted terminal/cleanup.

## Error taxonomy

Separate at least: invalid argument, contract/ABI mismatch, FULL/backpressure, STALE, STOPPING, CANCELLED, DEADLINE, and native handler failure. Use canonical Salts status mapping where available; do not invent success after ignored failure. `SALTS_OK` at an admitted boundary is not `COMMITTED`.

## One IDL source

The existing `Salts::IDL` Contract is the only grammar source. `service`, `message`, `channel`, and `component` already exist.

```text
schema Game [id(1001), version(1)];

message JoinRequest { uint64 player_id; }
message JoinResponse { uint64 session_id; }

service GameRoom {
    Join: JoinRequest -> JoinResponse;
}
```

SaltsUtils currently emits NativeSourceIR and generated native Service metadata; it also has an isolated **candidate** for a Component Provider projection that is not production-qualified. A formal `ARTIFACTS SERVICE` target must not be introduced in Orbit. Extend `salts-idlc` upstream to generate the local typed dispatch/client facade against Orbit's fixed published ABI, and fail closed on unsupported call shapes.

## M0 executable acceptance

1. One Service can be created from an admitted generated typed capability, called and stopped without any network library.
2. Two Service refs with reused slots never accept stale messages/replies.
3. Multi-producer send to a bounded mailbox rejects at capacity without orphaned ownership; per-Service ordering follows documented owner rules.
4. A concurrent call cancel, Service stop and reply race publishes one terminal callback and settles every response credit.
5. Strings/managed fields are either properly copied/moved/destroyed exactly once or refused at admission; no fake trivial copy.
6. ServiceHost N->N+1 old-generation Scope remains pinned until all old accepted calls terminal; new calls select N+1.
7. In-place native tests use the existing project test graph. No installer/consumer or redundant CI projects.
