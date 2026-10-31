#include <orbit/call.h>
#include <orbit/service.h>

#include <cflow/executor.h>
#include <cflow/machine.h>
#include <cflow/scheduler.h>
#include <cmeta/cmeta.h>
#include <cmeta/data.h>
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


/*
 * Test-only, fault-injectable CMeta DATA_BYTES v2 provider. It is NOT a
 * second production buffer engine: the real Orbit Port never inspects these
 * fields and uses only the same canonical cmeta_data_value_* operations
 * required by SaltsUtils NativeSourceIR. Deliberately do not assume the
 * optional CSTL SDK target exists in the installed Salts ServiceHost image.
 */
typedef struct test_owned_bytes {
    unsigned char *data;
    size_t size;
} test_owned_bytes;

static atomic_bool test_owned_fail_copy;

static bool test_owned_is_zero(const void *object) {
    const test_owned_bytes *v = (const test_owned_bytes *)object;
    return v && v->data == NULL && v->size == 0u;
}
static cmeta_status test_owned_init_zero(void *object) {
    if (!object) return CMETA_INVALID_ARGUMENT;
    *(test_owned_bytes *)object = (test_owned_bytes){0};
    return CMETA_OK;
}
static void test_owned_restore_zero(void *object) {
    test_owned_bytes *v = (test_owned_bytes *)object;
    if (!v) return;
    free(v->data);
    *v = (test_owned_bytes){0};
}
static cmeta_status test_owned_assign(
    void *object, const unsigned char *data, size_t size, size_t max_bytes) {
    test_owned_bytes *v = (test_owned_bytes *)object;
    unsigned char *copy;
    if (!v || (size && !data) || !test_owned_is_zero(v))
        return CMETA_INVALID_ARGUMENT;
    if (size > max_bytes) return CMETA_CAPACITY_EXCEEDED;
    if (atomic_load(&test_owned_fail_copy)) return CMETA_OUT_OF_MEMORY;
    if (size == 0u) return CMETA_OK;
    copy = (unsigned char *)malloc(size);
    if (!copy) return CMETA_OUT_OF_MEMORY;
    memcpy(copy, data, size);
    v->data = copy;
    v->size = size;
    return CMETA_OK;
}
static cmeta_status test_owned_read(
    const void *object, const unsigned char **out_data, size_t *out_size) {
    const test_owned_bytes *v = (const test_owned_bytes *)object;
    if (!v || !out_data || !out_size || (v->size && !v->data))
        return CMETA_INVALID_ARGUMENT;
    *out_data = v->data;
    *out_size = v->size;
    return CMETA_OK;
}
static void test_owned_move(void *destination, void *source) {
    test_owned_bytes *dst = (test_owned_bytes *)destination;
    test_owned_bytes *src = (test_owned_bytes *)source;
    CHECK(dst && src && dst != src && test_owned_is_zero(dst));
    *dst = *src;
    *src = (test_owned_bytes){0};
}
static const cmeta_type_identity test_owned_identity =
    CMETA_TYPE_ID_ATOM_INIT("orbit.test.managed.bytes.v1");
static const cmeta_type_desc test_owned_cmeta_type = {
    "test_owned_bytes", sizeof(test_owned_bytes),
    CMETA_ALIGNOF(test_owned_bytes), CMETA_T_OBJECT, NULL, NULL,
    &test_owned_identity
};
static const cmeta_data_buffer_shape test_owned_shape = {
    CMETA_DATA_BUFFER_OWNED
};
static const cmeta_data_buffer_ops test_owned_ops = {
    sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    &test_owned_cmeta_type, CMETA_DATA_BUFFER_OWNED,
    test_owned_is_zero, test_owned_assign, test_owned_restore_zero,
    test_owned_read, test_owned_init_zero, test_owned_move
};
static const cmeta_data_desc test_owned_cmeta_data = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orbit.test.managed.bytes.v1",
    .display_name = "test_owned_bytes",
    .kind = CMETA_DATA_BYTES,
    .storage_type = &test_owned_cmeta_type,
    .shape = &test_owned_shape,
    .buffer_ops = &test_owned_ops
};
static bool test_owned_from(
    test_owned_bytes *out, const unsigned char *data,
    size_t size, size_t max_bytes) {
    return test_owned_assign(out, data, size, max_bytes) == CMETA_OK;
}
static bool test_owned_resize(test_owned_bytes *out, size_t size) {
    unsigned char bytes[64] = {0};
    if (size > sizeof(bytes)) return false;
    return test_owned_from(out, bytes, size, sizeof(bytes));
}
static void test_owned_destroy(test_owned_bytes *out) {
    test_owned_restore_zero(out);
}

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
    atomic_bool hold_owned_business;
    atomic_bool owned_business_entered;
    atomic_bool release_owned_business;
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
    bool has_owned_value;
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


/* Same existing CFlow post-commit callback; a managed record is borrowed
 * through a generation-checked execution pin rather than read from a second
 * queue, global current-room or direct Actor Event pointer.
 */
static bool call_owned_observe(void *user, const cmeta_type_desc *type,
                               const void *value) {
    call_fixture *f = (call_fixture *)user;
    const void *borrowed = NULL;
    uint64_t token;
    orbit_call_status status;
    test_owned_bytes reply = {0};
    const unsigned char expected[] = {0u, 0xFFu, 0x42u, 0x80u};
    size_t checks = 0u;
    CHECK(f != NULL && value != NULL);
    CHECK(cmeta_type_equal(type, &cmeta_type_uint64));
    token = *(const uint64_t *)value;
    status = orbit_call_value_begin(&f->calls, token, 10u, &borrowed);
    if (status == ORBIT_CALL_STALE) {
        atomic_fetch_add(&f->probe.skipped_cancelled, 1);
        atomic_fetch_add(&f->probe.observed, 1);
        return true;
    }
    CHECK(status == ORBIT_CALL_OK && borrowed != NULL);
    atomic_fetch_add(&f->probe.business_executions, 1);
    if (atomic_load(&f->probe.hold_owned_business)) {
        atomic_store(&f->probe.owned_business_entered, true);
        while (!atomic_load(&f->probe.release_owned_business) &&
               checks++ < 10000u)
            cmeta_sleep_ms(1u);
        CHECK(atomic_load(&f->probe.release_owned_business));
    }
    {
        const test_owned_bytes *request = (const test_owned_bytes *)borrowed;
        CHECK(request->size == sizeof(expected));
        CHECK(memcmp(((const test_owned_bytes *)request)->data,
                     expected, sizeof(expected)) == 0);
    }
    CHECK(test_owned_from(
        &reply, expected, sizeof(expected), 32u));
    status = orbit_call_value_stage_reply(
        &f->calls, token, 10u, 0, &reply);
    CHECK(status == ORBIT_CALL_OK || status == ORBIT_CALL_STALE);
    test_owned_destroy(&reply); /* application still owns the reply */
    CHECK(orbit_call_value_end(&f->calls, token, 10u) == ORBIT_CALL_OK);
    status = orbit_call_value_observe(&f->calls, token);
    CHECK(status == ORBIT_CALL_OK || status == ORBIT_CALL_STALE);
    atomic_fetch_add(&f->probe.observed, 1);
    return true;
}

static void call_owned_terminal(
    void *user, uint64_t token, orbit_call_terminal outcome,
    int application_status, const void *response) {
    call_probe *p = (call_probe *)user;
    orbit_call_port_stats stats = {0};
    const unsigned char expected[] = {0u, 0xFFu, 0x42u, 0x80u};
    CHECK(p && token != 0u && application_status == 0);
    CHECK(orbit_call_u64_cancel(p->port, token) == ORBIT_CALL_STALE);
    CHECK(orbit_call_port_get_stats(p->port, &stats));
    CHECK(stats.callbacks_inflight >= 1u);
    atomic_fetch_add(&p->callback_count, 1);
    if (outcome == ORBIT_CALL_TERMINAL_SUCCESS) {
        const test_owned_bytes *bytes = (const test_owned_bytes *)response;
        CHECK(bytes != NULL && bytes->size == sizeof(expected));
        CHECK(memcmp(((const test_owned_bytes *)bytes)->data,
                     expected, sizeof(expected)) == 0);
        atomic_fetch_add(&p->successes, 1);
    } else if (outcome == ORBIT_CALL_TERMINAL_CANCELLED) {
        CHECK(response == NULL);
        CHECK(stats.executing == 1u); /* request still borrowed! */
        atomic_fetch_add(&p->cancelled, 1);
    } else {
        CHECK(0 && "unexpected native VALUE terminal");
    }
}

static void fixture_prepare_ex(call_fixture *fixture, size_t mailbox_capacity,
                               size_t call_capacity, bool managed) {
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
    fixture->has_owned_value = managed;
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
    atomic_init(&fixture->probe.hold_owned_business, false);
    atomic_init(&fixture->probe.owned_business_entered, false);
    atomic_init(&fixture->probe.release_owned_business, false);
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
        managed ? call_owned_observe : call_observe,
        call_on_error, call_on_done, fixture
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
    if (managed) {
        const orbit_call_value_contract contract = {
            10u, &test_owned_cmeta_data,
            &test_owned_cmeta_data, 32u, 32u
        };
        CHECK(orbit_call_port_init_value(
            &fixture->calls, &fixture->service,
            call_capacity, &contract) == ORBIT_CALL_OK);
    } else {
        CHECK(orbit_call_port_init(
            &fixture->calls, &fixture->service,
            call_capacity) == ORBIT_CALL_OK);
    }
    CHECK(orbit_service_ref_acquire(&fixture->service, &fixture->ref));
    {
        const uint64_t forged_token = UINT64_C(1) << 32 | UINT64_C(1);
        CHECK(orbit_service_ref_try_send(&fixture->ref, 10u, &forged_token)
              == CFLOW_ACTOR_SEND_INVALID_ARGUMENT);
    }
}

static void fixture_prepare(call_fixture *fixture, size_t mailbox_capacity,
                            size_t call_capacity) {
    fixture_prepare_ex(fixture, mailbox_capacity, call_capacity, false);
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


/*
 * M0h: the real CFlow Actor remains single-owner, while independent native
 * producer threads contend for the same bounded Orbit CallPort. There is
 * no test-only Actor, response queue or new runtime. Cancellation, close
 * and callbacks may race on distinct threads; every accepted call must
 * receive exactly one terminal, even if close wins during ADMITTING.
 */
enum { CALL_RACE_SENDERS = 8, CALL_RACE_ROUNDS = 8 };

typedef struct call_race_gate {
    atomic_int ready;
    atomic_bool start;
} call_race_gate;

typedef struct call_race_sender {
    call_fixture *fixture;
    call_race_gate *gate;
    orbit_service_ref producer;
    unsigned ordinal;
    atomic_int terminals;
    atomic_int cancelled;
    atomic_int stopped;
    orbit_call_status result;
    cflow_actor_send_status send;
    uint64_t token;
} call_race_sender;

typedef struct call_race_settle {
    orbit_call_port *port;
    call_race_gate *gate;
    uint64_t token;
    bool close_port;
    orbit_call_status result;
} call_race_settle;

static void call_race_gate_init(call_race_gate *gate) {
    atomic_init(&gate->ready, 0);
    atomic_init(&gate->start, false);
}

static void call_race_gate_wait(call_race_gate *gate) {
    size_t checks = 0u;
    atomic_fetch_add(&gate->ready, 1);
    while (!atomic_load(&gate->start) && checks++ < 10000u)
        cmeta_sleep_ms(1u);
    CHECK(atomic_load(&gate->start));
}

static void call_race_begin(call_race_gate *gate, int participants) {
    size_t checks = 0u;
    while (atomic_load(&gate->ready) != participants &&
           checks++ < 10000u)
        cmeta_sleep_ms(1u);
    CHECK(atomic_load(&gate->ready) == participants);
    atomic_store(&gate->start, true);
}

/*
 * A terminal callback can execute on the close/cancel/submitting thread.
 * The slot stays COMPLETING for the full callback, so its own token must
 * already be STALE. Per-attempt atomics forbid duplicated terminal delivery.
 */
static void call_race_terminal(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int application_status, uint64_t response) {
    call_race_sender *sender = (call_race_sender *)user;
    CHECK(sender != NULL && token != 0u);
    CHECK(application_status == 0 && response == 0u);
    CHECK(atomic_fetch_add(&sender->terminals, 1) == 0);
    CHECK(orbit_call_u64_cancel(
        &sender->fixture->calls, token) == ORBIT_CALL_STALE);
    if (terminal == ORBIT_CALL_TERMINAL_CANCELLED)
        atomic_fetch_add(&sender->cancelled, 1);
    else if (terminal == ORBIT_CALL_TERMINAL_STOPPED)
        atomic_fetch_add(&sender->stopped, 1);
    else
        CHECK(0 && "unmatched concurrent local terminal");
}

static void call_race_submit(void *user) {
    call_race_sender *sender = (call_race_sender *)user;
    call_race_gate_wait(sender->gate);
    sender->result = orbit_call_u64_async(
        &sender->fixture->calls, &sender->producer, 10u,
        UINT64_C(100) + (uint64_t)sender->ordinal,
        call_race_terminal, sender, &sender->token, &sender->send);
}

static void call_race_settle_worker(void *user) {
    call_race_settle *settle = (call_race_settle *)user;
    call_race_gate_wait(settle->gate);
    settle->result = settle->close_port
        ? orbit_call_port_close(
              settle->port, ORBIT_CALL_TERMINAL_STOPPED)
        : orbit_call_u64_cancel(settle->port, settle->token);
}

static void call_race_sender_init(call_race_sender *sender,
                                   call_fixture *fixture,
                                   call_race_gate *gate, unsigned ordinal) {
    memset(sender, 0, sizeof(*sender));
    sender->fixture = fixture;
    sender->gate = gate;
    sender->ordinal = ordinal;
    sender->result = ORBIT_CALL_BUSY;
    sender->send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    atomic_init(&sender->terminals, 0);
    atomic_init(&sender->cancelled, 0);
    atomic_init(&sender->stopped, 0);
    /* Each producer owns its own retained ref until its thread exits.
     * Never concurrently mutate or release another thread's ref.
     */
    CHECK(orbit_service_ref_retain(&fixture->ref, &sender->producer));
}

static void call_race_start_senders(
    call_fixture *fixture, call_race_gate *gate,
    call_race_sender senders[CALL_RACE_SENDERS],
    cmeta_thread_t threads[CALL_RACE_SENDERS]) {
    unsigned i;
    for (i = 0u; i < CALL_RACE_SENDERS; ++i) {
        call_race_sender_init(&senders[i], fixture, gate, i);
        CHECK(cmeta_thread_create(
            &threads[i], call_race_submit, &senders[i]) == 0);
    }
}

static void call_race_join_senders(
    call_race_sender senders[CALL_RACE_SENDERS],
    cmeta_thread_t threads[CALL_RACE_SENDERS]) {
    unsigned i;
    for (i = 0u; i < CALL_RACE_SENDERS; ++i) {
        CHECK(cmeta_thread_join(&threads[i]) == 0);
        orbit_service_ref_release(&senders[i].producer);
    }
}

/*
 * Eight real threads contend with no owner progress. Exactly two can hold
 * the two response credits; rejected calls must retain ownership and
 * receive no callback. Two other threads cancel those accepted calls while
 * a third closes the port. Any legal interleaving yields one terminal per
 * accepted token and no callback for a FULL rejection.
 */
static void test_concurrent_producers_and_terminals(void) {
    call_fixture f;
    call_race_gate admit_gate, settle_gate;
    call_race_sender senders[CALL_RACE_SENDERS];
    cmeta_thread_t submitters[CALL_RACE_SENDERS] = {0};
    call_race_settle settles[3] = {0};
    cmeta_thread_t settlers[3] = {0};
    orbit_call_port_stats stats = {0};
    uint64_t tokens[2] = {0};
    unsigned accepted = 0u, full = 0u, i;
    unsigned terminals = 0u, cancelled = 0u, stopped = 0u;

    fixture_prepare(&f, 2u, 2u);
    call_race_gate_init(&admit_gate);
    call_race_start_senders(&f, &admit_gate, senders, submitters);
    call_race_begin(&admit_gate, CALL_RACE_SENDERS);
    call_race_join_senders(senders, submitters);

    for (i = 0u; i < CALL_RACE_SENDERS; ++i) {
        if (senders[i].result == ORBIT_CALL_OK) {
            CHECK(accepted < 2u);
            CHECK(senders[i].token != 0u);
            CHECK(senders[i].send == CFLOW_ACTOR_SEND_ACCEPTED);
            tokens[accepted++] = senders[i].token;
        } else {
            CHECK(senders[i].result == ORBIT_CALL_FULL);
            CHECK(senders[i].token == 0u);
            CHECK(senders[i].send == CFLOW_ACTOR_SEND_INVALID_ARGUMENT);
            ++full;
        }
        CHECK(atomic_load(&senders[i].terminals) == 0);
    }
    CHECK(accepted == 2u && full == CALL_RACE_SENDERS - 2u);
    CHECK(tokens[0] != tokens[1]);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.admitted == 2u && stats.rejected_full == full);
    CHECK(stats.occupied == 2u && stats.admitting == 0u);
    CHECK(orbit_call_port_try_destroy(&f.calls) == ORBIT_CALL_BUSY);

    call_race_gate_init(&settle_gate);
    settles[0] = (call_race_settle){
        &f.calls, &settle_gate, tokens[0], false, ORBIT_CALL_BUSY
    };
    settles[1] = (call_race_settle){
        &f.calls, &settle_gate, tokens[1], false, ORBIT_CALL_BUSY
    };
    settles[2] = (call_race_settle){
        &f.calls, &settle_gate, 0u, true, ORBIT_CALL_BUSY
    };
    for (i = 0u; i < 3u; ++i)
        CHECK(cmeta_thread_create(
            &settlers[i], call_race_settle_worker, &settles[i]) == 0);
    call_race_begin(&settle_gate, 3);
    for (i = 0u; i < 3u; ++i)
        CHECK(cmeta_thread_join(&settlers[i]) == 0);
    CHECK(settles[2].result == ORBIT_CALL_OK);
    CHECK((settles[0].result == ORBIT_CALL_OK ||
           settles[0].result == ORBIT_CALL_STALE));
    CHECK((settles[1].result == ORBIT_CALL_OK ||
           settles[1].result == ORBIT_CALL_STALE));

    for (i = 0u; i < CALL_RACE_SENDERS; ++i) {
        const int callback_count = atomic_load(&senders[i].terminals);
        CHECK(callback_count ==
              (senders[i].result == ORBIT_CALL_OK ? 1 : 0));
        terminals += (unsigned)callback_count;
        cancelled += (unsigned)atomic_load(&senders[i].cancelled);
        stopped += (unsigned)atomic_load(&senders[i].stopped);
    }
    CHECK(terminals == 2u && cancelled + stopped == 2u);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.closed && stats.occupied == 0u);
    CHECK(stats.callbacks_inflight == 0u && stats.admitting == 0u);
    CHECK(stats.admitted == 2u && stats.cancelled == cancelled &&
          stats.stopped == stopped);
    CHECK(orbit_call_u64_cancel(&f.calls, tokens[0]) == ORBIT_CALL_STALE);
    CHECK(orbit_call_u64_cancel(&f.calls, tokens[1]) == ORBIT_CALL_STALE);

    /* Both CFlow token messages were accepted; after close they are stale,
     * cannot run the native business method and cannot reissue callbacks.
     */
    fixture_drive_until(&f, &f.probe.observed, 2);
    CHECK(atomic_load(&f.probe.business_executions) == 0);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.admitted == stats.cancelled + stats.stopped);
    fixture_destroy(&f);
}

/*
 * Repeatedly release the same gate for eight producers and one closer.
 * The port may close before, between or after in-flight admissions.
 * There is no requirement that any particular call wins; the invariant is
 * accepted == callback count, rejected == no callback, no residual credits.
 */
static void test_concurrent_close_during_admission(void) {
    unsigned round;
    for (round = 0u; round < CALL_RACE_ROUNDS; ++round) {
        call_fixture f;
        call_race_gate gate;
        call_race_sender senders[CALL_RACE_SENDERS];
        cmeta_thread_t submitters[CALL_RACE_SENDERS] = {0};
        call_race_settle closer = {0};
        cmeta_thread_t closer_thread = NULL;
        orbit_call_port_stats stats = {0};
        unsigned i, accepted = 0u, full = 0u, closed = 0u;

        fixture_prepare(&f, 2u, 2u);
        call_race_gate_init(&gate);
        call_race_start_senders(&f, &gate, senders, submitters);
        closer = (call_race_settle){
            &f.calls, &gate, 0u, true, ORBIT_CALL_BUSY
        };
        CHECK(cmeta_thread_create(
            &closer_thread, call_race_settle_worker, &closer) == 0);
        call_race_begin(&gate, CALL_RACE_SENDERS + 1);
        call_race_join_senders(senders, submitters);
        CHECK(cmeta_thread_join(&closer_thread) == 0);
        CHECK(closer.result == ORBIT_CALL_OK);

        for (i = 0u; i < CALL_RACE_SENDERS; ++i) {
            const call_race_sender *sender = &senders[i];
            const int callback_count = atomic_load(&sender->terminals);
            if (sender->result == ORBIT_CALL_OK) {
                ++accepted;
                CHECK(sender->send == CFLOW_ACTOR_SEND_ACCEPTED);
                CHECK(sender->token != 0u && callback_count == 1);
                CHECK(atomic_load(&sender->stopped) == 1);
            } else {
                CHECK(sender->result == ORBIT_CALL_FULL ||
                      sender->result == ORBIT_CALL_CLOSED);
                CHECK(sender->token == 0u && callback_count == 0);
                if (sender->result == ORBIT_CALL_FULL) ++full;
                else ++closed;
            }
        }
        CHECK(accepted <= 2u);
        CHECK(accepted + full + closed == CALL_RACE_SENDERS);
        CHECK(orbit_call_port_get_stats(&f.calls, &stats));
        CHECK(stats.closed && stats.occupied == 0u);
        CHECK(stats.callbacks_inflight == 0u && stats.admitting == 0u);
        CHECK(stats.admitted == accepted &&
              stats.stopped == accepted && stats.cancelled == 0u);
        CHECK(stats.rejected_full == full);
        CHECK(orbit_call_port_try_destroy(&f.calls) == ORBIT_CALL_BUSY);

        /* No owner action or post-commit dispatch ran before close. */
        CHECK(atomic_load(&f.probe.business_executions) == 0);
        fixture_destroy(&f);
    }
}


/* Explicit bounded managed VALUE admission reuses the existing Actor and
 * CMeta's owned byte_buffer (including embedded NUL/non-UTF8 octets).
 * The source can be changed and destroyed immediately after successful send.
 */
static void test_owned_value_deepcopy_and_capacity(void) {
    call_fixture f;
    test_owned_bytes source = {0};
    test_owned_bytes oversize = {0};
    const unsigned char raw[] = {0u, 0xFFu, 0x42u, 0x80u};
    orbit_call_port_stats stats = {0};
    uint64_t token = 0u, rejected = UINT64_MAX;
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;

    fixture_prepare_ex(&f, 2u, 1u, true);
    CHECK(test_owned_from(&source, raw, sizeof(raw), 32u));

    /* Force the sole canonical DataDesc copy callback to fail. Admission
     * must not publish a CFlow Event, transfer caller ownership, emit a
     * terminal, or leak an occupied response credit on this failure.
     */
    atomic_store(&test_owned_fail_copy, true);
    CHECK(orbit_call_value_async(
        &f.calls, &f.ref, 10u, &source,
        call_owned_terminal, &f.probe, &rejected, &send) ==
          ORBIT_CALL_VALUE_COPY_FAILED);
    atomic_store(&test_owned_fail_copy, false);
    CHECK(rejected == 0u && source.data != NULL);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.admitted == 0u && stats.occupied == 0u);
    CHECK(stats.callbacks_inflight == 0u &&
          atomic_load(&f.probe.callback_count) == 0);

    CHECK(orbit_call_value_async(
        &f.calls, &f.ref, 10u, &source,
        call_owned_terminal, &f.probe, &token, &send) == ORBIT_CALL_OK);
    CHECK(token != 0u && send == CFLOW_ACTOR_SEND_ACCEPTED);
    CHECK(orbit_call_u64_async(
        &f.calls, &f.ref, 10u, 1u, call_terminal, &f.probe,
        &rejected, &send) == ORBIT_CALL_TYPE_MISMATCH);

    CHECK(orbit_call_value_async(
        &f.calls, &f.ref, 10u, &source,
        call_owned_terminal, &f.probe,
        &rejected, &send) == ORBIT_CALL_FULL);
    CHECK(rejected == 0u);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.occupied == 1u && stats.rejected_full == 1u);
    /* In-place mutation cannot change the Port-owned deep clone. */
    source.data[1] = 0x11u;
    test_owned_destroy(&source);
    fixture_drive_until(&f, &f.probe.successes, 1);
    CHECK(atomic_load(&f.probe.business_executions) == 1);
    CHECK(atomic_load(&f.probe.callback_count) == 1);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.occupied == 0u && stats.executing == 0u);
    CHECK(stats.completed_success == 1u && stats.rejected_full == 1u);
    CHECK(stats.rejected >= 1u); /* forced CMeta copy fault */
    {
        const void *expired_request = NULL;
        CHECK(orbit_call_value_begin(&f.calls, token, 10u,
                                     &expired_request) == ORBIT_CALL_STALE);
        CHECK(expired_request == NULL);
    }

    CHECK(test_owned_resize(&oversize, 33u));
    CHECK(orbit_call_value_async(
        &f.calls, &f.ref, 10u, &oversize,
        call_owned_terminal, &f.probe,
        &rejected, &send) == ORBIT_CALL_VALUE_TOO_LARGE);
    CHECK(rejected == 0u && atomic_load(&f.probe.callback_count) == 1);
    test_owned_destroy(&oversize);
    fixture_destroy(&f);
}

typedef struct owned_cancel_context {
    call_fixture *fixture;
    uint64_t token;
    atomic_int result;
    atomic_int full_result;
} owned_cancel_context;

static void cancel_while_owned_request_borrowed(void *user) {
    owned_cancel_context *ctx = (owned_cancel_context *)user;
    call_fixture *f = ctx->fixture;
    orbit_call_port_stats stats = {0};
    test_owned_bytes input = {0};
    const unsigned char raw[] = {0u, 0xFFu, 0x42u, 0x80u};
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    uint64_t rejected = UINT64_MAX;
    size_t attempts = 0u;
    while (!atomic_load(&f->probe.owned_business_entered) &&
           attempts++ < 10000u)
        cmeta_sleep_ms(1u);
    CHECK(atomic_load(&f->probe.owned_business_entered));
    atomic_store(&ctx->result, (int)orbit_call_u64_cancel(
        &f->calls, ctx->token));
    CHECK(orbit_call_port_get_stats(&f->calls, &stats));
    CHECK(stats.executing == 1u && stats.occupied == 1u);
    CHECK(stats.callbacks_inflight == 0u && stats.cancelled == 1u);
    CHECK(test_owned_from(&input, raw, sizeof(raw), 32u));
    atomic_store(&ctx->full_result, (int)orbit_call_value_async(
        &f->calls, &f->ref, 10u, &input, call_owned_terminal, &f->probe,
        &rejected, &send));
    CHECK(rejected == 0u);
    test_owned_destroy(&input);
    atomic_store(&f->probe.release_owned_business, true);
}

/* The cancellation callback is already over, but the business method
 * continues to read Port-owned bytes. The slot stays RETAINED / FULL and
 * cannot be reused until the original owner releases its execution pin.
 */
static void test_owned_execution_cancel_keeps_request(void) {
    call_fixture f;
    owned_cancel_context ctx = {0};
    cmeta_thread_t thread = NULL;
    test_owned_bytes input = {0};
    const unsigned char raw[] = {0u, 0xFFu, 0x42u, 0x80u};
    uint64_t token = 0u;
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    orbit_call_port_stats stats = {0};

    fixture_prepare_ex(&f, 2u, 1u, true);
    atomic_store(&f.probe.hold_owned_business, true);
    CHECK(test_owned_from(&input, raw, sizeof(raw), 32u));
    CHECK(orbit_call_value_async(
        &f.calls, &f.ref, 10u, &input,
        call_owned_terminal, &f.probe, &token, &send) == ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED);
    test_owned_destroy(&input);
    ctx.fixture = &f;
    ctx.token = token;
    atomic_init(&ctx.result, ORBIT_CALL_BUSY);
    atomic_init(&ctx.full_result, ORBIT_CALL_BUSY);
    CHECK(cmeta_thread_create(
        &thread, cancel_while_owned_request_borrowed, &ctx) == 0);
    fixture_drive_until(&f, &f.probe.observed, 1);
    CHECK(cmeta_thread_join(&thread) == 0);
    CHECK(atomic_load(&ctx.result) == ORBIT_CALL_OK);
    CHECK(atomic_load(&ctx.full_result) == ORBIT_CALL_FULL);
    CHECK(atomic_load(&f.probe.cancelled) == 1);
    CHECK(atomic_load(&f.probe.successes) == 0);
    CHECK(atomic_load(&f.probe.business_executions) == 1);
    CHECK(orbit_call_u64_cancel(&f.calls, token) == ORBIT_CALL_STALE);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.admitted == 1u && stats.cancelled == 1u);
    CHECK(stats.occupied == 0u && stats.executing == 0u);
    CHECK(stats.callbacks_inflight == 0u && stats.rejected_full == 1u);

    /* Reuse is legal only AFTER the owned request execution borrow exits. */
    atomic_store(&f.probe.hold_owned_business, false);
    CHECK(test_owned_from(&input, raw, sizeof(raw), 32u));
    CHECK(orbit_call_value_async(
        &f.calls, &f.ref, 10u, &input,
        call_owned_terminal, &f.probe, &token, &send) == ORBIT_CALL_OK);
    test_owned_destroy(&input);
    fixture_drive_until(&f, &f.probe.successes, 1);
    CHECK(atomic_load(&f.probe.callback_count) == 2);
    CHECK(orbit_call_port_get_stats(&f.calls, &stats));
    CHECK(stats.admitted == 2u && stats.completed_success == 1u);
    CHECK(stats.cancelled == 1u && stats.occupied == 0u);
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
    test_owned_value_deepcopy_and_capacity();
    test_owned_execution_cancel_keeps_request();
    test_concurrent_producers_and_terminals();
    test_concurrent_close_during_admission();
    return 0;
}
