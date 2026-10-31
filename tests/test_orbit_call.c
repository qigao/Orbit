#include <orbit/call.h>
#include <orbit/service.h>

#include <cflow/executor.h>
#include <cflow/machine.h>
#include <cflow/scheduler.h>
#include <cmeta/cmeta.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        abort(); \
    } \
} while (0)

typedef struct call_probe {
    orbit_call_port *port;
    atomic_int calls;
    atomic_int action_executions;
    atomic_int business_executions;
    atomic_int skipped_cancelled;
    atomic_int observed;
    atomic_int done;
    atomic_int errors;
    atomic_int successes;
    atomic_int cancelled;
    atomic_int stopped;
    atomic_int application_errors;
    atomic_int no_reply;
    atomic_int callback_count;
    atomic_uint_fast64_t latest_response;
    atomic_uint_fast64_t latest_token;
    atomic_bool block_action;
    atomic_bool action_entered;
    atomic_bool release_action;
    atomic_bool block_terminal;
    atomic_bool terminal_entered;
    atomic_bool release_terminal;
} call_probe;

typedef struct call_fixture {
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    orbit_service service;
    orbit_service_ref ref;
    orbit_call_port calls;
    cflow_machine_action_binding action[2];
    cflow_actor_config actor_config;
    orbit_service_method methods[2];
    orbit_service_config service_config;
    int state;
    call_probe probe;
} call_fixture;

static bool call_action(void *user, const void *state, const void *event,
                        void *out_state, void *out_observation,
                        const char **out_error) {
    call_fixture *fixture = (call_fixture *)user;
    uint64_t token;

    if (fixture == NULL || state == NULL || event == NULL ||
        out_state == NULL || out_observation == NULL || out_error == NULL)
        return false;
    token = *(const uint64_t *)event;

    /*
     * CFlow Machine Actions REQUIRE DETERMINISTIC | NO_ALIAS. Therefore
     * this action never reads the mutable call port, executes business code,
     * or stages a reply before the CFlow state transition has committed.
     *
     * An optional test-only barrier changes WHEN this pure action completes,
     * never its output or effects.
     */
    atomic_fetch_add(&fixture->probe.action_executions, 1);
    if (atomic_load(&fixture->probe.block_action)) {
        size_t checks = 0u;
        atomic_store(&fixture->probe.action_entered, true);
        while (!atomic_load(&fixture->probe.release_action) && checks++ < 5000u)
            cmeta_sleep_ms(1u);
        CHECK(atomic_load(&fixture->probe.release_action));
    }
    *(int *)out_state = *(const int *)state + 1;
    *(uint64_t *)out_observation = token;
    *out_error = NULL;
    return true;
}

static bool call_observe(void *user, const cmeta_type_desc *type,
                         const void *value) {
    call_fixture *fixture = (call_fixture *)user;
    orbit_call_status status;
    uint64_t request = 0u;
    uint64_t token;

    CHECK(fixture != NULL);
    CHECK(value != NULL);
    CHECK(cmeta_type_equal(type, &cmeta_type_uint64));
    token = *(const uint64_t *)value;

    /*
     * This CFlow Subscriber is invoked AFTER the Machine transition commit.
     * Dispatching here is deliberately separate from the deterministic
     * Machine Action; it may produce application side effects and errors.
     */
    {
        orbit_method_id actual_method = 0u;
        status = orbit_call_u64_method(
            &fixture->calls, token, &actual_method);
        if (status == ORBIT_CALL_OK) {
            CHECK(actual_method == 10u || actual_method == 20u);
            status = orbit_call_u64_request(
                &fixture->calls, token, actual_method, &request);
            if (status == ORBIT_CALL_OK) {
                atomic_fetch_add(&fixture->probe.business_executions, 1);
                if (request != 12345u) {
                    const uint64_t response = request +
                        (actual_method == 10u ? 1u : 100u);
                    status = orbit_call_u64_stage_reply(
                        &fixture->calls, token, actual_method,
                        request == 999u ? 17 : 0, response);
                    CHECK(status == ORBIT_CALL_OK ||
                          status == ORBIT_CALL_STALE);
                }
            }
        }
        if (status == ORBIT_CALL_STALE) {
            atomic_fetch_add(&fixture->probe.skipped_cancelled, 1);
        } else {
            CHECK(status == ORBIT_CALL_OK);
        }
    }
    status = orbit_call_u64_observe(&fixture->calls, token);
    CHECK(status == ORBIT_CALL_OK || status == ORBIT_CALL_STALE);
    atomic_fetch_add(&fixture->probe.observed, 1);
    return true;
}

static void call_on_error(void *user, const char *message) {
    call_fixture *fixture = (call_fixture *)user;
    (void)message;
    atomic_fetch_add(&fixture->probe.errors, 1);
    CHECK(orbit_call_port_close(
        &fixture->calls, ORBIT_CALL_TERMINAL_ACTOR_FAILED) == ORBIT_CALL_OK);
}

static void call_on_done(void *user) {
    call_fixture *fixture = (call_fixture *)user;
    atomic_fetch_add(&fixture->probe.done, 1);
    CHECK(orbit_call_port_close(
        &fixture->calls, ORBIT_CALL_TERMINAL_STOPPED) == ORBIT_CALL_OK);
}

static void call_terminal(void *user, uint64_t token,
                          orbit_call_terminal outcome,
                          int application_status, uint64_t response) {
    call_probe *probe = (call_probe *)user;
    CHECK(probe != NULL && token != 0u);
    atomic_store(&probe->latest_token, token);
    atomic_store(&probe->latest_response, response);
    atomic_fetch_add(&probe->callback_count, 1);
    /* The callback can re-enter the port because it is NOT called
     * underneath its mutex. Its own token has already been retired.
     */
    if (probe->port != NULL) {
        orbit_call_port_stats stats = {0};
        CHECK(orbit_call_u64_cancel(probe->port, token) == ORBIT_CALL_STALE);
        CHECK(orbit_call_port_get_stats(probe->port, &stats));
        CHECK(stats.callbacks_inflight >= 1u);
    }
    switch (outcome) {
        case ORBIT_CALL_TERMINAL_SUCCESS:
            CHECK(application_status == 0);
            atomic_fetch_add(&probe->successes, 1);
            break;
        case ORBIT_CALL_TERMINAL_APPLICATION_ERROR:
            CHECK(application_status == 17);
            atomic_fetch_add(&probe->application_errors, 1);
            break;
        case ORBIT_CALL_TERMINAL_NO_REPLY:
            atomic_fetch_add(&probe->no_reply, 1);
            break;
        case ORBIT_CALL_TERMINAL_CANCELLED:
            atomic_fetch_add(&probe->cancelled, 1);
            break;
        case ORBIT_CALL_TERMINAL_STOPPED:
            atomic_fetch_add(&probe->stopped, 1);
            break;
        default:
            CHECK(0 && "unexpected terminal");
            break;
    }
    if (atomic_load(&probe->block_terminal)) {
        size_t checks = 0u;
        atomic_store(&probe->terminal_entered, true);
        while (!atomic_load(&probe->release_terminal) && checks++ < 5000u)
            cmeta_sleep_ms(1u);
        CHECK(atomic_load(&probe->release_terminal));
    }
}

static void fixture_prepare(call_fixture *fixture, size_t mailbox_capacity,
                            size_t call_capacity) {
    const cflow_machine_state states[] = {
        {10u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE}
    };
    const cflow_event_type events[] = {
        {100u, &cmeta_type_uint64},
        {101u, &cmeta_type_uint64}
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
    memset(fixture, 0, sizeof(*fixture));
    fixture->probe.port = &fixture->calls;
    /* An all-zero byte image is not a substitute for C11 atomic_init. */
    atomic_init(&fixture->probe.calls, 0);
    atomic_init(&fixture->probe.action_executions, 0);
    atomic_init(&fixture->probe.business_executions, 0);
    atomic_init(&fixture->probe.skipped_cancelled, 0);
    atomic_init(&fixture->probe.observed, 0);
    atomic_init(&fixture->probe.done, 0);
    atomic_init(&fixture->probe.errors, 0);
    atomic_init(&fixture->probe.successes, 0);
    atomic_init(&fixture->probe.cancelled, 0);
    atomic_init(&fixture->probe.stopped, 0);
    atomic_init(&fixture->probe.application_errors, 0);
    atomic_init(&fixture->probe.no_reply, 0);
    atomic_init(&fixture->probe.callback_count, 0);
    atomic_init(&fixture->probe.latest_response, UINT64_C(0));
    atomic_init(&fixture->probe.latest_token, UINT64_C(0));
    atomic_init(&fixture->probe.block_action, false);
    atomic_init(&fixture->probe.action_entered, false);
    atomic_init(&fixture->probe.release_action, false);
    atomic_init(&fixture->probe.block_terminal, false);
    atomic_init(&fixture->probe.terminal_entered, false);
    atomic_init(&fixture->probe.release_terminal, false);
    CHECK(cflow_machine_build(&fixture->machine, &definition) == CFLOW_MACHINE_OK);
    CHECK(cflow_executor_owner_init_with_capacity(
        &fixture->executor, 64u, NULL, NULL));
    CHECK(cflow_scheduler_owner_bind(
        &fixture->scheduler, &fixture->executor, 32u));

    fixture->action[0] = (cflow_machine_action_binding){
        300u, call_action, fixture
    };
    fixture->action[1] = (cflow_machine_action_binding){
        301u, call_action, fixture
    };
    fixture->actor_config.machine = (cflow_machine_instance_config){
        &fixture->machine, &fixture->state, &cmeta_type_uint64,
        NULL, 0u, fixture->action, 2u,
        mailbox_capacity, &fixture->executor
    };
    fixture->actor_config.scheduler = &fixture->scheduler;
    fixture->actor_config.callbacks = (cflow_subscriber_callbacks){
        call_observe, call_on_error, call_on_done, fixture
    };
    fixture->methods[0] = (orbit_service_method){
        10u, 100u, &cmeta_type_uint64, ORBIT_METHOD_CALL_TOKEN
    };
    fixture->methods[1] = (orbit_service_method){
        20u, 101u, &cmeta_type_uint64, ORBIT_METHOD_CALL_TOKEN
    };
    fixture->service_config = (orbit_service_config){
        &fixture->actor_config, fixture->methods, 2u
    };
    CHECK(orbit_service_spawn(&fixture->service, &fixture->service_config)
          .status == ORBIT_SERVICE_OK);
    CHECK(orbit_call_port_init(
        &fixture->calls, &fixture->service, call_capacity) == ORBIT_CALL_OK);
    CHECK(orbit_service_ref_acquire(&fixture->service, &fixture->ref));
    {
        const uint64_t forged_token = UINT64_C(1) << 32 | UINT64_C(1);
        CHECK(orbit_service_ref_try_send(&fixture->ref, 10u, &forged_token)
              == CFLOW_ACTOR_SEND_INVALID_ARGUMENT);
    }
}

static void fixture_drive_until(
    call_fixture *fixture, atomic_int *counter, int target) {
    size_t rounds = 0u;
    while (atomic_load(counter) < target && rounds++ < 2048u)
        (void)cflow_executor_run_one(&fixture->executor);
    CHECK(atomic_load(counter) >= target);
}

static void fixture_destroy(call_fixture *fixture) {
    size_t rounds = 0u;
    orbit_service_ref_release(&fixture->ref);
    CHECK(orbit_service_request_stop(&fixture->service) == CFLOW_ACTOR_OK ||
          cflow_actor_current_state(&fixture->service.actor) ==
              CFLOW_ACTOR_STATE_STOPPED);
    while (cflow_actor_current_state(&fixture->service.actor) !=
               CFLOW_ACTOR_STATE_STOPPED && rounds++ < 2048u)
        (void)cflow_executor_run_one(&fixture->executor);
    CHECK(cflow_actor_current_state(&fixture->service.actor) ==
          CFLOW_ACTOR_STATE_STOPPED);
    CHECK(orbit_service_wait(&fixture->service) == CFLOW_ACTOR_STATE_STOPPED);
    orbit_service_destroy(&fixture->service);
    CHECK(orbit_call_port_try_destroy(&fixture->calls) == ORBIT_CALL_OK);
    cflow_scheduler_destroy(&fixture->scheduler);
    cflow_executor_destroy(&fixture->executor);
    cflow_machine_destroy(&fixture->machine);
}

/* Case 1: successful call, exact credit and a CFlow Mailbox rejection. */
static void test_mailbox_rejection_and_postcommit(void) {
    call_fixture f;
    orbit_call_port_stats stats;
    uint64_t token = 0u;
    uint64_t rejected = 999u;
    cflow_actor_send_status send_status;
    fixture_prepare(&f, 1u, 2u);

    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 41u, call_terminal,
        &f.probe, &token, &send_status) == ORBIT_CALL_OK);
    CHECK(token != 0u && send_status == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(atomic_load(&f.probe.callback_count) == 0);
    {
        orbit_method_id actual = 0u;
        CHECK(orbit_call_u64_method(&f.calls, token, &actual) ==
              ORBIT_CALL_OK);
        CHECK(actual == 10u);
        CHECK(orbit_call_u64_method(&f.calls, token, NULL) ==
              ORBIT_CALL_INVALID_ARGUMENT);
    }
    {
        uint64_t probe_request = 0u;
        CHECK(orbit_call_u64_request(
            &f.calls, token, 20u, &probe_request) == ORBIT_CALL_TYPE_MISMATCH);
        CHECK(orbit_call_u64_stage_reply(
            &f.calls, token, 20u, 0, 5u) == ORBIT_CALL_TYPE_MISMATCH);
    }

    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 42u, call_terminal,
        &f.probe, &rejected, &send_status) == ORBIT_CALL_REJECTED);
    CHECK(rejected == 0u && send_status == CFLOW_ACTOR_SEND_FULL);
    CHECK(atomic_load(&f.probe.callback_count) == 0);

    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.capacity == 2u && stats.occupied == 1u);
    CHECK(stats.rejected == 1u);
    fixture_drive_until(&f, &f.probe.successes, 1);
    CHECK(atomic_load(&f.probe.callback_count) == 1);
    CHECK(atomic_load(&f.probe.latest_response) == 42u);
    {
        orbit_method_id unchanged = UINT64_C(987);
        CHECK(orbit_call_u64_method(&f.calls, token, &unchanged) ==
              ORBIT_CALL_STALE);
        CHECK(unchanged == UINT64_C(987));
    }
    CHECK(atomic_load(&f.probe.action_executions) == 1);
    CHECK(atomic_load(&f.probe.business_executions) == 1);
    CHECK(orbit_call_u64_observe(&f.calls, token) == ORBIT_CALL_STALE);
    CHECK(orbit_call_u64_stage_reply(&f.calls, token, 10u, 0, 10u) ==
          ORBIT_CALL_STALE);

    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.admitted == 1u && stats.completed_success == 1u &&
          stats.occupied == 0u);
    /* A committed CFlow observation without a staged business reply is a
     * terminal protocol failure, never a hung or invented success.
     */
    {
        uint64_t forgotten = 0u;
        CHECK(orbit_call_u64_async(
            &f.calls, &f.ref, 10u, 12345u, call_terminal,
            &f.probe, &forgotten, &send_status) == ORBIT_CALL_OK);
        fixture_drive_until(&f, &f.probe.no_reply, 1);
        CHECK(atomic_load(&f.probe.callback_count) == 2);
        CHECK(orbit_call_u64_cancel(&f.calls, forgotten) == ORBIT_CALL_STALE);
    }
    fixture_destroy(&f);
}

/* Case 2: fail-closed capacity, cancel old token, no slot ABA reply. */
static void test_cancel_credit_reuse(void) {
    call_fixture f;
    orbit_call_port_stats stats;
    uint64_t first = 0u, second = 0u, third = 0u;
    cflow_actor_send_status send;
    fixture_prepare(&f, 2u, 2u);
    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 11u, call_terminal,
        &f.probe, &first, &send) == ORBIT_CALL_OK);
    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 999u, call_terminal,
        &f.probe, &second, &send) == ORBIT_CALL_OK);
    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 55u, call_terminal,
        &f.probe, &third, &send) == ORBIT_CALL_FULL);
    CHECK(third == 0u);
    CHECK(orbit_call_u64_cancel(&f.calls, first) == ORBIT_CALL_OK);
    CHECK(atomic_load(&f.probe.cancelled) == 1);
    CHECK(orbit_call_u64_request(&f.calls, first, 10u, &third) == ORBIT_CALL_STALE);
    fixture_drive_until(&f, &f.probe.callback_count, 2);
    CHECK(atomic_load(&f.probe.successes) == 0);
    CHECK(atomic_load(&f.probe.application_errors) == 1);
    CHECK(atomic_load(&f.probe.skipped_cancelled) == 1);
    CHECK(atomic_load(&f.probe.latest_response) == 1000u);

    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 7u, call_terminal,
        &f.probe, &third, &send) == ORBIT_CALL_OK);
    CHECK(third != first);
    CHECK(orbit_call_u64_stage_reply(&f.calls, first, 10u, 0, 111u) ==
          ORBIT_CALL_STALE);
    fixture_drive_until(&f, &f.probe.callback_count, 3);
    CHECK(atomic_load(&f.probe.successes) == 1);
    CHECK(atomic_load(&f.probe.latest_response) == 8u);
    CHECK(orbit_call_u64_cancel(&f.calls, second) == ORBIT_CALL_STALE);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.admitted == 3u && stats.cancelled == 1u &&
          stats.completed_error == 1u && stats.completed_success == 1u);
    fixture_destroy(&f);
}

/* Case 3: closing cannot invent delivery success for an accepted call. */
static void test_close_and_stale_terminal(void) {
    call_fixture f;
    orbit_call_port_stats stats;
    uint64_t token = 0u;
    cflow_actor_send_status send;
    fixture_prepare(&f, 1u, 1u);
    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 101u, call_terminal,
        &f.probe, &token, &send) == ORBIT_CALL_OK);
    CHECK(orbit_call_port_try_destroy(&f.calls) == ORBIT_CALL_BUSY);
    CHECK(orbit_call_port_close(
        &f.calls, ORBIT_CALL_TERMINAL_STOPPED) == ORBIT_CALL_OK);
    CHECK(atomic_load(&f.probe.callback_count) == 1);
    CHECK(atomic_load(&f.probe.stopped) == 1);
    /* A closed pool is NOT proof the CFlow Actor has quiesced. */
    CHECK(orbit_call_port_try_destroy(&f.calls) == ORBIT_CALL_BUSY);
    CHECK(orbit_call_u64_cancel(&f.calls, token) == ORBIT_CALL_STALE);
    CHECK(orbit_call_u64_stage_reply(&f.calls, token, 10u, 0, 50u) ==
          ORBIT_CALL_STALE);
    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 1u, call_terminal,
        &f.probe, &token, &send) == ORBIT_CALL_CLOSED);
    CHECK(token == 0u);

    fixture_drive_until(&f, &f.probe.observed, 1);
    CHECK(atomic_load(&f.probe.callback_count) == 1);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.closed && stats.occupied == 0u && stats.stopped == 1u);
    fixture_destroy(&f);
}

typedef struct cancel_during_action_context {
    orbit_call_port *port;
    call_probe *probe;
    uint64_t token;
    atomic_int result;
} cancel_during_action_context;

static void cancel_while_actor_executing(void *user) {
    cancel_during_action_context *ctx = (cancel_during_action_context *)user;
    size_t checks = 0u;
    while (!atomic_load(&ctx->probe->action_entered) && checks++ < 5000u)
        cmeta_sleep_ms(1u);
    if (!atomic_load(&ctx->probe->action_entered)) {
        atomic_store(&ctx->result, ORBIT_CALL_BUSY);
        atomic_store(&ctx->probe->release_action, true);
        return;
    }
    atomic_store(&ctx->result, (int)orbit_call_u64_cancel(
        ctx->port, ctx->token));
    atomic_store(&ctx->probe->release_action, true);
}

static void test_cancel_during_real_actor_action(void) {
    call_fixture f;
    cancel_during_action_context ctx;
    cmeta_thread_t thread = NULL;
    uint64_t token = 0u;
    cflow_actor_send_status send;
    size_t rounds = 0u;

    fixture_prepare(&f, 1u, 1u);
    atomic_store(&f.probe.block_action, true);
    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 71u, call_terminal,
        &f.probe, &token, &send) == ORBIT_CALL_OK);
    memset(&ctx, 0, sizeof(ctx));
    ctx.port = &f.calls;
    ctx.probe = &f.probe;
    ctx.token = token;
    atomic_init(&ctx.result, ORBIT_CALL_BUSY);
    CHECK(cmeta_thread_create(
        &thread, cancel_while_actor_executing, &ctx) == 0);

    /* The fixed CFlow owner executes the action. Another native thread
     * cancels the call while the action is already in flight.
     */
    while (!atomic_load(&f.probe.action_entered) && rounds++ < 512u)
        (void)cflow_executor_run_one(&f.executor);
    CHECK(cmeta_thread_join(&thread) == 0);
    CHECK(atomic_load(&ctx.result) == ORBIT_CALL_OK);
    CHECK(atomic_load(&f.probe.cancelled) == 1);
    CHECK(atomic_load(&f.probe.action_executions) == 1);

    fixture_drive_until(&f, &f.probe.observed, 1);
    CHECK(atomic_load(&f.probe.business_executions) == 0);
    CHECK(atomic_load(&f.probe.callback_count) == 1);
    CHECK(atomic_load(&f.probe.successes) == 0);
    CHECK(orbit_call_u64_stage_reply(&f.calls, token, 10u, 0, 72u) ==
          ORBIT_CALL_STALE);
    fixture_destroy(&f);
}

/*
 * One blocked terminal callback must HOLD its call-slot credit.
 * A second real thread must observe FULL instead of spinning up another
 * callback on the same slot before the first terminal has returned.
 */
typedef struct terminal_credit_context {
    call_fixture *fixture;
    atomic_int result;
    atomic_int in_flight;
} terminal_credit_context;

static void attempt_while_terminal_is_running(void *user) {
    terminal_credit_context *ctx = (terminal_credit_context *)user;
    call_fixture *f = ctx->fixture;
    uint64_t rejected_token = UINT64_MAX;
    cflow_actor_send_status rejected_send = CFLOW_ACTOR_SEND_ACCEPTED;
    orbit_call_port_stats stats = {0};
    size_t checks = 0u;

    while (!atomic_load(&f->probe.terminal_entered) && checks++ < 5000u)
        cmeta_sleep_ms(1u);

    if (atomic_load(&f->probe.terminal_entered)) {
        const orbit_call_status result = orbit_call_u64_async(
            &f->calls, &f->ref, 10u, 72u, call_terminal, &f->probe,
            &rejected_token, &rejected_send);
        CHECK(rejected_token == 0u);
        CHECK(orbit_call_port_get_stats(&f->calls, &stats));
        CHECK(stats.callbacks_inflight == 1u);
        CHECK(stats.occupied == 1u);
        atomic_store(&ctx->in_flight, (int)stats.callbacks_inflight);
        atomic_store(&ctx->result, (int)result);
    }
    atomic_store(&f->probe.release_terminal, true);
}

static void test_terminal_callback_holds_credit(void) {
    call_fixture f;
    terminal_credit_context ctx;
    cmeta_thread_t thread = NULL;
    cflow_actor_send_status send;
    orbit_call_port_stats stats;
    uint64_t token = 0u;

    fixture_prepare(&f, 2u, 1u);
    atomic_store(&f.probe.block_terminal, true);
    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 41u, call_terminal,
        &f.probe, &token, &send) == ORBIT_CALL_OK);

    memset(&ctx, 0, sizeof(ctx));
    ctx.fixture = &f;
    atomic_init(&ctx.result, ORBIT_CALL_BUSY);
    atomic_init(&ctx.in_flight, 0);
    CHECK(cmeta_thread_create(
        &thread, attempt_while_terminal_is_running, &ctx) == 0);

    fixture_drive_until(&f, &f.probe.observed, 1);
    CHECK(cmeta_thread_join(&thread) == 0);
    CHECK(atomic_load(&ctx.result) == ORBIT_CALL_FULL);
    CHECK(atomic_load(&ctx.in_flight) == 1);
    CHECK(atomic_load(&f.probe.successes) == 1);

    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.occupied == 0u);
    CHECK(stats.callbacks_inflight == 0u);
    CHECK(stats.rejected_full == 1u);

    /* Only after the callback returns is the SAME credit reusable. */
    atomic_store(&f.probe.block_terminal, false);
    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 73u, call_terminal,
        &f.probe, &token, &send) == ORBIT_CALL_OK);
    fixture_drive_until(&f, &f.probe.successes, 2);
    CHECK(atomic_load(&f.probe.latest_response) == 74u);
    fixture_destroy(&f);
}

/*
 * A Call Port pins exactly one Actor AND exactly one immutable method table.
 * A producer ref for a different live Actor or a forged catalog cannot
 * claim that port's call credits or submit an Event.
 */
static void test_foreign_actor_and_catalog_rejected(void) {
    call_fixture first;
    call_fixture second;
    orbit_service_ref forged = {0};
    orbit_service_method foreign_methods[1] = {
        {10u, 100u, &cmeta_type_uint64, ORBIT_METHOD_CALL_TOKEN}
    };
    uint64_t token = UINT64_MAX;
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_ACCEPTED;

    fixture_prepare(&first, 2u, 2u);
    fixture_prepare(&second, 2u, 2u);
    CHECK(orbit_call_port_is_bound_to(&first.calls, &first.service));
    CHECK(!orbit_call_port_is_bound_to(&first.calls, &second.service));
    CHECK(orbit_call_port_is_bound_to(&second.calls, &second.service));

    CHECK(orbit_call_u64_async(
        &first.calls, &second.ref, 10u, 55u, call_terminal,
        &first.probe, &token, &send) == ORBIT_CALL_INVALID_ARGUMENT);
    CHECK(token == 0u);
    CHECK(send == CFLOW_ACTOR_SEND_INVALID_ARGUMENT);

    CHECK(orbit_service_ref_retain(&first.ref, &forged));
    /* A separate catalog—even with identical rows—is NOT the bound owner. */
    forged.methods = foreign_methods;
    token = UINT64_MAX;
    CHECK(orbit_call_u64_async(
        &first.calls, &forged, 10u, 65u, call_terminal,
        &first.probe, &token, &send) == ORBIT_CALL_INVALID_ARGUMENT);
    CHECK(token == 0u);
    orbit_service_ref_release(&forged);
    CHECK(atomic_load(&first.probe.callback_count) == 0);

    fixture_destroy(&second);
    fixture_destroy(&first);
}

/* Two different Service method IDs share the one real CFlow Actor,
 * but the post-commit observer retrieves the exact recorded method identity
 * from the generation-checked token. This is the runtime contract consumed
 * by the generated `salts-idlc` Service dispatcher.
 */
static void test_two_method_owner_dispatch(void) {
    call_fixture f;
    uint64_t first = 0u;
    uint64_t second = 0u;
    orbit_method_id id = 0u;
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    orbit_call_port_stats stats = {0};
    fixture_prepare(&f, 2u, 2u);

    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 5u, call_terminal,
        &f.probe, &first, &send) == ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 20u, 7u, call_terminal,
        &f.probe, &second, &send) == ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(first != 0u && second != 0u && first != second);
    CHECK(orbit_call_u64_method(&f.calls, first, &id) == ORBIT_CALL_OK);
    CHECK(id == 10u);
    CHECK(orbit_call_u64_method(&f.calls, second, &id) == ORBIT_CALL_OK);
    CHECK(id == 20u);
    CHECK(orbit_call_u64_request(
        &f.calls, second, 10u, &id) == ORBIT_CALL_TYPE_MISMATCH);

    fixture_drive_until(&f, &f.probe.successes, 2);
    CHECK(atomic_load(&f.probe.observed) == 2);
    CHECK(atomic_load(&f.probe.business_executions) == 2);
    CHECK(atomic_load(&f.probe.latest_response) == 107u);
    CHECK(orbit_call_u64_method(&f.calls, first, &id) == ORBIT_CALL_STALE);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.completed_success == 2u && stats.occupied == 0u);
    fixture_destroy(&f);
}

int main(void) {
    test_mailbox_rejection_and_postcommit();
    test_cancel_credit_reuse();
    test_close_and_stale_terminal();
    test_cancel_during_real_actor_action();
    test_terminal_callback_holds_credit();
    test_foreign_actor_and_catalog_rejected();
    test_two_method_owner_dispatch();
    return 0;
}
