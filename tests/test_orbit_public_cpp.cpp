#include <orbit/service.h>
#include <orbit/call.h>
#if defined(ORBIT_TEST_HAS_ACE)
#include <orbit/scope.h>
#include <salts/service_host_abi.h>
#endif

#include <cstdint>
#include <type_traits>

/* The public C11 handles must remain ordinary stable-layout C ABI records. */
static_assert(std::is_standard_layout<orbit_service>::value, "Service ABI");
static_assert(std::is_standard_layout<orbit_service_ref>::value, "Ref ABI");
static_assert(std::is_standard_layout<orbit_service_method>::value, "Method ABI");
static_assert(std::is_trivially_copyable<orbit_service_method>::value,
              "Method descriptors remain a copied immutable catalog");
static_assert(std::is_standard_layout<orbit_call_port>::value, "Call ABI");

#if defined(ORBIT_TEST_HAS_ACE)
static_assert(std::is_standard_layout<orbit_ace_scope_guard>::value,
              "ACE guard C++17 ABI");
static_assert(SALTS_SERVICE_HOST_ABI_MAJOR == 2u,
              "Orbit ACE requires native ServiceHost ABI 2");
#endif

int main() {
    orbit_service empty{};
    orbit_service_ref ref{};
    orbit_call_port calls{};
    std::uint64_t token = 1u;

    if (orbit_service_spawn(&empty, nullptr).status !=
        ORBIT_SERVICE_INVALID_ARGUMENT) return 1;
    if (orbit_service_ref_try_send(&ref, 1u, &token) !=
        CFLOW_ACTOR_SEND_INVALID_ARGUMENT) return 2;
    if (orbit_call_port_try_destroy(&calls) !=
        ORBIT_CALL_INVALID_ARGUMENT) return 3;

    orbit_service_ref_release(&ref);
    if (ref.actor.impl != nullptr || ref.catalog != nullptr) return 4;

#if defined(ORBIT_TEST_HAS_ACE)
    orbit_ace_scope_guard guard{};
    salts_service_host_status upstream = SALTS_SERVICE_HOST_OK;
    if (orbit_ace_scope_guard_generation_id(&guard) != 0u) return 5;
    if (orbit_ace_scope_guard_try_release(&guard, &upstream) !=
        ORBIT_ACE_SCOPE_INVALID_ARGUMENT) return 6;
    if (upstream != SALTS_SERVICE_HOST_INVALID_ARGUMENT) return 7;
#endif
    return 0;
}
