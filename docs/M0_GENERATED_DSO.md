# M0f — generated Native Context Service across a real Plugin generation

**Status: new stacked Draft source, not a release claim.**

This is the remaining integration proof connecting separately qualified
features without creating a second runtime or a hand-built reflection
path. The original `Game.schema` already defines two unary methods and
uses the canonical SaltsUtils `[native_context(1)]` receiver ABI:

```text
ServiceHost Scope N ── fixed DSO Provider projection
         │                 │
         │          Native receiver (borrowed)
         │                 │
         └─ CFlow Actor ───┴── generated Game.Room __orbit_binding
                   │
            post-commit token
                   │
         generated __orbit_dispatch()
                   │
         canonical Ping/Pong business C symbols
                   │
          original CMeta Provider vtable → actual N DSO method
                   │
             bounded Orbit CallPort terminal
```

A real `orbit_dso_value` projection is acquired **once** from the original
Salts ServiceHost Scope and saved in a stable receiver record. The generator
does not perform per-message service discovery, call `dlopen`, or create a
new CMeta FunctionDesc/FunctionAbi. It directly invokes the same contextual
native symbols admitted in M0e; those functions delegate through the
already acquired, native typed Provider projection, whose vtable lives in
the real Plugin DSO. The provider's DSO remains pinned until CFlow Actor,
CallPort, callbacks and the original Scope are released.

The existing `tests/test_orbit_scope_dso.c` extends its **existing CTest**
instead of adding a project/installer or duplicate workflow. With the
explicit `ORBIT_IDL_CONFORMANCE=ON` flag, it reuses the single generated
`game.service_native.h` produced for the core Room CTest. Otherwise, the
obsolete manually bound DSO test is no longer compiled or run. Turning
the IDL conformance flag OFF excludes this test rather than falling back
to hand-written dispatch.

Expected sequence:

1. Load and start two real strict-FR0 Salts Provider DSOs (N: 37, N+1: 83).
2. Publish N, acquire original Scope, bind CFlow Actor + CallPort + generated
   `Game.Room` receiver from the Scope's canonical Provider projection.
3. Publish N+1. N remains BUSY for generation drain and Plugin unload.
4. Admit generated Ping(5) and Pong(7) via their typed IDL client wrappers.
   CFlow post-commit invokes generated Dispatcher, and the actual N DSO
   getter contributes values, yielding Ping=42, Pong=144. N+1 would
   produce a different value, so incorrect generation routing is detected.
5. Stop Actor, drain outstanding callback credits, destroy CallPort. Scope
   release must be BUSY before this boundary, then succeed afterwards.
6. Drain N and unload original DSO. Retire N+1 independently.

## Boundaries and remaining work

- Receiver state privately owned by two Room instances is separately
  proven by M0e, not inferred from the DSO test.
- This test proves actual DSO Provider method invocation **through** the
  generated Dispatcher and a stable CMeta projection. It does not claim that
  `salts-idlc` produces the DSO implementation body or a complete
  Plugin Provider deployment descriptor; those remain application/provider
  code under the canonical Salts ComponentProvider ABI.
- Streaming, HTTP/2 RPC, TLS/mTLS, durable Directory, managed values and
  cross-process delivery semantics are not established by this local gate.
- Keep all stacked branches Draft until exact-head Linux Debug/Release and
  Windows Release Native CTests report SUCCESS with the same pinned
  SaltsUtils compiler and Salts3/Core SDK. No extra worker, ABI fallback,
  installer/consumer project or new CI workflow is allowed.
