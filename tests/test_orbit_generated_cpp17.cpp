#define DATABIND_NATIVE_ENABLE_ORBIT 1
#include "game.service_native.h"
#include "owned_room.service_native.h"
#include "idl/room_state.h"

#include <cstdint>
#include <type_traits>

static_assert(std::is_standard_layout<OwnedPacket>::value,
              "Managed NativeSourceIR request must have one public C++17 layout");
static_assert(std::is_standard_layout<
                  databind_9_OwnedGame_4_Room_4_Echo__orbit_context>::value,
              "Managed generated Client context must retain the C++17 C ABI");
static_assert(std::is_standard_layout<
                  databind_9_OwnedGame_4_Room_4_Echo__orbit_value_owner>::value,
              "Managed CMeta binding owner must have one stable C++17 layout");

static_assert(std::is_standard_layout<PingRequest>::value,
              "Canonical generated request must retain C layout");
static_assert(std::is_trivially_copyable<PingResponse>::value,
              "Scalar generated response must be trivially copied");
static_assert(std::is_standard_layout<databind_4_Game_4_Room_4_Ping__orbit_context>::value,
              "Generated callback context is plain C ABI storage");

/*
 * The exact same salts-idlc generated header is compiled as C11 and C++17
 * in ONE ordinary Orbit native test. The real native business function is
 * implemented in the C11 object; this is a cross-language linker check.
 */
extern "C" int orbit_generated_cpp17_smoke(void) {
    PingRequest request{UINT64_C(14)};
    PingResponse response{};
    orbit_room_state room{};
    if (databind_4_Game_4_Room_4_Ping(
            &room, &request, &response) != 0 ||
        response.ack != UINT64_C(15) || room.counter != UINT64_C(15))
        return 1;

    databind_4_Game_4_Room_4_Ping__orbit_context context{};
    orbit_call_port empty{};
    orbit_service_ref ref{};
    std::uint64_t token = 99u;
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_ACCEPTED;
    const auto result = databind_4_Game_4_Room_4_Ping__orbit_call_async(
        &empty, &ref, nullptr, &context, nullptr, nullptr, &token, &send);
    if (result != ORBIT_CALL_INVALID_ARGUMENT)
        return 2;
    return 0;
}
