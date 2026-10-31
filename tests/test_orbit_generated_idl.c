#define DATABIND_NATIVE_ENABLE_ORBIT 1
#include "game.service_native.h"

#include <cflow/executor.h>
#include <cflow/machine.h>
#include <cflow/scheduler.h>
#include <cmeta/cmeta.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The generated contract is also compiled and linked as actual C++17. */
extern int orbit_generated_cpp17_smoke(void);

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: failed: %s\n", \
                __FILE__, __LINE__, #expression); \
        abort(); \
    } \
} while (0)

/*
 * This source includes the REAL salts-idlc generated typed header. The
 * business methods are the original canonical NATIVE Service signatures,
 * not a duplicate reflected FunctionDesc or another scheduling engine.
 */
int databind_4_Game_4_Room_4_Ping(
    const PingRequest *request, PingResponse *response) {
    if (!request || !response) return 11;
    if (request->seq == UINT64_C(999)) return 17;
    response->ack = request->seq + UINT64_C(1);
    return 0;
}
int databind_4_Game_4_Room_4_Pong(
    const PingRequest *request, PingResponse *response) {
    if (!request || !response) return 12;
    response->ack = request->seq + UINT64_C(100);
    return 0;
}

typedef struct idl_fixture {
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    cflow_machine_action_binding bindings[2];
    orbit_service service;
    orbit_service_ref producer;
    orbit_call_port calls;
    databind_4_Game_4_Room_4_Ping__orbit_context ping_context;
    databind_4_Game_4_Room_4_Pong__orbit_context pong_context;
    int state;
    unsigned callbacks;
    unsigned observed;
    unsigned errors;
    unsigned app_errors;
    unsigned cancelled;
    uint64_t ping_reply;
    uint64_t pong_reply;
} idl_fixture;

static bool idl_machine_action(
    void *user, const void *state, const void *event,
    void *out_state, void *observation, const char **error) {
    (void)user;
    if (!state || !event || !out_state || !observation || !error)
        return false;
    *(int *)out_state = *(const int *)state + 1;
    *(uint64_t *)observation = *(const uint64_t *)event;
    *error = NULL;
    return true;
}

static bool idl_on_value(
    void *user, const cmeta_type_desc *type, const void *value) {
    idl_fixture *fixture = (idl_fixture *)user;
    orbit_call_status status;
    uint64_t token;
    CHECK(fixture && value);
    CHECK(cmeta_type_equal(type, &cmeta_type_uint64));
    token = *(const uint64_t *)value;
    /* Generated canonical dispatch occurs strictly AFTER CFlow commit. */
    status = databind_4_Game_4_Room__orbit_dispatch(
        &fixture->calls, token);
    CHECK(status == ORBIT_CALL_OK || status == ORBIT_CALL_STALE);
    ++fixture->observed;
    return true;
}

static void idl_on_error(void *user, const char *message) {
    idl_fixture *fixture = (idl_fixture *)user;
    (void)message;
    ++fixture->errors;
    CHECK(orbit_call_port_close(
        &fixture->calls, ORBIT_CALL_TERMINAL_ACTOR_FAILED) == ORBIT_CALL_OK);
}

static void idl_on_done(void *user) {
    idl_fixture *fixture = (idl_fixture *)user;
    CHECK(orbit_call_port_close(
        &fixture->calls, ORBIT_CALL_TERMINAL_STOPPED) == ORBIT_CALL_OK);
}

static void on_ping(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int app_status, const PingResponse *response) {
    idl_fixture *fixture = (idl_fixture *)user;
    CHECK(fixture && token != 0u && response);
    CHECK(terminal == ORBIT_CALL_TERMINAL_SUCCESS && app_status == 0);
    fixture->ping_reply = response->ack; /* Copy borrowed reply in callback. */
    ++fixture->callbacks;
}

static void on_pong(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int app_status, const PingResponse *response) {
    idl_fixture *fixture = (idl_fixture *)user;
    CHECK(fixture && token != 0u && response);
    CHECK(terminal == ORBIT_CALL_TERMINAL_SUCCESS && app_status == 0);
    fixture->pong_reply = response->ack;
    ++fixture->callbacks;
}

static void on_ping_application_error(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int app_status, const PingResponse *response) {
    idl_fixture *fixture = (idl_fixture *)user;
    CHECK(fixture != NULL && token != 0u);
    CHECK(terminal == ORBIT_CALL_TERMINAL_APPLICATION_ERROR);
    CHECK(app_status == 17);
    CHECK(response == NULL); /* Generated wrapper must not invent success. */
    ++fixture->app_errors;
}

static void on_ping_cancelled(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int app_status, const PingResponse *response) {
    idl_fixture *fixture = (idl_fixture *)user;
    CHECK(fixture != NULL && token != 0u);
    CHECK(terminal == ORBIT_CALL_TERMINAL_CANCELLED);
    CHECK(app_status == 0);
    CHECK(response == NULL);
    ++fixture->cancelled;
}

static void test_generated_unary_dispatch(void) {
    idl_fixture f;
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
    cflow_actor_config actor_config = {0};
    orbit_service_config service_config;
    PingRequest ping = {UINT64_C(5)};
    PingRequest pong = {UINT64_C(7)};
    PingRequest rejected = {UINT64_C(9)};
    PingRequest failure_request = {UINT64_C(999)};
    PingRequest cancelled_request = {UINT64_C(8)};
    databind_4_Game_4_Room_4_Ping__orbit_context rejected_context = {0};
    databind_4_Game_4_Room_4_Ping__orbit_context failure_context = {0};
    databind_4_Game_4_Room_4_Ping__orbit_context cancelled_context = {0};
    cflow_actor_send_status send_status = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    orbit_call_port_stats stats = {0};
    uint64_t first = 0u, second = 0u, third = UINT64_MAX;
    unsigned rounds = 0u;

    memset(&f, 0, sizeof(f));
    CHECK(cflow_machine_build(&f.machine, &definition) == CFLOW_MACHINE_OK);
    CHECK(cflow_executor_owner_init_with_capacity(
        &f.executor, 64u, NULL, NULL));
    CHECK(cflow_scheduler_owner_bind(&f.scheduler, &f.executor, 32u));
    f.bindings[0] = (cflow_machine_action_binding){
        300u, idl_machine_action, NULL
    };
    f.bindings[1] = (cflow_machine_action_binding){
        301u, idl_machine_action, NULL
    };
    actor_config.machine = (cflow_machine_instance_config){
        &f.machine, &f.state, &cmeta_type_uint64,
        NULL, 0u, f.bindings, 2u, 2u, &f.executor
    };
    actor_config.scheduler = &f.scheduler;
    actor_config.callbacks = (cflow_subscriber_callbacks){
        idl_on_value, idl_on_error, idl_on_done, &f
    };

    /* The Service method table is generated by the only salts-idlc owner. */
    service_config = databind_4_Game_4_Room__orbit_service_config(
        &actor_config);
    CHECK(service_config.method_count == 2u);
    CHECK(service_config.methods[0].id == UINT64_C(10));
    CHECK(service_config.methods[0].event_id == 100u);
    CHECK(service_config.methods[1].id == UINT64_C(20));
    CHECK(service_config.methods[1].event_id == 101u);
    CHECK(orbit_service_spawn(&f.service, &service_config).status ==
          ORBIT_SERVICE_OK);
    CHECK(orbit_call_port_init(&f.calls, &f.service, 2u) ==
          ORBIT_CALL_OK);
    CHECK(orbit_service_ref_acquire(&f.service, &f.producer));

    CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
        &f.calls, &f.producer, &ping, &f.ping_context, on_ping, &f,
        &first, &send_status) == ORBIT_CALL_OK);
    CHECK(send_status == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(databind_4_Game_4_Room_4_Pong__orbit_call_async(
        &f.calls, &f.producer, &pong, &f.pong_context, on_pong, &f,
        &second, &send_status) == ORBIT_CALL_OK);
    CHECK(send_status == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(first != 0u && second != 0u && first != second);

    /* The same generated client refuses a third call before owner progress:
     * bounded response credits are the existing Orbit CallPort authority.
     */
    CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
        &f.calls, &f.producer, &rejected, &rejected_context, on_ping, &f,
        &third, &send_status) == ORBIT_CALL_FULL);
    CHECK(third == 0u && f.callbacks == 0u);

    while (f.callbacks < 2u && rounds++ < 2048u)
        (void)cflow_executor_run_one(&f.executor);
    CHECK(f.callbacks == 2u && f.observed == 2u && f.errors == 0u);
    CHECK(f.ping_reply == UINT64_C(6));
    CHECK(f.pong_reply == UINT64_C(107));
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.completed_success == 2u && stats.occupied == 0u);
    CHECK(stats.rejected_full == 1u);
    CHECK(orbit_call_u64_observe(&f.calls, first) == ORBIT_CALL_STALE);

    /* Native function returned an application error. The typed generated
     * client must forward the exact error and a NULL response view.
     */
    third = 0u;
    CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
        &f.calls, &f.producer, &failure_request, &failure_context,
        on_ping_application_error, &f, &third, &send_status) ==
          ORBIT_CALL_OK);
    CHECK(third != 0u && send_status == CFLOW_ACTOR_SEND_ACCEPTED);
    rounds = 0u;
    while (f.app_errors == 0u && rounds++ < 2048u)
        (void)cflow_executor_run_one(&f.executor);
    CHECK(f.app_errors == 1u);
    CHECK(orbit_call_u64_observe(&f.calls, third) == ORBIT_CALL_STALE);

    /* A cancelled generation token cannot execute the native business
     * method or receive an invented successful typed response.
     */
    third = 0u;
    CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
        &f.calls, &f.producer, &cancelled_request, &cancelled_context,
        on_ping_cancelled, &f, &third, &send_status) == ORBIT_CALL_OK);
    CHECK(third != 0u && send_status == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(orbit_call_u64_cancel(&f.calls, third) == ORBIT_CALL_OK);
    CHECK(f.cancelled == 1u);
    CHECK(orbit_call_u64_cancel(&f.calls, third) == ORBIT_CALL_STALE);
    rounds = 0u;
    while (f.observed < 4u && rounds++ < 2048u)
        (void)cflow_executor_run_one(&f.executor);
    CHECK(f.observed == 4u && f.errors == 0u);
    CHECK(f.callbacks == 2u && f.app_errors == 1u && f.cancelled == 1u);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.admitted == 4u);
    CHECK(stats.completed_success == 2u);
    CHECK(stats.completed_error == 1u);
    CHECK(stats.cancelled == 1u && stats.occupied == 0u);

    CHECK(orbit_service_request_stop(&f.service) == CFLOW_ACTOR_OK);
    rounds = 0u;
    while (cflow_actor_current_state(&f.service.actor) !=
               CFLOW_ACTOR_STATE_STOPPED && rounds++ < 2048u)
        (void)cflow_executor_run_one(&f.executor);
    CHECK(orbit_service_wait(&f.service) == CFLOW_ACTOR_STATE_STOPPED);
    orbit_service_ref_release(&f.producer);
    orbit_service_destroy(&f.service);
    CHECK(orbit_call_port_try_destroy(&f.calls) == ORBIT_CALL_OK);
    cflow_scheduler_destroy(&f.scheduler);
    cflow_executor_destroy(&f.executor);
    cflow_machine_destroy(&f.machine);
}

int main(void) {
    CHECK(orbit_generated_cpp17_smoke() == 0);
    test_generated_unary_dispatch();
    return 0;
}
