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
 * Salts 3 SDK and its existing ServiceHost fixture in an integration build;
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

int main(void) {
    check_invalid_admission();
    check_unpublished_runtime_fails_closed();
    return 0;
}
