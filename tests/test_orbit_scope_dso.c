#include <orbit/scope.h>
#include "fixtures/orbit_dso_contract.h"

#include <salts/service_host.h>
#include <salts/plugin.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ORBIT_DSO_PROVIDER_N_PATH) || \
    !defined(ORBIT_DSO_PROVIDER_NEXT_PATH)
#error actual DSO target paths must be provided by the original CMake graph
#endif

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

typedef struct dso_generation {
    salts_service_host_generation generation;
    salts_configurator_deployment deployments[1];
    salts_configurator_instance instances[1];
    salts_component_dependency dependencies[1];
    size_t activation_order[1];
    salts_service_host_module modules[1];
} dso_generation;

static void build(
    dso_generation *g, uint64_t generation_id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref plugin) {
    const salts_service_host_generation_storage storage = {
        g->deployments, 1u, g->instances, 1u,
        g->dependencies, 1u, g->activation_order, 1u,
        g->modules, 1u
    };
    const salts_service_host_source source = {
        plugin, ORBIT_DSO_PROVIDER_EXPORT_ID, NULL, NULL
    };
    CHECK(salts_service_host_generation_build(
        &g->generation, generation_id, registry, &storage,
        NULL, 0u, &source, 1u, NULL, 0u) ==
        SALTS_SERVICE_HOST_OK);
}

static int actual_provider_value(orbit_ace_scope_guard *scope) {
    salts_configurator_service service;
    orbit_dso_value value = orbit_dso_value_bind(NULL, NULL);
    CHECK(orbit_ace_scope_guard_find_service(
        scope, orbit_dso_value_interface(), &service) ==
        SALTS_SERVICE_HOST_OK);
    CHECK(orbit_dso_value_borrow_from_object(
        service.object, service.interfaces, &value) == CMETA_OK);
    return orbit_dso_value_get(&value);
}

int main(void) {
    const cmeta_plugin_registry_config registry_config = {2u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref original = {0};
    cmeta_plugin_ref successor = {0};
    cmeta_plugin_lifecycle_info info;
    dso_generation n = {0};
    dso_generation next = {0};
    salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
    salts_service_host_generation *previous = NULL;
    orbit_ace_scope_guard old_scope = {0};
    orbit_ace_scope_guard next_scope = {0};
    salts_service_host_status exact = SALTS_SERVICE_HOST_OK;
    bool quiet = false;

    CHECK(cmeta_plugin_registry_init(&registry, &registry_config) ==
          CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_load(
        &registry, ORBIT_DSO_PROVIDER_N_PATH, &original) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_load(
        &registry, ORBIT_DSO_PROVIDER_NEXT_PATH, &successor) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_start(
        &registry, original) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_start(
        &registry, successor) == CMETA_PLUGIN_OK);

    build(&n, UINT64_C(21), &registry, original);
    build(&next, UINT64_C(22), &registry, successor);
    CHECK(cmeta_plugin_registry_get_lifecycle(
        &registry, original, &info) == CMETA_PLUGIN_OK);
    CHECK(info.active_leases == 1u);

    CHECK(salts_service_host_runtime_init(&runtime) ==
          SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_publish(
        &runtime, &n.generation, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == NULL);
    CHECK(orbit_ace_scope_guard_open(
        &old_scope, &runtime, UINT64_C(21), &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(actual_provider_value(&old_scope) == 37);

    CHECK(salts_service_host_runtime_publish(
        &runtime, &next.generation, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == &n.generation);
    CHECK(orbit_ace_scope_guard_open(
        &next_scope, &runtime, UINT64_C(22), &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(actual_provider_value(&next_scope) == 83);
    /* Old provider still executes from its original loaded DSO. */
    CHECK(actual_provider_value(&old_scope) == 37);
    CHECK(salts_service_host_generation_drain(
        &runtime, &n.generation) == SALTS_SERVICE_HOST_BUSY);

    CHECK(cmeta_plugin_registry_request_stop(
        &registry, original) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_unload(
        &registry, original) == CMETA_PLUGIN_BUSY);

    CHECK(orbit_ace_scope_guard_try_release(
        &old_scope, &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(salts_service_host_generation_drain(
        &runtime, &n.generation) == SALTS_SERVICE_HOST_OK);
    CHECK(cmeta_plugin_registry_get_lifecycle(
        &registry, original, &info) == CMETA_PLUGIN_OK);
    CHECK(info.active_leases == 0u);
    CHECK(cmeta_plugin_registry_poll_quiescent(
        &registry, original, &quiet) == CMETA_PLUGIN_OK);
    CHECK(quiet);
    CHECK(cmeta_plugin_registry_unload(
        &registry, original) == CMETA_PLUGIN_OK);

    CHECK(salts_service_host_runtime_close(
        &runtime, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == &next.generation);
    CHECK(salts_service_host_generation_drain(
        &runtime, &next.generation) == SALTS_SERVICE_HOST_BUSY);
    CHECK(orbit_ace_scope_guard_try_release(
        &next_scope, &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(salts_service_host_generation_drain(
        &runtime, &next.generation) == SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_destroy(&runtime) ==
          SALTS_SERVICE_HOST_OK);
    CHECK(cmeta_plugin_registry_request_stop(
        &registry, successor) == CMETA_PLUGIN_OK);
    quiet = false;
    CHECK(cmeta_plugin_registry_poll_quiescent(
        &registry, successor, &quiet) == CMETA_PLUGIN_OK);
    CHECK(quiet);
    CHECK(cmeta_plugin_registry_unload(
        &registry, successor) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_destroy(&registry) == CMETA_PLUGIN_OK);

    puts("Orbit true Salts Plugin DSO N/N+1 and Scope unload PASS");
    return 0;
}
