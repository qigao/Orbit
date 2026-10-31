#include <orbit/scope.h>
#include <salts/service_host_abi.h>

#include <stdlib.h>

/* The live Salts scope MUST NOT move: its owner_address is the authority. */
typedef struct orbit_ace_scope_impl {
    salts_service_host_scope scope;
    const orbit_service *service;
    const orbit_call_port *call_port;
    uint64_t generation_id;
    bool valid_generation;
    bool bound;
} orbit_ace_scope_impl;

static orbit_ace_scope_impl *orbit_ace_live(
    const orbit_ace_scope_guard *guard) {
    return guard == NULL ? NULL : (orbit_ace_scope_impl *)guard->impl;
}

orbit_ace_scope_status orbit_ace_scope_guard_open(
    orbit_ace_scope_guard *guard,
    salts_service_host_runtime *runtime,
    uint64_t expected_generation_id,
    salts_service_host_status *out_host_status) {
    orbit_ace_scope_impl *impl;
    salts_service_host_status status;

    if (out_host_status != NULL)
        *out_host_status = SALTS_SERVICE_HOST_INVALID_ARGUMENT;
    if (guard == NULL || guard->impl != NULL || runtime == NULL ||
        expected_generation_id == 0u)
        return ORBIT_ACE_SCOPE_INVALID_ARGUMENT;

    /* Require the canonical native ABI at link time, including cross builds.
     * Scope admission must never silently link an earlier host archive. */
    salts_service_host_abi_v2_0_required();

    impl = (orbit_ace_scope_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return ORBIT_ACE_SCOPE_ALLOCATION_FAILED;
    status = salts_service_host_scope_acquire(runtime, &impl->scope);
    if (out_host_status != NULL) *out_host_status = status;
    if (status != SALTS_SERVICE_HOST_OK) {
        free(impl);
        return ORBIT_ACE_SCOPE_HOST_ERROR;
    }

    impl->generation_id =
        salts_service_host_scope_generation_id(&impl->scope);
    if (impl->generation_id != expected_generation_id) {
        /* Do not turn a publication race into the wrong generation pin. */
        status = salts_service_host_scope_release(&impl->scope);
        if (out_host_status != NULL) *out_host_status = status;
        if (status != SALTS_SERVICE_HOST_OK) {
            /* Preserve the genuine noncopyable scope for explicit retry. */
            guard->impl = impl;
            return ORBIT_ACE_SCOPE_HOST_ERROR;
        }
        free(impl);
        return ORBIT_ACE_SCOPE_GENERATION_MISMATCH;
    }

    impl->valid_generation = true;
    guard->impl = impl;
    return ORBIT_ACE_SCOPE_OK;
}

salts_service_host_status orbit_ace_scope_guard_find_service(
    const orbit_ace_scope_guard *guard,
    const cmeta_interface_desc *interface_desc,
    salts_configurator_service *out_service) {
    orbit_ace_scope_impl *impl = orbit_ace_live(guard);
    if (impl == NULL || !impl->valid_generation ||
        interface_desc == NULL || out_service == NULL ||
        !impl->scope.live)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;
    return salts_service_host_scope_find_service(
        &impl->scope, interface_desc, out_service);
}

orbit_ace_scope_status orbit_ace_scope_guard_bind(
    orbit_ace_scope_guard *guard,
    const orbit_service *service,
    const orbit_call_port *call_port) {
    orbit_ace_scope_impl *impl = orbit_ace_live(guard);
    orbit_call_port_stats stats = {0};
    if (impl == NULL || !impl->valid_generation ||
        !impl->scope.live || service == NULL ||
        service->actor.impl == NULL ||
        (call_port != NULL &&
         !orbit_call_port_is_bound_to(call_port, service)))
        return ORBIT_ACE_SCOPE_INVALID_ARGUMENT;
    if (impl->bound)
        return ORBIT_ACE_SCOPE_ALREADY_BOUND;
    /*
     * A borrowed Provider Scope must be attached BEFORE the first call is
     * published. Attaching it after admission (even after completion) or
     * to a closed Port would leave an unpinned lifetime window. The startup
     * owner MUST serialize this snapshot with producer publication.
     */
    if (call_port != NULL &&
        (!orbit_call_port_get_stats(call_port, &stats) ||
         stats.closed || stats.admitted != 0u ||
         stats.occupied != 0u || stats.admitting != 0u ||
         stats.callbacks_inflight != 0u || stats.executing != 0u))
        return ORBIT_ACE_SCOPE_INVALID_ARGUMENT;
    impl->service = service;
    impl->call_port = call_port;
    impl->bound = true;
    return ORBIT_ACE_SCOPE_OK;
}

uint64_t orbit_ace_scope_guard_generation_id(
    const orbit_ace_scope_guard *guard) {
    orbit_ace_scope_impl *impl = orbit_ace_live(guard);
    return impl == NULL || !impl->valid_generation ||
        !impl->scope.live ? 0u : impl->generation_id;
}

orbit_ace_scope_status orbit_ace_scope_guard_try_release(
    orbit_ace_scope_guard *guard,
    salts_service_host_status *out_host_status) {
    orbit_ace_scope_impl *impl = orbit_ace_live(guard);
    salts_service_host_status result;
    if (out_host_status != NULL)
        *out_host_status = SALTS_SERVICE_HOST_INVALID_ARGUMENT;
    if (impl == NULL || !impl->scope.live)
        return ORBIT_ACE_SCOPE_INVALID_ARGUMENT;
    if (impl->bound &&
        ((impl->service != NULL && impl->service->actor.impl != NULL) ||
         (impl->call_port != NULL && impl->call_port->impl != NULL)))
        return ORBIT_ACE_SCOPE_BUSY;

    result = salts_service_host_scope_release(&impl->scope);
    if (out_host_status != NULL) *out_host_status = result;
    if (result != SALTS_SERVICE_HOST_OK)
        return ORBIT_ACE_SCOPE_HOST_ERROR;
    guard->impl = NULL;
    free(impl);
    return ORBIT_ACE_SCOPE_OK;
}
