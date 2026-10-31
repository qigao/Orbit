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
