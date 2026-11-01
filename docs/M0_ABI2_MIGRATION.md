## CMeta package identity follow-up

This branch builds the exact CMeta source `c711c378ca450d1261da619980281f4279033cf6` from
[Salts #1337](https://github.com/qigao/salts/pull/1337), stacked on #1336.
The GitHub repository remains qigao/salts until its separate manual rename.
Consumers require `find_package(CMeta CONFIG REQUIRED)` and link CMeta::
targets, with no former Salts package aliases. Core-owned Salts:: targets and
native/Provider/binding ABI contracts remain unchanged.

Both existing native jobs select the same compiler for installed Core, CMeta
and Orbit: runner-native Clang on Linux, llvm-mingw/UCRT 20261020 on Windows.
The shared compiler actions use vcpkg-cache@master after #244 merged.
Core source is pinned to `287c30db125e1ada12d51bd17ba36cddd8302305` from Core #135,
which qualifies explicit Platform DLL exports, real scheduler TLS and
llvm-mingw TinyTest initialization in the existing native graph.
No SDK publication or compiler/CRT cache relabeling is authorized by these
source tests. The separate old salts-idlc executable remains a host process.

Previous tables and CI evidence below describe earlier source revisions;
they do not qualify this package/toolchain change.

# M0 — ACE / Provider targets and native ABI 2

This stacked Draft continues Orbit #19 and consumes the breaking native API in
[Salts #1332](https://github.com/qigao/salts/pull/1332), followed by the ACE/Provider
CMake consolidation in [Salts #1336](https://github.com/qigao/salts/pull/1336). It is an application
migration and executable downstream qualification, not an SDK release.

## Runtime boundary

Both the Orbit build and its installed package require `Salts 4.0`. Static
Provider construction uses `salts_configurator_provider_binding`,
`salts_configurator_dependency` and binding ABI version 2. Dynamic Providers
include `<salts/service_host_provider_abi.h>` and link only
`CMeta::Provider`, which carries the CMeta/Plugin contracts without linking
the ACE runtime. Orbit::ACE links `CMeta::ACE`. The reflected
`salts_service_host_provider` Interface publishes the current version-2
contract and borrowed binding. No compatibility type, old header or fallback
contract is supplied by Orbit.

The ACE adapter calls `salts_service_host_abi_v2_0_required()` on Scope
admission. An earlier host archive cannot silently satisfy this link. Core-only
builds retain their original CFlow-only dependency graph.

## Lifecycle admission and configuration borrows

The pinned ACE implementation rejects lifecycle callback reentry while its
Configurator is STARTING/STOPPING. Construction config_data and config_value
expire after create() returns; Provider metadata and service views retain their
separate generation/Scope lifetime. Publication validates the exclusively owned
BUILT candidate outside runtime admission locking, then rechecks state/epoch
before attachment. Drain reserves one STOPPING bundle before validation and
teardown. These fixes retain native ABI 2.0 and the existing typed C functions.

Construction failure phase, component/dependency indices and Provider status
also survive complete generation cleanup and explicit recovery as scalar
Configurator diagnostics. Storage and Plugin leases still settle normally;
these error values do not retain a Provider image and reset on an admitted
new build. No native ABI/layout or caller lifecycle API changes are required.

## Existing executable path

The same native CTests exercise:

- static dependency construction and ServiceHost generation ownership;
- the genuine IDL-generated unary Client/Dispatcher and two independent Room
  receivers using canonical Native Context v1;
- two real Provider DSOs, N/N+1 publication, the original Scope holding N,
  accepted Actor/CallPort work, cancellation and terminal settlement, BUSY
  drain/unload, final Scope release and actual Plugin unload;
- public C11 and C++17 declarations with the native ABI 2 link boundary.

All construction, generation, Plugin and scheduler authority stays in the
original Salts owners. The generated Dispatcher binds one receiver once and
does not discover services or reconstruct an ABI for every message. The
discarded hand-written DSO adapter remains absent.

## Immutable qualification inputs

| Input | Identity |
| --- | --- |
| Salts runtime | `7d57b78ba8dffacd935f87d926b97a6eb8627377` — Salts 4.0 / ABI 2.0, ACE / Provider targets |
| Core runtime | `cfc1ed0d24364689b10ef7a85cff28d8776eb7db` — Core 0.1 |
| Canonical host compiler | SaltsUtils `852b5cc741ef0debb1735b03e129bb30a136f038` |
| Host artifact run | `41168655686`, Linux/Windows native SDKs |

The host compiler runs as a separate process. Its historical Salts 2.3 host
dependency does not enter the Orbit/Provider includes or linked runtime.
Native Context v1 is independent of the Configurator/ServiceHost ABI epoch;
its single parser, generator and generated business signatures are unchanged.

One `orbit_idl_codegen` build target owns the existing compiler command and all
three generated outputs. Both native test executables depend on that owner;
they do not each attach the custom rule to their own project. The first Windows
run exposed concurrent invocations racing the compiler's output backup. The
single-owner dependency fixes the build graph without disabling parallel
compilation or changing the compiler's publication behavior.

Local Linux tests against the actual installed runtime SDK pass Debug ACE+IDL
7/7, Release ACE+IDL 7/7, and Release Core-only+IDL 4/4. The real generated DSO
and two-Room CTests execute in these graphs. The existing native workflow must
provide independent exact-HEAD Linux/Windows evidence; local results are not a
claim of Windows or six-RID release qualification.

Remote RPC, Streaming, managed-value request/reply, cross-process delivery,
durable Directory and broader cross-thread race acceptance remain separate.
No merge, release tag, new consumer project or new CI workflow is introduced.
\n\nM0k.2l retains the exact CMeta package ABI but uses checked `cmeta_data_temp_try_close` from CMeta Draft #1340 to preserve a failed temporary and original Provider lease instead of incorrectly freeing incomplete cleanup.\n