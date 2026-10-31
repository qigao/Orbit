# M0k.2a — bounded managed VALUE slot on the original Orbit CallPort

**Draft / no merge or release.** This implements a first bounded CMeta VALUE
slot within the *existing* `Orbit::Core` response-credit owner. It is NOT
general generated managed `salts-idlc` Client/Dispatcher admission yet.

## One owner, not one extra runtime

The CFlow Mailbox still only transports a uint64 generation-checked token.
The existing CallPort adds a *single immutable owning VALUE contract* bound
at activation: method ID, canonical request/response CMeta DataDesc, and
explicit separate maximum payload-byte budgets (1–65536 each). Descriptors
are borrowed and remain Scope-pinned through Actor and Port destruction.

Only canonical owning STRING/BYTES BufferOps v2 and fully reflected VALUE V2
structs with primitive/owned STRING/BYTES leaves are admitted. Nested
records, collections, enum/variant, VIEW, nullable/presence overlays, unknown
traits or unbounded budgets fail closed; descriptors are never synthesized
by Orbit. This uses canonical `cmeta_data_value_copy`,
`cmeta_data_value_restore_zero` and `cmeta_data_temp_open/close`, not
an Orbit owned-buffer implementation.

At init, correctly aligned native record storage is allocated once per
preallocated Port slot and initialized to semantic zero. Admission validates
the original managed bytes against a total dynamic-byte limit and copies the
request into the reserved slot via the original CMeta provider *before*
submitting the token. FULL, CLOSED, malformed, budget-exceeded, copy failure
and upstream actor rejection leave token zero and deliver no callback. The
original user request remains owned by the caller.

## Independent callback / execution pins

```text
FREE -> ADMITTING -> ACTIVE
                    -> executing borrow acquired by post-commit Actor owner
ACTIVE -> cancellation/close -> one local terminal callback
         if execution held, TERMINAL_RETAINED (no slot reuse!)
         callback exit AND executing borrow release
         -> provider restore_zero -> FREE for next generation
```

A native business function calls `orbit_call_value_begin` only from the
original post-commit owner. The borrowed `const void*` is pinned by an
independent one-per-slot executing credit. Cancel/close can publish a local
terminal while the method is running but **cannot** destroy or recycle that
request. After native business exits it must call `orbit_call_value_end`,
even if a cancellation already won; an old method cannot settle or stage a
late reply. A successful response is deep-cloned with the exact response
DataDesc; callback borrows that response only during its invocation, and
its slot is not freed until callback exit. No callback under the Port mutex.

All standard u64 calls remain supported. The configured managed method
rejects scalar `orbit_call_u64_async` as TYPE_MISMATCH, so a generated
scalar wrapper can never silently borrow managed storage. Unknown and
unsupported schemas still fail closed in `salts-idlc`.

## Current narrow acceptance

The **existing** `orbit.core.call_native` exercises a test-local, fault-injectable CMeta BufferOps-v2 owned byte provider (raw 0x00 and 0xff included) over a real
CFlow Actor. It proves deep copy despite caller mutation/destroy, full credit
and byte-budget rejection, exactly-one response callback, and a second
thread cancelling *during* a post-commit native business borrow. It checks
CANCELLED followed by a real business read of still-live request bytes,
stale reply, occupied/exec credit retention and only-later credit reuse.

Still unproven: generated managed wrappers for mixed text/bytes records,
per-method heterogeneous contracts beyond one managed method per Port,
real N/N+1 DSO Scope retirement with managed callbacks, arbitrary CMeta
nested VALUE, adversarial provider reentry and exhaustive allocator-failure paths, remote transport, Streaming and durable COMMIT/ACK.

Before enabling generated `[orbit]` managed methods, the authoritative
SaltsUtils NativeSourceIR must project those typed wrappers through this
Port API and tests must prove negative compilation/admission and two-DSO
lifecycle. No fallback, additional Actor/scheduler, Plugin loader, second
parser, new test executable/CI graph, or installed-consumer project.

## SDK ABI-2 qualification

This clean downstream branch is based on the native Salts 4 / ServiceHost
ABI-2 migration, using the existing real Plugin/Scope owner and no legacy
ComponentProvider bindings. The CMeta VALUE implementation itself remains
part of standalone Orbit::Core and has no required ServiceHost dependency.
The compiler-host tool is the independently qualified SaltsUtils #731
artifact; its own historical build SDK is never linked to the runtime.


## M0k.2b generated Client/Dispatcher on Salts4 (strict Draft gate)

The only canonical `salts-idlc` from SaltsUtils Draft #733 accepts
exactly one `[orbit(10,100),native_context(1),orbit_value(64,64)]`
method per Service, with text/bytes-only owning NativeSourceIR records.
It generates a stable caller-owned pair of CMeta VALUE metadata binders,
the existing `orbit_call_port_init_value`, a typed async Client,
and a post-commit Dispatcher using begin/stage/end/observe. Response
`Record_clear` always runs, including after cancellation or stale reply.
The existing scalar `Game.Room` remains completely unchanged.

In the **existing** `orbit.idl.generated_unary` executable, a second
schema `OwnedGame.Room.Echo` is generated by the same sole
`orbit_idl_codegen` build owner. The native C11 test constructs two
independent actual CFlow Actors, immutable Method catalogs and
Core CallPorts, binds two distinct native receivers, and uses the
generated Client and Dispatcher with raw embedded NUL/non-ASCII bytes.
Immediately after successful admission, it destroys the original
caller-owned request and mutates the borrowed scratch data; each Room
must still receive the original bytes and return its own independent
response. The same generated typed Client must return FULL for a second
call before the CFlow owner progresses, with zero token and no callback.
The same emitted header is also compiled under C++17.

This tests native owning-value codegen and actual CFlow dispatch; it
does **not yet** prove a genuine managed Provider N/N+1 DSO under
concurrent cancellation and Scope retirement. Those gates stay open
until tested in the original ServiceHost ABI-2 DSO test graph. No
new test executable, extra CI workflow, second runtime, hidden
scheduler, old ComponentProvider binding or fallback.


## M0k.2c genuine managed Provider N→N+1 DSO acceptance

The existing orbit.ace.dso_native test now executes an additional,
separate, genuine ServiceHost runtime instance after retiring the original
scalar M0i/M0j fixture. This preserves those scalar lifetime-race proofs:
no extra managed Scope can make their generation-drain/BUSY assertions
trivially true. It does not introduce a second Plugin implementation,
loader, Actor/scheduler backend, CTest executable or CI workflow.

Both real old-N and new-N Plugin DSOs are loaded and two independent
OwnedGame.Room.Echo generated CFlow Rooms are bound via separate original
ServiceHost Scope leases to the immutable old/new Provider objects.
The sole canonical salts-idlc emits the owning typed Client, receiver-bound
Dispatcher and CMeta VALUE Port binding. OwnedPacket owns string label and
raw bytes, while only a uint64 generation token enters the CFlow Mailbox.

- Real Provider N method returns 37, N+1 returns 83. Both generated methods
  read original Room-A and raw 00 ff 42 80 after caller-owned request
  destruction and mutation of original scratch bytes. The generated results
  remain independent and owned, and exactly one SUCCESS callback is emitted.
  An extra request on the capacity-one old Port is FULL with zero token and
  no callback.
- The next old-N managed request is accepted and pauses after entering the
  canonical generated Native business symbol but *before* calling the old
  DSO vtable. A separate native thread cancels and sees one CANCELLED
  terminal while executing=1 and occupied=1. Original Scope release,
  generation drain and real Plugin unload must all remain BUSY.
- After CANCELLED callback return, native business must still safely read
  Port-owned label and bytes, invoke the real old N DSO vtable, release its
  execution borrow and never publish a second SUCCESS reply.
- Only after the old-N Actor, owned CallPort and Scope retire can generation
  N drain and its actual Plugin DSO unload. N+1 remains live throughout:
  with N code unloaded, it must execute a new generated owning VALUE call
  through its separately retained N+1 DSO Scope. It then drains and unloads.

The preexisting scalar N→N+1 fixture still runs independently and first.
This test qualifies only the narrow local managed CFlow/DSO owner and terminal
boundary; it is not a claim of arbitrary nested managed graphs, every
thread interleaving, TSan, Streaming, remote exactly-once, or persistent
business COMMIT/ACK. Exact-head Linux/Windows native CTests remain the
authority. Keep the new PR Draft until actual CI has run.
