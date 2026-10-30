#include <orbit/service.h>

#include <cmeta/cmeta.h>
#include <cmeta/type_traits.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * One immutable catalog allocation per spawned Service. CFlow owns the
 * Actor's independently retained control ref; this private catalog owner
 * merely keeps the prebound method rows alive for late producer references.
 * It neither owns an Executor/Mailbox nor retains any Plugin/Type provider.
 */
typedef struct orbit_method_catalog {
    _Atomic size_t refs;
    size_t count;
    orbit_service_method methods[];
} orbit_method_catalog;

static orbit_method_catalog *orbit_catalog_create(
    const orbit_service_method *methods, size_t count) {
    orbit_method_catalog *catalog;
    size_t bytes;
    if (methods == NULL || count == 0u ||
        count > (SIZE_MAX - sizeof(*catalog)) / sizeof(*methods))
        return NULL;
    bytes = sizeof(*catalog) + count * sizeof(*methods);
    catalog = (orbit_method_catalog *)malloc(bytes);
    if (catalog == NULL) return NULL;
    atomic_init(&catalog->refs, 1u);
    catalog->count = count;
    memcpy(catalog->methods, methods, count * sizeof(*methods));
    return catalog;
}

static bool orbit_catalog_retain(orbit_method_catalog *catalog) {
    size_t count;
    if (catalog == NULL) return false;
    count = atomic_load_explicit(&catalog->refs, memory_order_relaxed);
    while (count != 0u && count != SIZE_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &catalog->refs, &count, count + 1u,
                memory_order_acquire, memory_order_relaxed))
            return true;
    }
    return false;
}

static void orbit_catalog_release(orbit_method_catalog *catalog) {
    if (catalog != NULL &&
        atomic_fetch_sub_explicit(&catalog->refs, 1u,
                                  memory_order_acq_rel) == 1u)
        free(catalog);
}

static bool orbit_methods_valid(const orbit_service_config *config) {
    size_t i;
    size_t j;
    const cflow_machine *machine;
    const size_t n = config->method_count;

    if (config->actor == NULL || config->methods == NULL || n == 0u)
        return false;
    machine = config->actor->machine.machine;
    if (machine == NULL || machine->impl == NULL ||
        n > cflow_machine_event_count(machine))
        return false;

    for (i = 0u; i < n; ++i) {
        const orbit_service_method *method = &config->methods[i];
        bool found = false;
        if (method->id == 0u || method->event_id == 0u ||
            method->request_type == NULL ||
            !cmeta_type_desc_valid(method->request_type) ||
            cmeta_type_require_traits(
                method->request_type,
                CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY) != CMETA_OK ||
            (method->flags & ~ORBIT_METHOD_FLAGS_MASK) != 0u ||
            (method->flags == ORBIT_METHOD_CALL_TOKEN &&
             !cmeta_type_equal(method->request_type, &cmeta_type_uint64)) ||
            (i != 0u && config->methods[i-1u].id >= method->id))
            return false;

        /*
         * One service method must select exactly one distinct Machine Event.
         * Aliasing two method IDs to one Event silently loses method identity
         * when CFlow dispatches; reject it during activation.
         */
        for (j = 0u; j < i; ++j)
            if (config->methods[j].event_id == method->event_id)
                return false;

        for (j = 0u; j < cflow_machine_event_count(machine); ++j) {
            const cflow_event_type *declared = cflow_machine_event_at(machine, j);
            if (declared != NULL && declared->id == method->event_id &&
                cmeta_type_equal(declared->payload_type, method->request_type)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

orbit_service_spawn_result orbit_service_spawn(
    orbit_service *service, const orbit_service_config *config) {
    orbit_service_spawn_result result = {
        ORBIT_SERVICE_INVALID_ARGUMENT,
        CFLOW_ACTOR_INVALID_ARGUMENT,
        CFLOW_MACHINE_INSTANCE_OK
    };
    cflow_actor_init_result actor_init;
    cflow_actor_status start_status;
    orbit_method_catalog *catalog;

    if (service == NULL || config == NULL || service->actor.impl != NULL ||
        service->methods != NULL || service->method_count != 0u ||
        service->catalog != NULL || config->actor == NULL)
        return result;
    if (!orbit_methods_valid(config)) {
        result.status = ORBIT_SERVICE_INVALID_METHOD;
        return result;
    }

    /* Snapshot the validated immutable method rows BEFORE Actor publication.
     * Source may reside in a generated header or transient application array.
     */
    catalog = orbit_catalog_create(config->methods, config->method_count);
    if (catalog == NULL) {
        result.status = ORBIT_SERVICE_ALLOCATION_FAILED;
        return result;
    }

    actor_init = cflow_actor_init(&service->actor, config->actor);
    result.actor_status = actor_init.status;
    result.machine_status = actor_init.machine_status;
    if (actor_init.status != CFLOW_ACTOR_OK) {
        orbit_catalog_release(catalog);
        result.status = ORBIT_SERVICE_ACTOR_INIT_REJECTED;
        return result;
    }

    start_status = cflow_actor_start(&service->actor);
    result.actor_status = start_status;
    if (start_status != CFLOW_ACTOR_OK) {
        /* CFlow alone owns the failed Actor's terminal/cleanup semantics. */
        cflow_actor_destroy(&service->actor);
        orbit_catalog_release(catalog);
        result.status = ORBIT_SERVICE_ACTOR_START_REJECTED;
        return result;
    }

    service->catalog = catalog;
    service->methods = catalog->methods;
    service->method_count = catalog->count;
    result.status = ORBIT_SERVICE_OK;
    return result;
}

bool orbit_service_ref_acquire(const orbit_service *service,
                               orbit_service_ref *out) {
    orbit_method_catalog *catalog;
    if (service == NULL || out == NULL || service->actor.impl == NULL ||
        service->catalog == NULL || service->methods == NULL ||
        service->method_count == 0u ||
        out->actor.impl != NULL || out->methods != NULL ||
        out->method_count != 0u || out->catalog != NULL)
        return false;
    catalog = (orbit_method_catalog *)service->catalog;
    if (!orbit_catalog_retain(catalog)) return false;
    if (!cflow_actor_ref_acquire(&service->actor, &out->actor)) {
        orbit_catalog_release(catalog);
        return false;
    }
    out->catalog = catalog;
    out->methods = catalog->methods;
    out->method_count = catalog->count;
    return true;
}

bool orbit_service_ref_retain(const orbit_service_ref *ref,
                              orbit_service_ref *out) {
    orbit_method_catalog *catalog;
    if (ref == NULL || out == NULL || ref->actor.impl == NULL ||
        ref->catalog == NULL || ref->methods == NULL ||
        ref->method_count == 0u ||
        out->actor.impl != NULL || out->methods != NULL ||
        out->method_count != 0u || out->catalog != NULL)
        return false;
    catalog = (orbit_method_catalog *)ref->catalog;
    if (!orbit_catalog_retain(catalog)) return false;
    if (!cflow_actor_ref_retain(&ref->actor, &out->actor)) {
        orbit_catalog_release(catalog);
        return false;
    }
    out->catalog = catalog;
    out->methods = catalog->methods;
    out->method_count = catalog->count;
    return true;
}

void orbit_service_ref_release(orbit_service_ref *ref) {
    orbit_method_catalog *catalog;
    if (ref == NULL) return;
    catalog = (orbit_method_catalog *)ref->catalog;
    cflow_actor_ref_release(&ref->actor);
    ref->catalog = NULL;
    ref->methods = NULL;
    ref->method_count = 0u;
    orbit_catalog_release(catalog);
}

cflow_actor_send_status orbit_service_ref_try_send(
    const orbit_service_ref *ref, orbit_method_id method,
    const void *request) {
    size_t first;
    size_t last;
    if (ref == NULL || ref->actor.impl == NULL || ref->catalog == NULL ||
        ref->methods == NULL || ref->method_count == 0u ||
        method == 0u || request == NULL)
        return CFLOW_ACTOR_SEND_INVALID_ARGUMENT;

    first = 0u;
    last = ref->method_count;
    while (first < last) {
        const size_t middle = first + (last - first) / 2u;
        const orbit_service_method *entry = &ref->methods[middle];
        if (entry->id == method) {
            /* Reserved call-token events cannot enter via generic send. */
            if ((entry->flags & ORBIT_METHOD_CALL_TOKEN) != 0u)
                return CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
            const cflow_event_view event = {
                entry->event_id,
                entry->request_type,
                request
            };
            return cflow_actor_ref_try_send(&ref->actor, &event);
        }
        if (entry->id < method)
            first = middle + 1u;
        else
            last = middle;
    }
    return CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
}

cflow_actor_status orbit_service_request_stop(orbit_service *service) {
    return service != NULL
        ? cflow_actor_request_stop(&service->actor)
        : CFLOW_ACTOR_INVALID_ARGUMENT;
}

cflow_actor_state orbit_service_wait(orbit_service *service) {
    return service != NULL
        ? cflow_actor_wait(&service->actor)
        : CFLOW_ACTOR_STATE_FAILED;
}

bool orbit_service_get_stats(const orbit_service *service,
                             cflow_actor_stats *out) {
    return service != NULL &&
        cflow_actor_get_stats(&service->actor, out);
}

void orbit_service_destroy(orbit_service *service) {
    orbit_method_catalog *catalog;
    if (service == NULL) return;
    cflow_actor_destroy(&service->actor);
    catalog = (orbit_method_catalog *)service->catalog;
    service->catalog = NULL;
    service->methods = NULL;
    service->method_count = 0u;
    orbit_catalog_release(catalog);
}
