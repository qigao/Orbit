# Orbit architecture — one Service contract, existing runtime owners

**Status:** proposed integration design, not a statement of completed product capabilities.
**Baseline:** standalone first; network/cluster modules are optional. Orbit is the application boundary, not a new scheduler, parser, or protocol engine.

## Architecture

```text
 app: game.schema + game_room.c [+ deployment.json]
                         |
       SaltsUtils salts-idlc (canonical IdlContract)
       generated native types / typed stubs / MethodPlans
                         |
             Orbit::Core — Service façade
                spawn / send / call / reply / stop
                         |
     Salts::Configurator + Salts::ServiceHost
    Component/Provider, Plugin lease and generation
                         |
           Salts::CFlow Actor / Executor
               bounded mailbox / state
                         |
                  optional edges
           +---------------+----------------+
           |               |                |
     Orbit::RPC         Orbit::Event    Orbit::Directory
       CHttp H2           FlowMQ        TurboRaft app FSM
       via CNet         optional MQTT   committed revision
           |               |                |
           +-------- CNet/NativeIO ----------+
                            |
                       TurboDB ORM
               application-owned durable facts
```

A **Service instance** is a dynamic application execution entity. It is not a CMeta Component graph node per Actor, a CHttp connection, or a TurboRaft group. A **ServiceHost generation** is the lifetime/ABI admission boundary for provider code shared by many Service instances. A **Raft group** is an explicit durable consensus unit, not a default cost paid per Service.

## Existing upstream owners

| Concern | One authoritative owner | Orbit's role |
| --- | --- | --- |
| IDL grammar/ContractIR | SaltsUtils IDL / salts-idlc | Consume generated metadata and request upstream codegen where missing |
| Native identity/ABI | Salts CMeta | Bind exact typed methods at activation; no per-message reflection |
| Component construction | Salts Configurator | Reuse provider selection/dependency activation |
| Provider generations/Plugin lease | Salts ServiceHost/Plugin | Pin acquired scopes over accepted calls/streams |
| Service message progress | Salts CFlow Actor / Scheduler | Adapt a generated typed operation to existing bounded mailbox |
| Network and TLS | SaltsCore CNet/NativeIO/Crypto | No sockets, TLS engine, second poller in Orbit |
| HTTP/2 and RPC wire | CHttp | Extend **CHttp upstream** for streaming/protocol capabilities |
| Async inter-node patterns | FlowMQ | Explicit bounded Service adapter; never shift FlowMQ I/O to Actor worker |
| Consensus, WAL, snapshots | TurboRaft | Deterministic Directory **application state machine** |
| Business commit | TurboDB ORM and application | Exact entry identity, applied marker and recovery |

## Execution boundaries

### Local send/call

1. At activation, resolve a generated method descriptor, check canonical FunctionAbi and ownership/capacity, and bind a stable handler. Acquire the correct ServiceHost Scope or source-owned lifetime reference.
2. `send` admits one *owned* message to the existing CFlow Actor mailbox. Its `OK` status means **local admission** only.
3. `call_async` additionally reserves one bounded reply/correlation slot and one terminal obligation. The handler consumes the message only from its configured Actor execution owner.
4. Completion publishes one typed result/error and releases ownership exactly once; cancellation/timeouts race through one terminal state machine.
5. Stop withdraws new admission, drains or explicitly cancels accepted messages, retires reply slots, and only then releases handler/provider Scope.

Do not build a second Actor queue or coroutine scheduler. Current CFlow Actor `try_send` accepts trivially copied payloads; managed native values require an explicitly admitted copy/move/destroy adapter or pre-encoded owned buffer. Never pass a borrowed HTTP/IDL data pointer across callback or thread boundaries.

### Remote call

- Resolve an *immutable* remote endpoint snapshot, validate authority, TLS security profile and required service contract identity.
- CNet/CHttp owns endpoint choice and the already accepted HTTP/2 request/stream; Orbit owns service resolution, typed request/response dispatch and call terminal.
- An accepted H2 request is pinned to the selected endpoint. Do not rehash/redirect a live Stream on Directory change.
- The server copies/decodes request before crossing the H2 owner -> CFlow boundary; accepted work must not block the H2 callback.
- CHttp's current JSON-RPC server handler is synchronous, despite ordinary H2 deferred responses existing. A valid asynchronous RPC bridge needs explicit upstream support or a carefully owned adapter; do not pretend it already exists.
- No implicit H2->H1 downgrade, retry after unknown remote execution, or use of FlowMQ as a fallback transport.

### Asynchronous events

FlowMQ is caller-owner-driven with ROUTER/DEALER/PUB/SUB patterns, HWM, credit and reconnect. Its `send` success never proves remote DATA receipt, application acceptance, or commit. A FlowMQ -> CFlow handoff must own every admitted byte/slice and implement explicit bounded backpressure with no hidden MPSC queue. MQTT ingress is opt-in and governed by broker authentication, ACL, QoS and session semantics; QoS does not imply exactly-once application transactions.

## Interceptors, identity and lifetimes

- **CHttp ingress middleware**: transport authentication, authorization principal, body-size/admission and header-level policy. Never trust an HTTP user-supplied identity in place of a verified peer.
- **CMeta ACE Interceptor**: type-checked Service invocation before/after/error, with provider/Scope lifetime already acquired. Current Interceptor is synchronous; it must not span a suspended asynchronous invocation with borrowed callback state.
- **Generation**: ServiceHost N->N+1 publication changes new-call admission while accepted N callbacks remain pinned. No DSO unload before all call/stream/worker terminal owners settle.
- **Location**: node/boot identity, service/instance identity, provider generation and Directory revision are separate concepts.

## Failure policy

`ACCEPTED`, `RECEIVED`, `EXECUTED`, `COMMITTED` and `APPLIED` are different facts. Expose only what the owner can prove. FULL/STALE/STOPPING errors must remain observable. Transport cancellation cannot undo an already applied business transaction. Unknown commit state must be reconciled against durable identity instead of guessed rollback.

## Dependency policy

`Orbit::Core` may depend on the installed Salts CMeta/ACE/CFlow stack, but **must not** require CHttp, FlowMQ, TurboRaft, TurboDB or MQTT. Optional integration targets use explicit required SDK versions and fail fast on missing/incompatible declared public APIs; no switch to retired Salts/CHttp runtime families.

Source/build work may test in the existing repositories' native graphs, but should not create installer/consumer probe projects, speculative `try_compile` compatibility ladders or redundant GitHub workflows.
