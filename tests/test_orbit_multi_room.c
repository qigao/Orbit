#define DATABIND_NATIVE_ENABLE_ORBIT 1
#include "game.service_native.h"
#include "idl/room_state.h"

#include <cflow/executor.h>
#include <cflow/machine.h>
#include <cflow/scheduler.h>
#include <cmeta/cmeta.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        abort(); \
    } \
} while (0)

/*
 * Two instance-local Actor/Executor/CallPort/receiver owners for the SAME
 * generated Game.Room IDL contract. The canonical business functions live
 * in test_orbit_generated_idl.c; no global/TLS room lookup, no extra engine.
 */
typedef struct multi_room {
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    cflow_machine_action_binding bindings[2];
    orbit_service service;
    orbit_service_ref producer;
    orbit_call_port calls;
    databind_4_Game_4_Room__orbit_binding binding;
    databind_4_Game_4_Room_4_Ping__orbit_context ping_ctx;
    databind_4_Game_4_Room_4_Pong__orbit_context pong_ctx;
    databind_4_Game_4_Room_4_Ping__orbit_context cancelled_ctx;
    orbit_room_state state;
    int machine_state;
    uint64_t ping_result;
    uint64_t pong_result;
    unsigned callbacks;
    unsigned observations;
    unsigned cancels;
    unsigned errors;
} multi_room;

static bool multi_action(
    void *user, const void *state, const void *event,
    void *next_state, void *observation, const char **error) {
    (void)user;
    if (!state || !event || !next_state || !observation || !error)
        return false;
    *(int *)next_state = *(const int *)state + 1;
    *(uint64_t *)observation = *(const uint64_t *)event;
    *error = NULL;
    return true;
}

static bool multi_observe(void *user, const cmeta_type_desc *type,
                          const void *value) {
    multi_room *room = (multi_room *)user;
    orbit_call_status step;
    CHECK(room && value);
    CHECK(cmeta_type_equal(type, &cmeta_type_uint64));
    step = databind_4_Game_4_Room__orbit_dispatch(
        &room->binding, *(const uint64_t *)value);
    CHECK(step == ORBIT_CALL_OK || step == ORBIT_CALL_STALE);
    ++room->observations;
    return true;
}

static void multi_on_error(void *user, const char *why) {
    multi_room *room = (multi_room *)user;
    (void)why;
    ++room->errors;
    CHECK(orbit_call_port_close(
        &room->calls, ORBIT_CALL_TERMINAL_ACTOR_FAILED) == ORBIT_CALL_OK);
}

static void multi_on_done(void *user) {
    multi_room *room = (multi_room *)user;
    CHECK(orbit_call_port_close(
        &room->calls, ORBIT_CALL_TERMINAL_STOPPED) == ORBIT_CALL_OK);
}

static void multi_ping_complete(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int application_status, const PingResponse *response) {
    multi_room *room = (multi_room *)user;
    CHECK(room && token != 0u);
    CHECK(terminal == ORBIT_CALL_TERMINAL_SUCCESS);
    CHECK(application_status == 0 && response != NULL);
    room->ping_result = response->ack;
    ++room->callbacks;
}

static void multi_pong_complete(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int application_status, const PingResponse *response) {
    multi_room *room = (multi_room *)user;
    CHECK(room && token != 0u);
    CHECK(terminal == ORBIT_CALL_TERMINAL_SUCCESS);
    CHECK(application_status == 0 && response != NULL);
    room->pong_result = response->ack;
    ++room->callbacks;
}

static void multi_cancel_complete(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int application_status, const PingResponse *response) {
    multi_room *room = (multi_room *)user;
    CHECK(room && token != 0u);
    CHECK(terminal == ORBIT_CALL_TERMINAL_CANCELLED);
    CHECK(application_status == 0 && response == NULL);
    ++room->cancels;
}

static void multi_room_init(multi_room *room, uint64_t initial) {
    const cflow_machine_state states[] = {
        {10u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE}
    };
    const cflow_event_type events[] = {
        {100u, &cmeta_type_uint64}, {101u, &cmeta_type_uint64}
    };
    const cflow_machine_action actions[] = {
        {300u, &cmeta_type_int, 100u, &cmeta_type_uint64,
         &cmeta_type_int, CMETA_EFFECT_STATEFUL,
         CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
         CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_uint64, 0u},
        {301u, &cmeta_type_int, 101u, &cmeta_type_uint64,
         &cmeta_type_int, CMETA_EFFECT_STATEFUL,
         CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
         CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_uint64, 0u}
    };
    const cflow_machine_transition transitions[] = {
        {10u, 100u, 0u, 300u, 10u, 1u},
        {10u, 101u, 0u, 301u, 10u, 1u}
    };
    const cflow_machine_definition definition = {
        states, 1u, 10u, events, 2u, NULL, 0u,
        actions, 2u, transitions, 2u
    };
    cflow_actor_config config = {0};
    orbit_service_config service_config;
    memset(room, 0, sizeof(*room));
    room->state.counter = initial;

    CHECK(cflow_machine_build(&room->machine, &definition) == CFLOW_MACHINE_OK);
    CHECK(cflow_executor_owner_init_with_capacity(
        &room->executor, 64u, NULL, NULL));
    CHECK(cflow_scheduler_owner_bind(&room->scheduler, &room->executor, 32u));
    room->bindings[0] = (cflow_machine_action_binding){
        300u, multi_action, NULL
    };
    room->bindings[1] = (cflow_machine_action_binding){
        301u, multi_action, NULL
    };
    config.machine = (cflow_machine_instance_config){
        &room->machine, &room->machine_state, &cmeta_type_uint64,
        NULL, 0u, room->bindings, 2u, 2u, &room->executor
    };
    config.scheduler = &room->scheduler;
    config.callbacks = (cflow_subscriber_callbacks){
        multi_observe, multi_on_error, multi_on_done, room
    };

    service_config = databind_4_Game_4_Room__orbit_service_config(&config);
    CHECK(service_config.method_count == 2u);
    CHECK(orbit_service_spawn(&room->service, &service_config).status ==
          ORBIT_SERVICE_OK);
    CHECK(orbit_call_port_init(&room->calls, &room->service, 2u) ==
          ORBIT_CALL_OK);
    CHECK(databind_4_Game_4_Room__orbit_bind(
        &room->binding, &room->calls, &room->state));
    CHECK(room->binding.receiver == &room->state);
    CHECK(orbit_service_ref_acquire(&room->service, &room->producer));
}

static void multi_room_stop(multi_room *room) {
    unsigned rounds = 0u;
    CHECK(orbit_service_request_stop(&room->service) == CFLOW_ACTOR_OK);
    while (cflow_actor_current_state(&room->service.actor) !=
               CFLOW_ACTOR_STATE_STOPPED && rounds++ < 2048u)
        (void)cflow_executor_run_one(&room->executor);
    CHECK(cflow_actor_current_state(&room->service.actor) ==
          CFLOW_ACTOR_STATE_STOPPED);
    CHECK(orbit_service_wait(&room->service) == CFLOW_ACTOR_STATE_STOPPED);
    orbit_service_ref_release(&room->producer);
    orbit_service_destroy(&room->service);
    CHECK(orbit_call_port_try_destroy(&room->calls) == ORBIT_CALL_OK);
    cflow_scheduler_destroy(&room->scheduler);
    cflow_executor_destroy(&room->executor);
    cflow_machine_destroy(&room->machine);
}

int orbit_generated_multi_room_test(void) {
    multi_room a, b;
    PingRequest a_ping = {UINT64_C(2)};
    PingRequest b_ping = {UINT64_C(2)};
    PingRequest a_pong = {UINT64_C(3)};
    PingRequest b_pong = {UINT64_C(4)};
    PingRequest cancelled = {UINT64_C(1)};
    PingRequest b_more = {UINT64_C(1)};
    orbit_call_port_stats a_stats = {0}, b_stats = {0};
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    uint64_t ta1 = 0u, ta2 = 0u, tb1 = 0u, tb2 = 0u, ta_cancel = 0u;
    unsigned rounds = 0u;
    multi_room_init(&a, UINT64_C(5));
    multi_room_init(&b, UINT64_C(100));
    CHECK(a.binding.receiver != b.binding.receiver);
    CHECK(a.producer.actor.impl != b.producer.actor.impl);

    CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
        &a.calls, &a.producer, &a_ping, &a.ping_ctx,
        multi_ping_complete, &a, &ta1, &send) == ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
        &b.calls, &b.producer, &b_ping, &b.ping_ctx,
        multi_ping_complete, &b, &tb1, &send) == ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(databind_4_Game_4_Room_4_Pong__orbit_call_async(
        &a.calls, &a.producer, &a_pong, &a.pong_ctx,
        multi_pong_complete, &a, &ta2, &send) == ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(databind_4_Game_4_Room_4_Pong__orbit_call_async(
        &b.calls, &b.producer, &b_pong, &b.pong_ctx,
        multi_pong_complete, &b, &tb2, &send) == ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(ta1 && ta2 && tb1 && tb2);

    while ((a.callbacks < 2u || b.callbacks < 2u) && rounds++ < 2048u) {
        (void)cflow_executor_run_one(&a.executor);
        (void)cflow_executor_run_one(&b.executor);
    }
    CHECK(a.callbacks == 2u && b.callbacks == 2u);
    CHECK(a.observations == 2u && b.observations == 2u);
    CHECK(a.errors == 0u && b.errors == 0u);
    CHECK(a.ping_result == UINT64_C(8));
    CHECK(a.pong_result == UINT64_C(111));
    CHECK(b.ping_result == UINT64_C(103));
    CHECK(b.pong_result == UINT64_C(207));
    CHECK(a.state.counter == UINT64_C(111));
    CHECK(b.state.counter == UINT64_C(207));
    CHECK(a.state.ping_count == 1u && a.state.pong_count == 1u);
    CHECK(b.state.ping_count == 1u && b.state.pong_count == 1u);

    CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
        &a.calls, &a.producer, &cancelled, &a.cancelled_ctx,
        multi_cancel_complete, &a, &ta_cancel, &send) == ORBIT_CALL_OK);
    CHECK(orbit_call_u64_cancel(&a.calls, ta_cancel) == ORBIT_CALL_OK);
    CHECK(a.cancels == 1u);
    CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
        &b.calls, &b.producer, &b_more, &b.ping_ctx,
        multi_ping_complete, &b, &tb1, &send) == ORBIT_CALL_OK);
    rounds = 0u;
    while ((a.observations < 3u || b.callbacks < 3u) && rounds++ < 2048u) {
        (void)cflow_executor_run_one(&a.executor);
        (void)cflow_executor_run_one(&b.executor);
    }
    CHECK(a.observations == 3u && a.cancels == 1u);
    CHECK(a.callbacks == 2u && a.state.counter == UINT64_C(111));
    CHECK(b.callbacks == 3u && b.state.counter == UINT64_C(209));
    CHECK(b.ping_result == UINT64_C(209));
    CHECK(a.state.ping_count == 1u);
    CHECK(b.state.ping_count == 2u);
    CHECK(orbit_call_port_get_stats(&a.calls, &a_stats));
    CHECK(orbit_call_port_get_stats(&b.calls, &b_stats));
    CHECK(a_stats.cancelled == 1u && a_stats.completed_success == 2u);
    CHECK(b_stats.completed_success == 3u);
    CHECK(a_stats.occupied == 0u && b_stats.occupied == 0u);

    multi_room_stop(&b);
    multi_room_stop(&a);
    return 0;
}
