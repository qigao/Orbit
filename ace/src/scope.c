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
    bool activating; /* protects original Scope during Provider callbacks */
    bool native_active;
    void *native_receiver;
    unsigned native_seal; /* address identity within this stable Scope owner */
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

/*
 * Pre-publication only: a mismatched, closed or previously used Port cannot
 * be back-filled with a Provider lease after the first accepted call.
 * The application serializes this snapshot with all producer publication.
 */
static bool orbit_ace_scope_port_unused(
    const orbit_service *service, const orbit_call_port *port) {
    orbit_call_port_stats stats = {0};
    return service != NULL && service->actor.impl != NULL &&
        port != NULL && orbit_call_port_is_bound_to(port, service) &&
        orbit_call_port_get_stats(port, &stats) &&
        !stats.closed && stats.admitted == 0u &&
        stats.occupied == 0u && stats.admitting == 0u &&
        stats.callbacks_inflight == 0u && stats.executing == 0u;
}

orbit_ace_scope_status orbit_ace_scope_guard_bind(
    orbit_ace_scope_guard *guard,
    const orbit_service *service,
    const orbit_call_port *call_port) {
    orbit_ace_scope_impl *impl = orbit_ace_live(guard);
    if (impl == NULL || !impl->valid_generation ||
        !impl->scope.live || service == NULL ||
        service->actor.impl == NULL ||
        (call_port != NULL &&
         !orbit_call_port_is_bound_to(call_port, service)))
        return ORBIT_ACE_SCOPE_INVALID_ARGUMENT;
    if (impl->activating) return ORBIT_ACE_SCOPE_BUSY;
    if (impl->bound)
        return ORBIT_ACE_SCOPE_ALREADY_BOUND;
    /* Legacy Actor-only binding may omit a Port; the native receiver
     * activation below requires an exact never-admitted Port.
     */
    if (call_port != NULL &&
        !orbit_ace_scope_port_unused(service, call_port))
        return ORBIT_ACE_SCOPE_INVALID_ARGUMENT;
    impl->service = service;
    impl->call_port = call_port;
    impl->bound = true;
    return ORBIT_ACE_SCOPE_OK;
}

/*
 * The original Scope owns the Provider lease. The only external hooks here
 * are the canonical CMeta provider projection and the trusted application
 * receiver initializer. No hook executes while a CallPort mutex is held.
 * Protect the Scope from reentrant release/bind during these hooks.
 */
orbit_ace_scope_status orbit_ace_scope_guard_activate_native(
    orbit_ace_scope_guard *guard,
    const orbit_service *service,
    const orbit_call_port *call_port,
    const cmeta_interface_desc *interface_desc,
    orbit_ace_native_receiver_init_fn init_receiver,
    void *user,
    orbit_ace_native_activation *out) {
    orbit_ace_scope_impl *impl = orbit_ace_live(guard);
    salts_configurator_service provider = {0};
    cmeta_interface_projection projection = CMETA_INTERFACE_PROJECTION_INIT;
    orbit_ace_scope_status result = ORBIT_ACE_SCOPE_INVALID_ARGUMENT;
    void *receiver = NULL;
    if (out != NULL) out->source = NULL;
    if (impl == NULL || out == NULL || !impl->valid_generation ||
        !impl->scope.live || service == NULL || call_port == NULL ||
        !cmeta_interface_desc_valid(interface_desc) ||
        init_receiver == NULL)
        return ORBIT_ACE_SCOPE_INVALID_ARGUMENT;
    if (impl->activating)
        return ORBIT_ACE_SCOPE_BUSY;
    if (impl->bound)
        return ORBIT_ACE_SCOPE_ALREADY_BOUND;
    if (!orbit_ace_scope_port_unused(service, call_port))
        return ORBIT_ACE_SCOPE_INVALID_ARGUMENT;

    /* No one may retire this original address-stable Scope while external
     * Provider/application callbacks execute on its borrowed code.
     */
    impl->activating = true;
    if (salts_service_host_scope_find_service(
            &impl->scope, interface_desc, &provider) !=
        SALTS_SERVICE_HOST_OK) {
        result = ORBIT_ACE_SCOPE_HOST_ERROR;
        goto done;
    }
    if (cmeta_object_interface_project_borrowed(
            provider.object, provider.interfaces, interface_desc,
            &projection) != CMETA_OK) {
        result = ORBIT_ACE_SCOPE_PROJECTION_REJECTED;
        goto done;
    }
    if (!init_receiver(user, &projection, &receiver) ||
        receiver == NULL) {
        result = ORBIT_ACE_SCOPE_RECEIVER_REJECTED;
        goto done;
    }
    /* The trusted initializer may reenter public CallPort APIs. Recheck
     * all owner identities/capacity after it returns, before publication.
     */
    if (!impl->valid_generation || !impl->scope.live ||
        impl->generation_id !=
            salts_service_host_scope_generation_id(&impl->scope) ||
        impl->bound || !orbit_ace_scope_port_unused(service, call_port))
        goto done;

    impl->service = service;
    impl->call_port = call_port;
    impl->native_receiver = receiver;
    impl->native_active = true;
    impl->bound = true;
    out->source = &impl->native_seal;
    result = ORBIT_ACE_SCOPE_OK;
done:
    impl->activating = false;
    return result;
}

orbit_ace_scope_status orbit_ace_native_activation_receiver(
    const orbit_ace_scope_guard *guard,
    const orbit_ace_native_activation *activation,
    const orbit_service *service,
    const orbit_call_port *call_port,
    void **out_receiver) {
    orbit_ace_scope_impl *impl = orbit_ace_live(guard);
    if (out_receiver != NULL) *out_receiver = NULL;
    if (impl == NULL || out_receiver == NULL || activation == NULL ||
        activation->source == NULL || !impl->scope.live ||
        !impl->valid_generation || !impl->bound || !impl->native_active ||
        impl->activating || impl->native_receiver == NULL ||
        activation->source != &impl->native_seal ||
        service == NULL || service != impl->service ||
        call_port == NULL || call_port != impl->call_port ||
        !orbit_call_port_is_bound_to(call_port, service))
        return ORBIT_ACE_SCOPE_INVALID_ARGUMENT;
    *out_receiver = impl->native_receiver;
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
    if (impl->activating)
        return ORBIT_ACE_SCOPE_BUSY;
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
