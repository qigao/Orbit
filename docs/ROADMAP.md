# Orbit development and qualification ledger

## Status legend

- **Existing upstream**: verified in corresponding source, not necessarily compatible with every newest SDK.
- **Orbit design**: described here; no runtime implementation claim.
- **Pending**: requires code + exact-head native tests + integration proof.

| ID | Work | Current status | Owning implementation |
| --- | --- | --- | --- |
| M0 | `spawn/send/call/reply/stop` local typed Service | Orbit design | Orbit Core adapter + existing CFlow/ServiceHost |
| IDL0 | `IdlContract` -> generated typed Actor stub/dispatch | Partial upstream NATIVE projection; Orbit adapter pending | SaltsUtils `salts-idlc` |
| M1 | HTTP/2 unary RPC -> Actor deferred completion | H2/JSON-RPC exist upstream; bridge pending | CHttp RPC and Orbit adapter |
| M2 | Server streaming, then client/bidi | H2 body source/sink exist; RPC stream pending | CHttp H2 + SaltsUtils generated method |
| M3 | Resolve/Watch Directory state machine | TurboRaft primitives exist; Directory pending | Orbit Directory using TurboRaft |
| M4 | FlowMQ/MQTT application messages | FlowMQ/MQTT existing libraries; Orbit adapter pending | Orbit Event module |
| M5 | Real game-room acceptance and benchmarks | Pending | Orbit existing native tests + application fixtures |

## Dependency readiness

- The current `salts-idlc` supports canonical IDL `service/channel/component/message`, NativeSourceIR, NATIVE/PLUGIN, RPC/HTTP MethodPlan and FlowMQ projections with strict capability limits. In particular, do **not** assume generated RPC handles arbitrary structured egress or streaming.
- CFlow's current Actor is based on Machine/Statechart, not itself a generic dynamic Skynet service host. Prove the adapter can represent that mapping without private Actor scheduling.
- ACE Configurator/ServiceHost is for a provider Component graph and Plugin generations, **not one Component per spawned Actor**.
- CHttp already supports H2 unary JSON-RPC and ordinary H2 deferred response; the JSON-RPC request callback is currently synchronous and has no streaming method contract.
- TurboRaft exposes consensus/ReadIndex/Joint Consensus/FlowMQ peer service; a Service Directory is not yet in the runtime.
- Existing TurboRaft CHttp ControlPlane interface references older CRPC symbols: fix real upstream API/SDK compatibility without adapter fallback.

## Phased gates

**M0** needs a real, installed CFlow/ACE SDK version, a single runtime owner and exact type/lifetime semantics; test local calls, FULL, STOPPING, STALE, per-call terminal, cancel/close contention, generation drains and no leaked managed fields.

**IDL0** needs generated C11 and C++17 ABI declarations, correct generated native fingerprints/provider leases, compile-negative unsupported shapes, deterministic outputs and no overwrite of user business source.

**M1** needs two processes and a verified H2 TLS connection, no blocking protocol owner, independent concurrent H2 requests, deadlines/cancel/reply and no implicit retries.

**M2** needs record framing, end-of-input vs cancel, independent per-stream backpressure, sibling progress, plugin lifetime through stream terminal, H2 RST/GOAWAY.

**M3** needs 3-node actual Raft with leader change, snapshots, compaction, ReadIndex, Directory revision gap/resync, stale-epoch write fencing and static seed bootstrap.

**M4** needs identity authorization, bounded send/receive admission, disconnect handling, no hidden dispatcher worker, no false exactly-once claim.

**M5** needs a small native application built in this repo's normal test graph and fair Skynet vs Orbit comparisons for payload sizes, Actor counts, concurrency and p50/p99 CPU/memory. Durable replication benchmarks are separate.

## CI policy

No redundant GitHub Actions graph, installer/consumer project, or speculative CMake probing. Integrate focused test sources with Orbit's real native targets once they exist. Cache real toolchain/dependency work, cancel superseded runs per branch, and record exact HEAD plus passed jobs. Upstream native fixes are tested in their owning repositories. Keep PRs Draft until evidence is complete; do not block an unrelated branch on missing macOS runner unless its own release gate requires it.
