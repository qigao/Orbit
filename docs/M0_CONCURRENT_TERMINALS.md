# M0h — concurrent CallPort credit and terminal arbitration

**Draft, not merged/released.** Stacked on Orbit M0 legacy-provider retirement
PR #19. The installed Salts3 / SaltsCore0.1 owner graph and the exact
SaltsUtils #730 host compiler pins do not change.

## One actor, many real producers

This qualification extends **existing** \`tests/test_orbit_call.c\` inside
\`orbit.core.call_native\`. It uses the existing \`Salts::Platform\`
\`cmeta_thread_create/join\` API and C11 atomics. It does not create another
Actor, Mailbox, scheduler, terminal owner, test executable, external project,
or CI workflow. There is one actual CFlow Actor/Executor/Scheduler per
fixture and one bounded Orbit CallPort.

### Exact admitted-credit competition

Eight independent native threads start at a common atomic gate, each using
its own retained producer ref for the **same** CallPort. No CFlow executor
progress is allowed until all submissions return. With mailbox capacity
two and CallPort response credits two:

- exactly two calls must be accepted with distinct nonzero generation tokens;
- the remaining six must return \`ORBIT_CALL_FULL\`, zero token, and **no**
  callback, proving that rejection leaves the caller's data untransferred;
- before settling, \`admitted=2\`, \`occupied=2\`, and \`admitting=0\`.

Two cancel threads and one close thread then race at another atomic gate.
The cancels may return OK or STALE depending on which terminal claimed the
slot first, but each accepted token must receive **exactly one** callback,
with result CANCELLED or STOPPED and no response. The callback re-enters
\`orbit_call_u64_cancel\` on its own token and must see STALE while its
credit is retained in COMPLETING state. After all threads join, verify
\`occupied=0\`, \`callbacks_inflight=0\`, and
\`admitted=cancelled+stopped=2\`. The two accepted CFlow events are then
driven and must be stale: **zero business invocations** and no duplicate
terminal. Destroying the port while the Actor lives stays BUSY.

### Close-versus-ADMITTING competition

Eight producers and one simultaneous close thread start at one gate for
eight bounded repetitions. No order is forced. A producer may return
accepted, FULL or CLOSED according to the winning lock order, but all
accepted calls must terminate exactly once with STOPPED, and all
rejected calls receive **no** callbacks or tokens. The port has no
leftover submissions, terminal callbacks or occupied credits at quiescence.

### Ownership boundaries

Producer refs are retained and released by the startup/join owner, never
copied/mutated by concurrent workers. Callback storage remains address
stable until every submitting/cancel/closing thread has joined. The CFlow
owner alone advances the Machine/Subscriber; no worker executes business
logic. This tests the **real** concurrent CallPort arbitration, not an
alternate state machine. No hardcoded winner or required scheduling
interleaving is assumed for the close race.

**Not yet qualified by this test:** concurrent Plugin Scope release and
actual DSO unload while callback/actor credit is still outstanding,
simultaneous CFlow owner progress during business execution,
thread sanitizer proof, managed-value ownership, remote delivery or
durable commit/ACK. The existing generated N/N+1 DSO test retains its
separate exact-head evidence. The old ComponentProvider Candidate and
handwritten DSO adapter stay deleted.

## Required CI

The existing Ubuntu 24.04 Debug/Release ACE=ON CTests (7/7 each),
Ubuntu Core-only (4/4), Windows x64 Release ACE=ON (7/7), Windows
Core-only (4/4) must **actually run the extended
\`orbit.core.call_native\`** at the new PR HEAD. Failed or unrun checks
cannot be represented as proof. No merge/release while Draft.
