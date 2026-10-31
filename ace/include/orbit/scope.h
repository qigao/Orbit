#ifndef ORBIT_ACE_SCOPE_H
#define ORBIT_ACE_SCOPE_H

#include <orbit/call.h>
#include <cmeta/object_interface.h>
#include <salts/service_host.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional Orbit::ACE integration. This API is a lifetime guard around
 * CMeta::ACE's ORIGINAL, noncopyable scope. It is NOT a second
 * Configurator, Plugin loader, generation controller or Scope implementation.
 *
 * A guard is one address-stable, noncopyable owner. It must be opened BEFORE
 * any provider-owned CFlow Machine/method table/callbacks are borrowed from
 * the selected ServiceHost generation.
 *
 * The guard holds that exact generation across existing accepted Actor work,
 * CallPort callbacks and stop/drain. A published N+1 never retargets it.
 *
 * The caller MUST keep the guard, host runtime, Service handle, CallPort, and
 * all borrowed provider pointers address-stable for the complete lifecycle.
 */
typedef struct orbit_ace_scope_guard {
    void *impl;
} orbit_ace_scope_guard;

typedef enum orbit_ace_scope_status {
    ORBIT_ACE_SCOPE_OK = 0,
    ORBIT_ACE_SCOPE_INVALID_ARGUMENT,
    ORBIT_ACE_SCOPE_ALLOCATION_FAILED,
    ORBIT_ACE_SCOPE_HOST_ERROR,
    ORBIT_ACE_SCOPE_GENERATION_MISMATCH,
    ORBIT_ACE_SCOPE_BUSY,
    ORBIT_ACE_SCOPE_ALREADY_BOUND,
    ORBIT_ACE_SCOPE_PROJECTION_REJECTED,
    ORBIT_ACE_SCOPE_RECEIVER_REJECTED
} orbit_ace_scope_status;

/*
 * Acquire the canonical Salts scope into heap-allocated, address-stable
 * backing storage. Expected generation must be explicit and nonzero.
 * If N+1 raced with a planned N provider activation, the scope is released
 * and GENERATION_MISMATCH is returned—never silently pin the wrong provider.
 *
 * out_host_status preserves the exact Salts status (may be NULL).
 * On a rare failed rollback release, return HOST_ERROR with a LIVE guard
 * requiring explicit try_release; do not free unsettled Salts-owned leases.
 */
orbit_ace_scope_status orbit_ace_scope_guard_open(
    orbit_ace_scope_guard *guard,
    salts_service_host_runtime *runtime,
    uint64_t expected_generation_id,
    salts_service_host_status *out_host_status);

/* Get a provider capability while the original Salts scope is live. All
 * borrowed descriptors/interface pointers expire after guard release.
 */
salts_service_host_status orbit_ace_scope_guard_find_service(
    const orbit_ace_scope_guard *guard,
    const cmeta_interface_desc *interface_desc,
    salts_configurator_service *out_service);

/*
 * Bind at most one CFlow Actor service and optional Orbit Call Port to the
 * acquired provider generation. Only the explicit service owner may call this
 * during startup, before publishing the Service to other producers.
 *
 * This records the handles that MUST be destroyed before releasing the
 * generation. A non-null CallPort must be live, belong to the exact Service,
 * and have admitted NO calls yet (including calls already completed); a
 * closed or previously used Port cannot be retroactively Scope-pinned.
 * The startup owner must serialize bind with producer publication: these
 * checks do not make concurrent bind/admission safe.
 *
 * The opaque receiver's Provider provenance cannot be deduced from void*.
 * Obtain its typed Provider projection through guard_find_service on this
 * exact guard before building the native receiver; NEVER combine a receiver
 * from a different ServiceHost Scope with this Service/Port.
 */
orbit_ace_scope_status orbit_ace_scope_guard_bind(
    orbit_ace_scope_guard *guard,
    const orbit_service *service,
    const orbit_call_port *call_port);

/*
 * M0k.2j — Scope-minted Native receiver activation.
 *
 * The original Salts Scope authorizes one CMeta object/interface projection
 * before the Service has accepted a call. This is an opt-in stronger binding
 * path for generated [native_context(1)] methods, not a second method ABI.
 *
 * The application's trusted initializer receives ONLY the projection
 * actually created by the original Scope's Provider. It must construct the
 * address-stable business receiver from this projection, then return that
 * exact receiver address. On failure the caller must discard partial
 * application state; Orbit does not own, copy or destroy the receiver.
 * A callback must never publish producers or run Actor work during init.
 *
 * This is a runtime integration/lifetime contract for cooperative native
 * code, not a security sandbox against a malicious application initializer.
 * A void* receiver by itself cannot attest its internal CMeta field contents.
 */
typedef bool (*orbit_ace_native_receiver_init_fn)(
    void *user, const cmeta_interface_projection *provider,
    void **out_receiver);

/*
 * A borrowed activation identity issued only after the original Scope:
 *   find_service -> original CMeta provider projection -> trusted init ->
 *   exact Service/unused CallPort admission -> bind
 * has completed. Zero initialize; do not move/retain past Scope release.
 * No independent Plugin lease, registry, lookup or callback executor.
 */
typedef struct orbit_ace_native_activation {
    const void *source;
} orbit_ace_native_activation;

orbit_ace_scope_status orbit_ace_scope_guard_activate_native(
    orbit_ace_scope_guard *guard,
    const orbit_service *service,
    const orbit_call_port *call_port,
    const cmeta_interface_desc *interface_desc,
    orbit_ace_native_receiver_init_fn init_receiver,
    void *user,
    orbit_ace_native_activation *out);

/*
 * Retrieve the exact previously minted receiver ONCE while initializing
 * the generated __orbit_binding or DataBindNativeExecution.context. The
 * calling Scope, Service and CallPort must ALL match the original mint.
 * A forged/empty/foreign/expired token fails closed with NULL receiver.
 * Do not call in the post-commit per-event data path.
 */
orbit_ace_scope_status orbit_ace_native_activation_receiver(
    const orbit_ace_scope_guard *guard,
    const orbit_ace_native_activation *activation,
    const orbit_service *service,
    const orbit_call_port *call_port,
    void **out_receiver);

/* Returns zero for an invalid/empty guard. */
uint64_t orbit_ace_scope_guard_generation_id(
    const orbit_ace_scope_guard *guard);

/*
 * No implicit stop or blocking wait. An already bound CFlow Actor must have
 * completed orbit_service_destroy(), and a bound call port must have completed
 * orbit_call_port_try_destroy() before releasing the Salts Scope.
 *
 * Release invokes salts_service_host_scope_release EXACTLY ONCE on success.
 * Salts failure leaves the guard and original Scope live for explicit repair
 * and retry. No Plugin stop/unload is performed by Orbit.
 * External callers must serialize with service/port destruction; no pointer
 * may be concurrently accessed or freed while try_release checks it.
 */
orbit_ace_scope_status orbit_ace_scope_guard_try_release(
    orbit_ace_scope_guard *guard,
    salts_service_host_status *out_host_status);

#ifdef __cplusplus
}
#endif

#endif /* ORBIT_ACE_SCOPE_H */
