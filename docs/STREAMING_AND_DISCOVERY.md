# Orbit streaming and directory — staged design

**Status:** future opt-in integrations. These are not current Orbit public capabilities.

## HTTP/2 RPC streaming

CHttp already owns H2/HPACK/session/stream tables, flow-control windows, RST_STREAM, GOAWAY, body source/sink and bounded deferred ordinary responses. The existing generated JSON-RPC server callback is synchronous and its RPC does **not** implement streaming. H2 deferred streaming is explicitly outside the present public CHttp API.

Orbit must **not** implement a private H2 frame protocol or upgrade arbitrary JSON-RPC calls to streams.

### Contract extension (owned by salts-utils)

```text
/* Proposed, currently unsupported grammar: */
service Agent {
    Run: RunRequest -> RunResponse;                     // unary
    Generate: GenerateRequest -> stream<Token>;          // server-stream
    Upload: stream<Chunk> -> UploadResponse;             // client-stream
    Converse: stream<ChatMessage> -> stream<ChatMessage>; // bidi
}
```

This extends canonical `IdlOperation` with a cardinality, not another schema parser. Generated MethodPlans/typed adapters depend on independently versioned CHttp public streaming support; unsupported shapes must fail generation/mount.

### Execution and wire boundary

```text
CHttp H2 owner <-> one bounded cross-owner bridge <-> CFlow subscription/Actor
     stream slot + generation               typed messages + demand
          (CHttp owns)                        (CFlow owns)
```

- First slice: **Server Streaming** plus `Directory.Watch`, not bidi.
- Explicit versioned media type and record framing, independent of H2 DATA frame boundaries. A single record may span multiple H2 DATA frames.
- H2 retains control of stream IDs/HPACK/WINDOW_UPDATE; generated DataBind record codec owns the application payload format. Do not claim that streaming JSON-RPC or protobuf/gRPC is already supported.
- On initial HTTP request, acquire a ServiceHost generation Scope and reserve bounded stream/response credits. The stream keeps that old generation pinned until terminal, even after N+1 publishes.
- H2 owner never blocks on Actor execution; use one explicitly bounded cross-owner work transfer and a generation-checked wake. Borrowed request/body views never escape a callback.
- `try_send` success is bridge admission, not network delivery. Half-close closes one direction only; cancellation, connection close, shutdown and deadline race to exactly one **local** terminal.
- The client owns an immutable selected endpoint and TLS/authority identity for a live stream; Directory revisions affect **future** calls, not existing H2 streams.

### Backpressure

The flow-control chain is CFlow downstream demand -> bounded bridge bytes/items -> H2 stream receive/send windows -> H2 connection window -> CNet retained completion.

Do not publish WINDOW_UPDATE for an item not actually consumed/admitted by the receiving owner. The current H2 parser's DATA_PAUSE includes connection-level input-paused state; specifically qualify that one slow Stream does not indefinitely block siblings. This must be fixed in CHttp, not papered over with Orbit buffering.

Required tests: slow subscriber and sibling stream, full bridge/credit, decomposed DATA record frames, half-close, zero-length terminal, RST/GOAWAY, concurrent cancel/stop/reply, stale stream handles, Plugin N->N+1 drain, actual TLS ALPN h2. Memory has hard per-stream and per-connection budgets.

## Service directory

Directory is an **application state machine on TurboRaft**, distinct from Raft membership. A Raft voter is not necessarily a business Service host. A local ServiceHost generation is not a cluster-wide committed directory revision.

### Durable record

```text
ServiceEntry {
  name, contract_fingerprint, interface_version,
  node_id, process_boot_id, instance_id,
  endpoint_id, uri, authority, tls_identity,
  service_generation, shard_id, fencing_epoch,
  lifecycle_state, routing_weight
}
DirectorySnapshot { committed_revision, entries[] }
```

Proposed lifecycle:
```text
PREPARING -> SERVING -> DRAINING -> REMOVED
```

An instance is not marked SERVING merely because the Configurator created it; readiness must be acknowledged after activation and published through a committed Directory command. Status snapshots and health metrics should not write to Raft on every transient heartbeat. A server refusing an old `fencing_epoch` must enforce that rule **at the authoritative business mutation boundary**, not just the routing cache.

### Resolve/Watch

- `Directory.Resolve(name, shard)` returns a **committed** immutable revision and endpoint set, with explicit consistency/read-policy (ReadIndex when linearizable semantics are required).
- `Directory.Watch(from_revision)` is a CHttp **server stream** from a retained revision; its deltas carry `previous_revision` and `revision` so receivers can reject gaps.
- If a revision is compacted, the client fetches a new full snapshot. Watch queues are bounded; slow subscribers terminate with an explicit resync-required status rather than consuming unbounded memory.
- Client uses immutable validated snapshots and CNet's existing `cnet_destination_choose()`. Chosen `endpoint_id` and `snapshot_generation` become H2/FlowMQ connection-admission inputs; local owner placement is independent.
- Static/bootstrap control endpoints are configured outside Directory to avoid cyclic discovery.
- A stale directory or lost quorum does not magically authorize stateful writes; the application selects a fail-closed vs explicitly allowed stale-read policy.
- Do not add per-call Raft round trips to ordinary stateless requests. Keep data plane off the consensus hot path.

### Raft/HTTP integration gates

TurboRaft currently still has ControlPlane references to the prior standalone `crpc_server` types whereas CHttp master exposes `chttp_server_rpc_configure()` attached to `chttp_server`. Also confirm Salts/SaltsUtils SDK epochs; do not hide this with a compatibility API. Fix the authoritative upstream compatibility first, then integrate Directory.

TurboRaft already has FlowMQ peer service, Joint Consensus, ReadIndex, snapshots and an application-owned TurboDB ORM state-machine transaction protocol. Directory should consume these, without a second WAL, cluster membership service or database adapter.

## Not implied

Neither HTTP/2 stream success nor FlowMQ send implies application-level durable delivery. Raft commit does not automatically prove downstream TurboDB business apply. A disconnected old shard owner may still run, so fail-closed fencing must be enforced at the final business mutation authority.
