#ifndef ORBIT_ACE_SCOPE_H
#define ORBIT_ACE_SCOPE_H

#include <orbit/call.h>
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
    ORBIT_ACE_SCOPE_ALREADY_BOUND
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
 * generation. It does not prove their business callbacks came from this
 * provider: obtain the provider binding via the guard above before building.
 */
orbit_ace_scope_status orbit_ace_scope_guard_bind(
    orbit_ace_scope_guard *guard,
    const orbit_service *service,
    const orbit_call_port *call_port);

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
