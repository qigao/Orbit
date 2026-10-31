# M0e — canonical instance-bound Native Service

**Status: Draft code and validation pending.** This is an independent
product-level acceptance on top of the already-qualified scalar/unary
generated C11/C++17 Service (Orbit M0d PR #14).

## Binding contract

A Service is defined once by the canonical SaltsUtils IDL:

```text
schema Game [version(1)];

message PingRequest  { uint64 seq; }
message PingResponse { uint64 ack; }
service Room {
    [orbit(10,100), native_context(1)] Ping: PingRequest -> PingResponse;
    [orbit(20,101), native_context(1)] Pong: PingRequest -> PingResponse;
}
```

One and only one NativeSourceIR method ABI is used. The same canonical
business C symbol now receives an explicit `void *receiver` before the
existing Request and Response parameters. This is the receiver bound in the
**original DataBindNativeExecution.context**, not a new reflected argument,
global service variable, per-message method lookup or alternate Orbit
handler ABI.

The generated `__orbit_binding` is a two-pointer, application-owned
activation record `{ CallPort*, receiver* }`. It is initialized exactly once
after creating the real Orbit Service and CallPort. The generated post-commit
`__orbit_dispatch(&binding, token)` uses the original method identity,
generation-protected call slot, and canonical Native business function to
operate only on the bound instance.

The deterministic CFlow Machine Action continues to project the token.
Business state writes happen exclusively in the original CFlow post-commit
Subscriber owner; acceptance via the CFlow mailbox does not imply a durable
transaction, and cancellation does not roll back already run business code.

## Exact owner and lifetime boundaries

- `Orbit::Core` still holds one real CFlow Actor and a bounded CallPort per
  instance, with a Core-owned immutable method catalog and generation-checked
  tokens. There is no hidden worker, scheduler, event loop or second mailbox.
- A receiver and its configuration are application-owned and must remain at
  an address-stable location until the last accepted callback and CallPort
  terminal have completed. Never borrow a temporary stack object past its
  owner lifetime; never use global/TLS `current_room`.
- A Plugin-owned receiver is additionally pinned by the original
  `Orbit::ACE` / Salts ServiceHost Scope until Actor and CallPort quiesce.
  Publication N->N+1 does not rewrite a still-running old binding.
- Unannotated old Native Services keep their previous business signature.
  `native_context(1)` is a *versioned opt-in* on the same NativeSourceIR.
  Invalid/mixed receiver versions and null receiver admissions fail closed.
  The older non-bound typed CFlow Native adapter explicitly rejects the
  contextual shape instead of installing a fallback.

## Native M0e product acceptance

`tests/test_orbit_generated_idl.c` uses the actual pinned `salts-idlc`
generated header and tests one stateful Room with typed Ping/Pong, native
application status, cancellation and terminal credits.

`tests/test_orbit_multi_room.c` is **another source file of the same
existing CTest executable**, not a separate project: two independent Rooms
are spawned from one IDL schema with private counters initialized to 5 and
100. Calls interleave on their respective real CFlow owner/Executor, and
results must be A=(8,111) and B=(103,207). Cancelling A's next call must
leave A unchanged at 111, while B's subsequent request reaches 209. They
share no writable data, and all native owner resources drain explicitly.

`tests/test_orbit_generated_cpp17.cpp` compiles the same canonical header
as C++17, constructs a real receiver and calls the exact C11 business symbol,
proving no duplicated cross-language method ABI.

## Open independent gates

- Exact-head SaltsUtils Native compiler CI and exact-head Orbit Linux/Windows
  real installed-SDK generated test must pass together.
- Direct canonical DataBindNativeExecution receiver binding must be
  executable, not just inspected as emitted text.
- The **same generated** receiver-bound Dispatcher still needs a real
  Plugin DSO N->N+1 retained Scope/unload test. The earlier Orbit M0c
  DSO test is a separately verified manual business adapter, not this
  generated method.
- No managed/optional/nullable/Streaming shapes until their unique
  upstream ownership, DataBind lifecycle and CFlow credit contracts qualify.
- Both stacked PRs remain Draft until the declared exact-head tests are GREEN.
  No automatic merge/release and no installer/consumer/CI-probe projects.
