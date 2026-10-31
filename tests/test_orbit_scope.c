#include <orbit/scope.h>

#include <salts/service_host.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        abort(); \
    } \
} while (0)

/*
 * These are actual Salts ServiceHost runtime calls, never test doubles.
 * A complete positive generation/Provider DSO acceptance needs the published
 * Salts 4 SDK and its existing ServiceHost fixture in an integration build;
 * it is an independent open gate, not something this test fabricates.
 */
static void check_invalid_admission(void) {
    orbit_ace_scope_guard guard = {0};
    salts_service_host_status native = SALTS_SERVICE_HOST_OK;

    CHECK(orbit_ace_scope_guard_generation_id(&guard) == 0u);
    CHECK(orbit_ace_scope_guard_try_release(
        &guard, &native) == ORBIT_ACE_SCOPE_INVALID_ARGUMENT);
    CHECK(native == SALTS_SERVICE_HOST_INVALID_ARGUMENT);
    CHECK(orbit_ace_scope_guard_open(
        NULL, NULL, 1u, &native) == ORBIT_ACE_SCOPE_INVALID_ARGUMENT);
    CHECK(orbit_ace_scope_guard_open(
        &guard, NULL, 1u, &native) == ORBIT_ACE_SCOPE_INVALID_ARGUMENT);
    CHECK(orbit_ace_scope_guard_open(
        &guard, NULL, 0u, &native) == ORBIT_ACE_SCOPE_INVALID_ARGUMENT);
    CHECK(guard.impl == NULL);
    CHECK(orbit_ace_scope_guard_find_service(
        &guard, NULL, NULL) == SALTS_SERVICE_HOST_INVALID_ARGUMENT);
}

static void check_unpublished_runtime_fails_closed(void) {
    salts_service_host_runtime host = SALTS_SERVICE_HOST_RUNTIME_INIT;
    orbit_ace_scope_guard guard = {0};
    salts_service_host_status native = SALTS_SERVICE_HOST_OK;
    orbit_ace_scope_status status;

    CHECK(salts_service_host_runtime_init(&host) ==
          SALTS_SERVICE_HOST_OK);

    /* The native runtime has no committed/published generation. Orbit
     * cannot manufacture one or silently accept a wrong epoch.
     */
    status = orbit_ace_scope_guard_open(&guard, &host, 1u, &native);
    CHECK(status == ORBIT_ACE_SCOPE_HOST_ERROR);
    CHECK(native != SALTS_SERVICE_HOST_OK);
    CHECK(guard.impl == NULL);
    CHECK(orbit_ace_scope_guard_generation_id(&guard) == 0u);
    CHECK(host.active_scopes == 0u);

    CHECK(salts_service_host_runtime_destroy(&host) ==
          SALTS_SERVICE_HOST_OK);
}

/*
 * Exercise Salts' real built/published generation count, no private Scope
 * implementation and no fake mutable `generation.active_scopes` writes.
 *
 * The generation deliberately has an empty STATIC Configurator graph: this
 * tests the publication/Scope ownership control plane only. It is not a
 * substitute for provider/Plugin DSO runtime conformance.
 */
static void check_generation_revision_and_drain(void) {
    salts_service_host_generation first = SALTS_SERVICE_HOST_GENERATION_INIT;
    salts_service_host_generation second = SALTS_SERVICE_HOST_GENERATION_INIT;
    salts_service_host_generation_storage storage = {0};
    salts_service_host_generation *previous = NULL;
    salts_service_host_runtime host = SALTS_SERVICE_HOST_RUNTIME_INIT;
    orbit_ace_scope_guard guard = {0};
    salts_service_host_status native = SALTS_SERVICE_HOST_OK;

    CHECK(salts_service_host_generation_build(
        &first, UINT64_C(11), NULL, &storage, NULL, 0u,
        NULL, 0u, NULL, 0u) == SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_generation_build(
        &second, UINT64_C(12), NULL, &storage, NULL, 0u,
        NULL, 0u, NULL, 0u) == SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_init(&host) ==
          SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_publish(
        &host, &first, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == NULL);

    /* Mandatory exact generation pin: mismatch must not leak one Scope. */
    CHECK(orbit_ace_scope_guard_open(
        &guard, &host, UINT64_C(12), &native) ==
            ORBIT_ACE_SCOPE_GENERATION_MISMATCH);
    CHECK(native == SALTS_SERVICE_HOST_OK);
    CHECK(guard.impl == NULL);
    CHECK(host.active_scopes == 0u);

    CHECK(orbit_ace_scope_guard_open(
        &guard, &host, UINT64_C(11), &native) ==
            ORBIT_ACE_SCOPE_OK);
    CHECK(orbit_ace_scope_guard_generation_id(&guard) == UINT64_C(11));
    CHECK(host.active_scopes == 1u);
    CHECK(orbit_ace_scope_guard_open(
        &guard, &host, UINT64_C(11), &native) ==
            ORBIT_ACE_SCOPE_INVALID_ARGUMENT);

    CHECK(salts_service_host_runtime_publish(
        &host, &second, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == &first);
    CHECK(salts_service_host_generation_drain(
        &host, &first) == SALTS_SERVICE_HOST_BUSY);

    CHECK(salts_service_host_runtime_close(
        &host, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == &second);
    CHECK(salts_service_host_generation_drain(
        &host, &second) == SALTS_SERVICE_HOST_OK);

    CHECK(orbit_ace_scope_guard_try_release(
        &guard, &native) == ORBIT_ACE_SCOPE_OK);
    CHECK(native == SALTS_SERVICE_HOST_OK);
    CHECK(guard.impl == NULL);
    CHECK(host.active_scopes == 0u);

    CHECK(salts_service_host_generation_drain(
        &host, &first) == SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_destroy(
        &host) == SALTS_SERVICE_HOST_OK);
}

int main(void) {
    check_invalid_admission();
    check_unpublished_runtime_fails_closed();
    check_generation_revision_and_drain();
    return 0;
}
