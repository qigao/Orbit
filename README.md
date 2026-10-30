# Orbit

**IDL-first C11 actor and distributed service framework, built on Salts.**

Orbit is the **application-facing service composition layer** for the Salts ecosystem. Its goal is to make a native service as straightforward to create, send to, call and observe as a Skynet service—without duplicating the runtimes that already exist.

> **Status: architecture/bootstrap. No Orbit runtime implementation or CI qualification yet.**

## Architectural rule

One logical contract, one execution owner and no hidden fallback:

- **SaltsUtils `salts-idlc`** owns the sole IDL parser, canonical `IdlContract`, native type/function bindings and artifact generation. Orbit must not ship a second IDL compiler.
- **Salts CMeta/ACE** owns Component/Plugin/Configurator/ServiceHost, typed Interceptor, provider generation and Scope/lease lifetime.
- **Salts CFlow** owns Actor mailbox, scheduling, reactive demand and execution. Orbit must not implement an independent Actor engine, mailbox or scheduler.
- **SaltsCore CNet/NativeIO** owns network transport, TLS and I/O progress.
- **CHttp** owns HTTP/2 connection/stream state, JSON-RPC and future RPC Streaming protocol implementation.
- **FlowMQ** owns asynchronous inter-node messaging and transport credit/HWM; it is not an Actor worker or a durable message log.
- **TurboRaft** owns consensus, membership changes, WAL/snapshot and ReadIndex. A service directory, when added, is an **Orbit-owned application state machine** on TurboRaft, not a second consensus engine.
- **TurboDB** owns application transactions, persisted applied markers and recovery.

Orbit integrates those owners through explicit bounded adapters; network and cluster dependencies are **opt-in** and must not appear in the standalone core's link graph.

## Developer experience

The target experience is one service contract, one native implementation and optional deployment configuration:

```text
game.schema             # logical service, message, channel, component contracts
game_room.c             # business implementation
deployment.json         # optional cluster topology, limits and endpoints
```

`salts-idlc` generates typed native interfaces and, as features qualify, service adapters, RPC bindings, streaming adapters and discovery metadata. Business source remains caller-owned and is never overwritten by generation.

## Incremental delivery

| Stage | Goal | Gate |
| --- | --- | --- |
| M0 | Standalone `spawn/send/call/reply/stop` over CFlow and Salts ACE | Owned payload, bounded mailbox, stale refs, cancellation, exact-once terminal |
| M1 | CHttp HTTP/2 unary RPC to local Actor/Service via deferred completion | Owner isolation, deadlines, no retry/fallback, typed ABI |
| M2 | HTTP/2 Server Streaming, then client/bidi | Per-stream credit, half-close, cancel, Scope drain, sibling isolation |
| M3 | TurboRaft-backed service directory with Resolve/Watch | Committed revisions, immutable snapshots, reconnect gaps, fencing |
| M4 | FlowMQ and optional MQTT adapters | Bounded handoff, identity/ACL, no delivery/commit confusion |
| M5 | Real applications and performance comparison | Same-head CI, error/cancel/crash coverage, service ergonomics vs Skynet |

## Design constraints

- **No second framework runtime** for Actor, Configurator, Scheduler, Reactor, IDL, RPC protocol, Raft or database.
- **No implicit dynamic transport fallback** or hidden delivery retry. `ACCEPTED` only means the specified boundary accepted work; it does not imply remote delivery or durable commit.
- Every publication and callback documents its **owner, capacity, generation, borrowed/owned values and terminal conditions**.
- Fast-path dispatch uses generated and pre-bound CMeta/FunctionAbi adapters, never runtime reflection lookup per packet.
- Existing CI/test graphs are the only acceptance owners. No new installer/consumer probe projects or speculative CMake discovery gates.
- No release/merge based on documentation or CI from another HEAD.

Architecture details and API contracts are developed in Draft PRs and tracked in Issues before production cutover.

## Upstreams

[Salts](https://github.com/qigao/salts) · [SaltsUtils](https://github.com/qigao/salts-utils) · [SaltsCore](https://github.com/qigao/salts-core) · [CHttp](https://github.com/qigao/chttp) · [FlowMQ](https://github.com/qigao/flowmq) · [TurboRaft](https://github.com/qigao/turboraft) · [TurboDB](https://github.com/qigao/turbodb) · [Skynet](https://github.com/cloudwu/skynet)
