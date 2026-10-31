#define DATABIND_NATIVE_ENABLE_ORBIT 1
#include "owned_room.service_native.h"

#include <cflow/executor.h>
#include <cflow/machine.h>
#include <cflow/scheduler.h>
#include <cmeta/cmeta.h>
#include <cmeta/data.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: failed: %s\n", \
        __FILE__, __LINE__, #expression); abort(); \
} } while (0)

/* This is the exact canonical NativeSourceIR business symbol, shared with
 * DataBindNativeExecution.context. No second Orbit handler ABI or parser.
 * The result is a separately owned generated record, never an alias.
 */
typedef struct owned_room_state {
    uint8_t marker;
    unsigned calls;
} owned_room_state;

int databind_9_OwnedGame_4_Room_4_Echo(
    void *receiver, const OwnedPacket *request, OwnedPacket *response) {
    owned_room_state *state = (owned_room_state *)receiver;
    static const unsigned char expected[] = {0x00u, 0xffu, 0x42u, 0x80u};
    if (!state || !request || !response ||
        !request->label.data || request->label.size != 6u ||
        memcmp(request->label.data, "Room-A", 6u) != 0 ||
        !request->payload.data ||
        request->payload.size != sizeof(expected) ||
        memcmp(request->payload.data, expected, sizeof(expected)) != 0)
        return 17;
    if (OwnedPacket_clone(response, request) != 0)
        return 18;
    response->payload.data[0] = state->marker;
    ++state->calls;
    return 0;
}

typedef struct owned_instance {
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    orbit_service service;
    orbit_service_ref producer;
    orbit_call_port port;
    cflow_machine_action_binding action[1];
    databind_9_OwnedGame_4_Room_4_Echo__orbit_value_owner value_owner;
    databind_9_OwnedGame_4_Room_4_Echo__orbit_context ctx;
    databind_9_OwnedGame_4_Room__orbit_binding dispatch;
    owned_room_state state;
    int machine_state;
    unsigned observed;
    unsigned callbacks;
    unsigned application_errors;
    unsigned errors;
    uint64_t last_token;
    uint8_t response_marker;
} owned_instance;

static bool owned_action(
    void *user, const void *state, const void *event,
    void *next_state, void *observation, const char **out_error) {
    (void)user;
    if (!state || !event || !next_state || !observation || !out_error)
        return false;
    *(int *)next_state = *(const int *)state + 1;
    *(uint64_t *)observation = *(const uint64_t *)event;
    *out_error = NULL;
    return true;
}

static bool owned_observe(void *user, const cmeta_type_desc *type,
                          const void *value) {
    owned_instance *room = (owned_instance *)user;
    orbit_call_status status;
    uint64_t token;
    CHECK(room && value && cmeta_type_equal(type, &cmeta_type_uint64));
    token = *(const uint64_t *)value;
    status = databind_9_OwnedGame_4_Room__orbit_dispatch(
        &room->dispatch, token);
    CHECK(status == ORBIT_CALL_OK || status == ORBIT_CALL_STALE);
    ++room->observed;
    return true;
}

static void owned_on_error(void *user, const char *message) {
    owned_instance *room = (owned_instance *)user;
    (void)message;
    ++room->errors;
    CHECK(orbit_call_port_close(
        &room->port, ORBIT_CALL_TERMINAL_ACTOR_FAILED) == ORBIT_CALL_OK);
}

static void owned_on_done(void *user) {
    owned_instance *room = (owned_instance *)user;
    CHECK(orbit_call_port_close(
        &room->port, ORBIT_CALL_TERMINAL_STOPPED) == ORBIT_CALL_OK);
}

static void owned_done(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int application_status, const OwnedPacket *response) {
    owned_instance *room = (owned_instance *)user;
    CHECK(room && token != 0u);
    CHECK(terminal == ORBIT_CALL_TERMINAL_SUCCESS);
    CHECK(application_status == 0 && response);
    CHECK(response->label.size == 6u &&
          response->label.data &&
          memcmp(response->label.data, "Room-A", 6u) == 0);
    CHECK(response->payload.size == 4u &&
          response->payload.data &&
          response->payload.data[0] == room->state.marker &&
          response->payload.data[1] == 0xffu &&
          response->payload.data[2] == 0x42u &&
          response->payload.data[3] == 0x80u);
    CHECK(orbit_call_u64_cancel(&room->port, token) == ORBIT_CALL_STALE);
    room->last_token = token;
    room->response_marker = response->payload.data[0];
    ++room->callbacks;
}

/* This fixture consumes the original actual CFlow Machine/Event/Actor/Port,
 * and the generated VALUE init/client/dispatch helpers. No second executor
 * or invented reflection table is used by Orbit itself.
 */
static void owned_start(owned_instance *room, uint8_t marker) {
    const cflow_machine_state states[] = {
        {1u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE}
    };
    const cflow_event_type events[] = {
        {100u, &cmeta_type_uint64}
    };
    const cflow_machine_action actions[] = {
        {300u, &cmeta_type_int, 100u, &cmeta_type_uint64,
         &cmeta_type_int, CMETA_EFFECT_STATEFUL,
         CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
         CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_uint64, 0u}
    };
    const cflow_machine_transition transitions[] = {
        {1u, 100u, 0u, 300u, 1u, 1u}
    };
    const cflow_machine_definition definition = {
        states, 1u, 1u, events, 1u, NULL, 0u,
        actions, 1u, transitions, 1u
    };
    cflow_actor_config actor = {0};
    orbit_service_config config;
    memset(room, 0, sizeof(*room));
    room->state.marker = marker;
    CHECK(cflow_machine_build(&room->machine, &definition) == CFLOW_MACHINE_OK);
    CHECK(cflow_executor_owner_init_with_capacity(
        &room->executor, 64u, NULL, NULL));
    CHECK(cflow_scheduler_owner_bind(&room->scheduler, &room->executor, 32u));
    room->action[0] = (cflow_machine_action_binding){
        300u, owned_action, room
    };
    actor.machine = (cflow_machine_instance_config){
        &room->machine, &room->machine_state, &cmeta_type_uint64,
        NULL, 0u, room->action, 1u, 1u, &room->executor
    };
    actor.scheduler = &room->scheduler;
    actor.callbacks = (cflow_subscriber_callbacks){
        owned_observe, owned_on_error, owned_on_done, room
    };
    config = databind_9_OwnedGame_4_Room__orbit_service_config(&actor);
    CHECK(config.method_count == 1u);
    CHECK(config.methods[0].id == UINT64_C(10));
    CHECK(config.methods[0].event_id == UINT32_C(100));
    CHECK(orbit_service_spawn(&room->service, &config).status ==
          ORBIT_SERVICE_OK);
    CHECK(databind_9_OwnedGame_4_Room_4_Echo__orbit_port_init_value(
        &room->port, &room->service, 1u, &room->value_owner) ==
          ORBIT_CALL_OK);
    CHECK(room->value_owner.request.reflection.mode ==
          CMETA_DATA_REFLECTION_VALUE);
    CHECK(room->value_owner.response.reflection.mode ==
          CMETA_DATA_REFLECTION_VALUE);
    CHECK(databind_9_OwnedGame_4_Room__orbit_bind(
        &room->dispatch, &room->port, &room->state));
    CHECK(orbit_service_ref_acquire(&room->service, &room->producer));
}

static void owned_stop(owned_instance *room) {
    unsigned rounds = 0u;
    orbit_call_port_stats stats = {0};
    const cflow_actor_status stopped =
        orbit_service_request_stop(&room->service);
    CHECK(stopped == CFLOW_ACTOR_OK || stopped == CFLOW_ACTOR_STOPPED);
    while (cflow_actor_current_state(&room->service.actor) !=
           CFLOW_ACTOR_STATE_STOPPED && rounds++ < 2048u)
        (void)cflow_executor_run_one(&room->executor);
    CHECK(orbit_service_wait(&room->service) == CFLOW_ACTOR_STATE_STOPPED);
    CHECK(orbit_call_port_get_stats(&room->port, &stats));
    CHECK(stats.occupied == 0u && stats.executing == 0u);
    orbit_service_ref_release(&room->producer);
    orbit_service_destroy(&room->service);
    CHECK(orbit_call_port_try_destroy(&room->port) == ORBIT_CALL_OK);
    cflow_scheduler_destroy(&room->scheduler);
    cflow_executor_destroy(&room->executor);
    cflow_machine_destroy(&room->machine);
}

int orbit_generated_owned_room_test(void) {
    owned_instance old_room;
    owned_instance next_room;
    char original_label[] = "Room-A";
    unsigned char original_bytes[] = {0x00u, 0xffu, 0x42u, 0x80u};
    OwnedPacket scratch = {
        {original_label, sizeof(original_label) - 1u},
        {original_bytes, sizeof(original_bytes)}
    };
    OwnedPacket caller_request = {0};
    uint64_t old_token = 0u, next_token = 0u, refused = UINT64_MAX;
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    unsigned rounds = 0u;

    owned_start(&old_room, 37u);
    owned_start(&next_room, 83u);
    CHECK(old_room.dispatch.receiver != next_room.dispatch.receiver);
    CHECK(OwnedPacket_clone(&caller_request, &scratch) == 0);
    CHECK(databind_9_OwnedGame_4_Room_4_Echo__orbit_call_async(
        &old_room.port, &old_room.producer, &caller_request,
        &old_room.ctx, owned_done, &old_room, &old_token, &send) ==
          ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED && old_token != 0u);
    CHECK(databind_9_OwnedGame_4_Room_4_Echo__orbit_call_async(
        &next_room.port, &next_room.producer, &caller_request,
        &next_room.ctx, owned_done, &next_room, &next_token, &send) ==
          ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED && next_token != 0u);
    CHECK(databind_9_OwnedGame_4_Room_4_Echo__orbit_call_async(
        &old_room.port, &old_room.producer, &caller_request,
        &old_room.ctx, owned_done, &old_room, &refused, &send) ==
          ORBIT_CALL_FULL);
    CHECK(refused == 0u && old_room.callbacks == 0u);
    /* Each Port owns independent clones. The original can be cleared and
     * even the original stack-scratch bytes mutated before owner progress.
     */
    OwnedPacket_clear(&caller_request);
    original_label[0] = 'X';
    original_bytes[1] = 0x11u;
    while ((old_room.callbacks < 1u || next_room.callbacks < 1u) &&
           rounds++ < 2048u) {
        (void)cflow_executor_run_one(&old_room.executor);
        (void)cflow_executor_run_one(&next_room.executor);
    }
    CHECK(old_room.callbacks == 1u && next_room.callbacks == 1u);
    CHECK(old_room.observed == 1u && next_room.observed == 1u);
    CHECK(old_room.errors == 0u && next_room.errors == 0u);
    CHECK(old_room.state.calls == 1u && next_room.state.calls == 1u);
    CHECK(old_room.response_marker == 37u &&
          next_room.response_marker == 83u);
    CHECK(orbit_call_u64_cancel(&old_room.port, old_token) ==
          ORBIT_CALL_STALE);
    CHECK(orbit_call_u64_cancel(&next_room.port, next_token) ==
          ORBIT_CALL_STALE);
    owned_stop(&old_room);
    owned_stop(&next_room);
    puts("Orbit generated owned VALUE Room C11 two-owner PASS");
    return 0;
}
