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


## M0k.2g Provider close reentry during admission and stage

M0k.2f moved all owning CMeta Provider copy/restore hooks outside the
original CallPort mutex while preserving its STAGING and RECLAIMING
credits. Two additional reentrant close orders remain independently
important and are now tested inside the existing native Core CTest.

First, a Provider's *request copy* hook calls Port close while the
request is ADMITTING and no CFlow Event has been accepted. The Port
must keep one pending slot credit until the copy returns, then reclaim
that value without transferring the caller's data, emitting a callback
or inventing a STOPPED terminal. The original async call returns CLOSED
and token zero with admitted=0 and occupied=0 at quiescence.

Second, a Provider's *response copy* hook calls Port close during
STAGING after the real post-commit Native business method acquired its
execution borrow. The original STOPPED terminal is delivered exactly
once, while executing=1 and occupied=1. The Provider returns to the
original stage function, which sees STALE, and the Actor completes
business without a late SUCCESS. The final original credit is released
only after the execution pin retires; the prior token is STALE.

Both checks use the exact original CallPort, Salts Platform thread and
test-local CMeta BufferOps-v2 provider. No duplicate Actor, scheduler,
parser, loader, CI workflow, CTest executable or consumer project is
introduced. These controlled close hooks do not establish exhaustive
concurrent scheduling, ThreadSanitizer correctness or recoverable
Provider restore failures. Remote RPC/Streaming and business COMMIT/ACK
remain separate product gates.

## M0k.2h activation-only Scope/CallPort admission (pending exact CI)

The Salts ServiceHost Scope remains the **only** Provider lease. The ACE
`orbit_ace_scope_guard_bind` control-plane operation now checks the live
original CallPort identity and requires a clean **never-admitted** Port,
not merely one whose old tokens have already been settled. A closed Port,
a foreign Port from a second CFlow Actor, or a Port that has ever admitted
a call fails with `ORBIT_ACE_SCOPE_INVALID_ARGUMENT`, before the Scope
records the Actor/Port owners. A successful activation still uses the
existing single `orbit_ace_scope_guard_bind` and canonical generated
`native_context(1)` receiver.

The existing `orbit.ace.provider_native` CTest intentionally exercises
wrong Actor/Port pairings across two independently published original
ServiceHost generations N and N+1, a never-used but closed CallPort, and
an already-admitted CallPort (both before and after the terminal callback).
Its test-only deliberately late-bound Actor is kept under a manually live
Scope through complete owner teardown; it never proves it is safe to
release a Scope before binding. The preexisting positive N->N+1 and
real generated managed DSO suites remain mandatory.

**Remaining hard boundary:** an application-owned `void *receiver` is an
opaque business object. Comparing Port and Actor identity does **not**
prove that receiver's embedded CMeta Provider projection originated in
the same Scope. Callers must acquire the typed Provider through the
original `guard_find_service` and bind during serialized activation,
before any producer can submit. A future automatic Provider-provenance
claim would need an explicitly Scope-minted activation capability;
do not pretend raw pointers make that claim. Concurrent binding vs
admission is invalid owner behavior rather than an implicitly locked
runtime contract.

The CMake dependency is the canonical `find_package(CMeta CONFIG REQUIRED)`
in both source and exported package; CI still pins immutable CMeta/Core
source revisions, Clang/llvm-mingw and the actual ServiceHost ABI v2 symbol
at compile/link time. Removing the redundant package-version constraint
does **not** permit another Provider ABI or SDK fallback.

This extension does not add a parser, generated function ABI, Actor,
ServiceHost replacement, second CTest executable, CI workflow, package
probe or installer/consumer application. **Draft until current exact-head
native Linux/Windows CI runs.**


## M0k.2i full-source LLVM ThreadSanitizer admission

The existing native Linux Clang CI job now rebuilds the exact immutable
SaltsCore 0.1 and CMeta 4.0 sources with the same ThreadSanitizer
instrumentation used by CMeta's original sanitizer workflow:
fsanitize=thread, frame pointers, debug symbols, and O1.
Orbit itself (C11 Core, C11 ACE, generated C11/C++17 tests and the
actual original N/N+1 Provider DSOs) is compiled and linked using the
same Clang TSan flags against these instrumented installed dependencies.

No new workflow, CI job, test executable, consumer project, simulator,
alternate Actor/Plugin runtime or CMake detection is added. This
source-instrumented admission uses the same native CI's preexisting
salts-idlc host provenance-pinned external CLI; its old standalone
host SDK is not injected into the CMeta/Orbit application.

The original native CTest graph runs under TSAN_OPTIONS with
halt_on_error=1 and exitcode=66, including native CallPort
multi-producer/admission/cancel/close, Provider copy/restore/close
reentrancy, generated owning VALUE Room and real Plugin N/N+1 DSO
Scope/busy/drain/unload. A failure must be reported as a real RED,
not suppressed, reduced to a single mock test or counted as success
from an earlier compiler/source epoch.

Safety policy for CMeta restore_zero failure is deliberately fail-stop:
after the slot has transitioned to RECLAIMING, both independent
request and response Provider restore hooks are attempted. If either
fails, the process aborts without transitioning that generation
to FREE. This is not a recoverable failed operation; retrying or
publishing a partially freed response would violate owning CMeta
VALUE semantics. Full recoverable cleanup would require an explicit
separate provider ABI and fault model, not a local error alias.

One successful TSan run is a sampled dynamic race check of the actual
instrumented source, not a formal proof of every scheduler interleaving.
Unsupported nested/nullable/collection VALUE, remote HTTP/2 Streaming
and durable business COMMIT/ACK remain separate admission gates.

## M0k.2j Scope-minted Native activation (Draft; exact-head CI required)

The M0k.2h original ServiceHost Scope/CallPort pre-admission guard is now
extended with **one opt-in activation-time join**. It is implemented in
`Orbit::ACE` and operates on the **existing** original Salts Scope and CMeta
`cmeta_object_interface_project_borrowed`. The canonical
`native_context(1)` C business method, original NativeSourceIR/FunctionAbi,
generated `__orbit_bind` / `__orbit_dispatch`, CFlow Actor and bounded
CallPort are unchanged.

```text
original ServiceHost Scope N -- find_service(expected Interface)
                            -- CMeta Provider projects ObjectRef
                            -- trusted application initializes receiver
                            -- recheck original live Actor + unused CallPort
                            -- bind original Scope and mint activation handle
                            -- resolve receiver ONCE for __orbit_bind
                            -- publish producers
```

The new `orbit_ace_scope_guard_activate_native()` accepts the exact live
Actor, its **never-admitted** CallPort, an exact CMeta interface descriptor
and a **trusted application receiver initializer**. The initializer receives
the borrowed `cmeta_interface_projection` emitted by the original Scope,
not an arbitrary application-supplied projection. It constructs the typed
Provider field within the address-stable application-owned business
receiver, and supplies that receiver pointer. CMeta validates the generic
projection and the initializer must validate the typed Interface surface.
The actor's generated method still receives that same `void *receiver`,
not a second reflection ABI. No CMeta lookup occurs during post-commit
business execution.

The original Scope is marked **activating** while calling the external
Provider/project/initializer functions. Reentrant Scope release or old raw
Scope binding returns BUSY. The CallPort is rechecked after callbacks; if a
callback closes it or admits a call, activation fails with **no published
ticket or Scope binding**. Projection rejection or initializer failure
also leaves the Scope unbound for the application owner to repair/retry,
with no implicit receiver cleanup (application state is caller-owned).

A successful activation yields one non-owning
`orbit_ace_native_activation` identity that is resolved **once** through
`orbit_ace_native_activation_receiver()` with the *same live original
Scope, Service and CallPort*. Empty/foreign/expired activations and
cross-Actor or cross-N/N+1 pairings reject and clear the requested output.
The handle is not a Plugin lease and is discarded before original Scope
release. Existing service/CallPort close, callbacks, CFlow drain, original
ServiceHost generation drain and real Plugin unload remain the same
authoritative lifetime path.

**Exact supported meaning:** this prevents accidental mismatch of the
*activation issued by a Scope* against another Scope/Actor/Port. The
initializer is explicitly trusted application code; no native system can
prove the contents of an arbitrary `void *receiver` supplied outside this
API. The handle is an owner-scoped, borrowed runtime identity, **not an
unforgeable security credential**; user code must not mutate/forge its
storage or reuse it after Scope retirement. There is no permission for
concurrent producer publication during activation.

The existing `orbit.ace.provider_native` verifies two independent genuine
static CMeta Providers, N/N+1 projections and cross-activation rejection.
It also injects initializer rejection, Scope release/bind reentry and a
callback that closes its own CallPort during activation. The original
`orbit.ace.dso_native` now uses this path to bind BOTH actual generated
scalar Game.Room and managed OwnedGame.Room receivers from original
Plugin DSOs under N/N+1 and tests cross-generation activation rejection.
The old original DSO unload, cancellation-after-entry and N+1 continuation
must all still actually pass.

Keep the single existing Clang/llvm-mingw native CI, together with source-
instrumented Linux TSan and current immutable CMeta/Core/salts-idlc host
provenance. No new test executable, workflow, CMake probe, installer,
consumer, Provider loader, runtime queue or fallback. **Do not claim
M0k.2j complete or merge until the new exact PR HEAD has passed.**

## M0k.2l — checked CMeta temporary teardown, fail-stop vs quarantine

**Upstream source owner:** [CMeta Draft #1340](https://github.com/qigao/cmeta/pull/1340)
HEAD `c711c378ca450d1261da619980281f4279033cf6`.
This is the exact installed CMeta SDK source pinned by the **existing**
Linux Clang / Windows llvm-mingw workflow, not a second CMeta implementation
linked next to an older package.

There are **three distinct failure boundaries**:

1. **Ordinary request/response copy failure** is recoverable *only if* the
   original CMeta owning-value rollback/restore succeeds. Pre-admission
   failure leaves no accepted token/callback; accepted reply copy failure
   returns one NO_REPLY and subsequent capacity may be reused. Existing
   M0k.2a/e/f/g fixtures verify these routes.
2. **A broken no-fail Provider restore during LIVE slot reclamation** is a
   fatal in-progress lifecycle invariant: the original `RECLAIMING` slot
   is never transitioned to `FREE`; `orbit_call_reclaim_slot` attempts
   both restore operations and deliberately aborts on non-OK. No inferred
   retry, second cleanup provider or forced slot recycle is permitted.
3. **A broken no-fail Provider during FINAL Port teardown** occurs only
   after the original Actor, callbacks, admission and execution borrows
   have all been quiesced. The Port now calls CMeta's
   `cmeta_data_temp_try_close` for each original owned temporary.
   When any cleanup is non-OK it returns
   `ORBIT_CALL_VALUE_CLEANUP_FAILED` and **permanently quarantines** the
   original Port/catalog/provider owner without freeing the failed
   temporary, releasing its retained CFlow producer identity or allowing
   its original ServiceHost Scope/Plugin to unload. A later
   `try_destroy` returns the same error with **no implicit retry**.
   Operator process restart/fail-stop remains the terminal policy; this
   is containment, not a promise that arbitrary hostile Provider code
   can be repaired in place.

The original `orbit.ace.scope_guard_try_release` stays BUSY while this
quarantined CallPort handle is live; this is intentional and preserves the
DSO code that owns the invalid native-value descriptor. Production callers
must NOT drop the failed Port's address-stable ServiceHost owner storage
or attempt native free/Plugin unload after a failed teardown.

The **same existing `orbit_call_native_test` executable** now has one
separate negative CTest invocation,
`orbit.core.cleanup_quarantine`, which injects a deliberately malformed
test-only CMeta BufferOps-v2 is_zero/restore postcondition *only during final
teardown*, after the real CFlow Actor is destroyed. It proves the returned
cleanup error, permanent rejection of repeated destroy and retention of
the original live Port/owner. Its isolated test process exits with the
quarantine deliberately intact, because freeing it would defeat the
safety contract. This adds **no new executable/project or CI workflow**.

The exact full cross-platform Clang/llvm-mingw and Linux full-source TSan
CI on this new HEAD must still pass. Existing generated scalar and managed
N/N+1 real DSO cancellation, old Provider unload and live N+1 continuation
remain a separate mandatory acceptance. No general nested/nullable VALUE,
remote H2/Streaming, durable COMMIT/ACK or exhaustive scheduler proof.


## M0k.2m real Plugin DSO lease under final VALUE quarantine

The original CMeta #1340 checked temp close and Orbit #42 quarantining
CallPort have an independent Core-only negative test. This additional
mode of the existing orbit.ace.dso_native executable carries the owner
lifetime witness through the genuine Salts ServiceHost Plugin DSO.

- Publish actual N=21 and N+1=22 generation snapshots, load and start
  their original N and N+1 DSOs, and open separate real Scope guards.
- Bind one original Game.Room CFlow Actor to old N with an owning
  capacity-one CallPort using a test-local CMeta BufferOps v2 descriptor.
  The descriptor's final no-fail semantic-zero postcondition is made
  intentionally invalid only after the Actor has stopped and been
  destroyed; its failure callback calls the real old N Plugin DSO vtable
  to prove old N code is still loaded at the final cleanup boundary.
- A final quiescent call_port_try_destroy returns
  ORBIT_CALL_VALUE_CLEANUP_FAILED; both retry and original Scope release
  remain refused. Real generation drain and Plugin registry unload are
  BUSY; the old Provider getter still executes from its original N DSO.
- The independent N+1 generated Room succeeds both before and after
  the old N final cleanup quarantine. N+1 can stop, destroy its healthy
  Port, release its genuine Scope, drain and unload its own Provider
  while old N is intentionally still pinned.

This is a **test-local malformed owning CMeta descriptor** whose failure
callback enters an actual Plugin DSO. The existing DSO's exported
Provider itself was not modified to supply a malformed data descriptor;
do not report this as proof of a DSO-authored malicious VALUE provider.
The tested property is real DSO/Scope retention after a CMeta cleanup
contract violation. Reproducing a malicious descriptor implemented
inside a DSO, as opposed to bound while that DSO is leased, is a
separate trust-boundary test if required.

The negative mode runs as a second command of the SAME existing DSO
test executable, in an isolated process because the invalid old-N
owner MUST NOT be force-destroyed or unloaded. No new CTest executable,
CI workflow, plugin loader, Provider ABI, schema, parser or consumer
project is added. The existing Clang source-level TSan graph executes
this real-life owner lifecycle mode as well.

Broken no-fail Provider cleanup is fail-closed/quarantined at already
quiescent final teardown, not recoverable. In-progress RECLAIMING
cleanup violations still abort. The test cannot prove every possible
concurrent schedule, runtime isolation from malicious native source,
remote RPC, Streaming or persistent business COMMIT/ACK.
