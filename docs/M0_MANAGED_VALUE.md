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


## M0k.2d borrowed managed response callback versus close and FULL

The managed N/N+1 ServiceHost lifecycle witness in M0k.2c proves a
running business method cannot lose its execution borrow when local
cancellation finishes. A separate safety boundary applies to a successful
managed response while its terminal callback has started but not returned.

The existing orbit.core.call_native holds an actual managed SUCCESS
terminal callback open on its CFlow post-commit owner thread. A second
real Salts native thread confirms:

- callbacks_inflight=1, executing=0, occupied=1, completed_success=1.
- While that borrowed owning response pointer is held, another attempt
  on the capacity-one Port returns FULL with zero token and no callback.
- The worker closes the original CallPort with STOPPED; close returns OK
  and a further attempt returns CLOSED with zero token/no callback.
- The response credit is still occupied and the callback remains active
  even after close. The original callback reads the same owning binary
  response again only after the worker has completed all the checks.
- After callback return and only then, provider restore_zero may recycle
  the response; total terminal accounting is exactly one SUCCESS and zero
  additional STOPPED, old token STALE and no residual credits.

This adds no production API, Actor, scheduler, runtime, Plugin loader,
CTest executable, CI workflow or installer/consumer project. It uses the
same test-local canonical CMeta BufferOps-v2 provider and Salts Platform
thread API. It does not establish exhaustive scheduling, sanitizer/TSan,
remote RPC, Streaming or business COMMIT/ACK.


## M0k.2e response deep-copy failure and subsequent reuse

The existing Core native test now forces the canonical CMeta BufferOps-v2
provider to fail specifically when the *owned business response* is being
deep-copied into the original CallPort's reply slot, after business-side
response construction succeeded. The application destroys its local
response separately; the Port must restore any partially staged reply to
semantic zero without inventing a successful terminal.

The test expects exactly one NO_REPLY terminal with a NULL borrowed
response, one failed accepted call, no leaked occupied/executing/callback
credits, and a stale original token. Without changing the Actor/Port owner,
the test retries a new managed request on the same capacity-one slot,
receives a genuine SUCCESS and verifies independent new-generation token,
correct raw owning binary reply and one callback per accepted request.

The earlier pre-admission request-copy fault remains a *different* gate:
zero token, no admission and no callback. Here the request was already
accepted, so NO_REPLY is the fail-closed local terminal rather than silent
credit leakage. This is a controlled fault-injection of the canonical
provider copy callback in the existing native CTest, not a separate
allocator/runtime, parser, CMake probe, CI workflow or consumer app.


## M0k.2f Provider reentry and lock-free VALUE lifecycle

Review of the original CallPort found two external CMeta lifecycle
callbacks previously invoked under the original Port mutex: staging an
accepted owning response via cmeta_data_value_copy and retiring a slot
via cmeta_data_value_restore_zero. Those callbacks can reenter public
Port APIs and cannot safely be treated as lock-free inert memory copies.

The same bounded CallPort now reserves STAGING while the existing
post-commit business execution borrow is active. The actual CMeta
response size/read and deep-copy execute without the mutex. Reentrant
get_stats, cancel and close are supported; an attempted reentrant
business end or observe during STAGING returns BUSY. Cancel/close may
claim the one local terminal, but cannot recycle the slot before the
original stage operation finishes and its execution pin retires.

All owned request and response restores run outside the mutex only
after the slot transitions to RECLAIMING. The credit stays occupied and
the token is STALE throughout this phase. A concurrent/reentrant
admission remains FULL until both original Provider restores finish.
This applies independently to rejected pre-admission copies, close
during submission, CFlow send rejection, terminal callback exit and
cancellation after native business execution. Provider teardown may
reenter try_destroy; the original Port marks its final destruction
boundary and rejects recursive destruction as BUSY.

The existing native test uses the original fault-injectable CMeta
BufferOps-v2 provider, and enables its external copy hook only inside
Port-stage (not business-local response construction). That hook
reenters stats, attempts end and observe (both BUSY), and cancels the
same generation (one CANCELLED, no late SUCCESS). Reuse only succeeds
after the original business borrow and provider copy complete. A
separate restore hook runs while RECLAIMING and reenters stats,
try_destroy and another bounded admission (FULL); cleanup then
finishes and a fresh generation succeeds. Neither test installs an
alternate runtime, custom provider ABI or separate CTest project.

A CMeta restore failure remains a fail-stop ownership violation:
both independent restores are attempted, but unsafe partially freed
storage is never published as reusable. This is not a general
recoverable allocator failure, nested/nullable VALUE admission,
exhaustive scheduler/TSan proof, Streaming or RPC/COMMIT-ACK gate.
