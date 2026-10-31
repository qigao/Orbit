> Historical source/evidence record. The current native SDK requirement and
> Provider ABI migration are documented in [M0 ABI 2](M0_ABI2_MIGRATION.md).

# M0c — canonical Salts ServiceHost Scope integration

**Status: stacked Draft source, NOT compiled or CI-qualified.**

## Objective

An Orbit Service / CFlow Actor can borrow a provider's native CMeta
FunctionDesc/FunctionAbi, callbacks and component methods from the
**original** Salts ServiceHost/Plugin generation without prematurely
unloading old code during N -> N+1 publication, callbacks or cancellation.

Orbit must not duplicate Configurator, Plugin leases, generation counters or
ServiceHost Scope. A `salts_service_host_scope` is original, noncopyable and
nonmovable: its address is checked by Salts. A scope cannot be stored in a
resizable vector, moved after acquisition, or copied into a call slot.

## Component boundaries

```text
        Orbit::Core                       Orbit::ACE (optional)
    native CFlow Actor                   canonical Scope guard
    bounded Call Port                         |
    no ServiceHost link                       v
                               Salts::ACE (Configurator / ServiceHost)
                                     Plugin Registry / generations
```

`Orbit::Core` remains a standalone static library linked against
`Salts::CFlow` and `Salts::Platform`. The separate `Orbit::ACE`
static target is built only with the explicit `-DORBIT_BUILD_ACE=ON`
selection and links `Orbit::Core` and `Salts::ACE`. With the
default `OFF` only `Orbit::Core` is built, without requiring a
ACE target. An explicitly selected missing `Salts::ACE`
is a hard CMake error, not a probe or fallback.

The real Salts 3.0 CMake package remains the one ABI authority. No dynamic
loader, application thread, scheduler, message queue or fallbacks are created.

## Native API

```c
orbit_ace_scope_guard guard = {0};
salts_service_host_status exact = SALTS_SERVICE_HOST_OK;

/* First: acquire BEFORE provider-owned code/metadata is borrowed. */
orbit_ace_scope_guard_open(&guard, &host_runtime,
                           expected_generation, &exact);

/* Discover a service only through that original canonical Scope. */
salts_configurator_service provider = {0};
orbit_ace_scope_guard_find_service(
    &guard, &expected_interface, &provider);

/* Application builds native Machine/Actor from provider-owned bindings
   while the guard remains live, then initializes optional Call Port. */
orbit_ace_scope_guard_bind(&guard, &actor_service, &call_port);

/* Withdraw Actor admission; complete/settle Call Port callbacks, destroy
   Actor, then destroy Call Port while keeping that same guard alive. */
orbit_service_destroy(&actor_service);
orbit_call_port_try_destroy(&call_port);

/* After all declared owner-side quiescence only: */
orbit_ace_scope_guard_try_release(&guard, &exact);
```

This code is **illustrative startup/shutdown ordering**, not an example that
may call a blocking `orbit_service_destroy()` from a callback. Real
application code must use its explicit CFlow owner and stop/wait/drain steps.

### Scope admission facts

- `guard_open` calls `salts_service_host_scope_acquire` into a heap-owned,
  **address-stable original Scope**. The object is not moved thereafter.
- The expected generation is mandatory. If N+1 was published between the
  application's planned provider binding and `guard_open`, the Scope is
  released and the call fails `GENERATION_MISMATCH` rather than accepting
  the wrong provider.
- A rare failed rollback release preserves the **live but quarantined** Scope
  for explicit retry through `guard_try_release`. That scope cannot be used
  for provider lookup/bind, and remains tracked rather than being freed.
- `guard_find_service` delegates to
  `salts_service_host_scope_find_service`. Its output borrows the original
  Component/Provider code and is valid only while the Scope is live.
- `guard_bind` pins a single initialized, address-stable `orbit_service`
  and optional `orbit_call_port`, which must be bound to exactly that actor.
  Binding is a control-plane operation, before Service publication.
- `guard_try_release` returns BUSY while the bound Actor has not been
  destroyed or the bound Call Port is still live. It never stops the Actor,
  drains a plugin, calls `Plugin::unload`, or waits for a terminal.
- `salts_service_host_scope_release` is the sole release authority. Failure
  leaves the original Scope live for recovery. On success the wrapper frees
  its stable storage.
- A pinned generation can remain draining after N+1 publication. Only Salts
  ServiceHost has authority to decide when generation/Plugin lease drain
  succeeds and a DSO becomes unloadable.

### Important limitations

- The guard is a **generation lifetime helper**, not a proof that an arbitrary
  callback was produced from that provider. The application must obtain the
  actual native bindings through `guard_find_service` before Actor admission.
  The future `salts-idlc` generated adapter should make this relationship
  type-checked and nonoptional for plugin-owned methods.
- This first slice pins one generation **for the entire Service lifetime**,
  not independently per call. That is conservative; it can delay old-generation
  drain until explicit old Service retirement. Never silently repin existing
  calls or streams to a newer generation.
- A Scope is used by one thread at a time. Open, find, bind and release belong
  to the owner's serialized control plane; application callback method-level
  concurrency still belongs to the provider.
- Live Service/Call Port owner storage may NOT be moved or freed before the
  guard is released. Their destroy operations must not race guard release.
- This does **not** implement a general ServiceHost factory, runtime
  load/unload, arbitrary managed DataBind buffer lifetime, unary H2 RPC,
  Streaming or Directory.

## Native conformance

`tests/test_orbit_scope.c` uses the **real Salts ServiceHost**
public calls: it first checks fail-closed admission on an unpublished runtime,
then builds/publishes two actual **empty static Configurator generations**
N=11 and N+1=12. This is a legitimate lifecycle-only case (NOT a fake
Provider/Plugin). It checks that a mismatched generation does not leak a
scope, an old live Scope survives publication of N+1, and the original
`salts_service_host_generation_drain` reports BUSY until the exact original
Scope is released. No test mutates private counters to fake success.

The remaining **provider-level** acceptance needs a real static or DSO
Provider generation, a bound CFlow Actor and Call Port, N->N+1 publication,
release only after native Actor terminal, and actual Plugin unload
quiescence. It must use the existing Salts/Orbit native test graphs—not a
new installer/consumer project—and run against an immutable same-epoch
Salts 3 SDK. Source presence is NOT a passing CTest result.

## Build/release gates

As of the checked GitHub state, Salts `master` declares version 3.0.0,
but the currently visible latest Salts Release is `v2.3.0-rc.17`.
SaltsCore `master` declares 0.1.0, with no visible Release. This is a
**real SDK qualification blocker**. Neither older Salts 2.x packages nor
copied source headers may be used as fallback.

All Orbit PRs remain Draft. No claim of compile pass, CTest pass, package
publication, merge or full ServiceHost/Plugin acceptance is authorized merely
by this document or an accepted source commit.

## Existing-source C++17 public contract gate

The same in-repository native CTest graph also compiles and links
`tests/test_orbit_public_cpp.cpp` against the actual installed Salts 3
CMeta/CFlow SDK and Orbit's C ABI (with/without the optional ACE adapter).
This proves the exported C11 headers admit C++17 consumers without an
additional installer/consumer project. It is **not** an assertion of generic
C++ Service code generation or arbitrary managed-type ABI.
