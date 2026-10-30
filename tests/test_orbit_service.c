#include <orbit/service.h>

#include <cflow/executor.h>
#include <cflow/machine.h>
#include <cflow/scheduler.h>
#include <cmeta/cmeta.h>

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* This is a genuine CFlow Machine/Actor test, NOT a mocked actor or poller. */
typedef struct service_probe {
    atomic_int executed;
    atomic_int observed;
    atomic_int done;
    atomic_int errors;
    atomic_int last;
} service_probe;

typedef struct service_fixture {
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    orbit_service service;
    orbit_service_method methods[1];
    cflow_machine_action_binding action_binding;
    cflow_actor_config actor_config;
    orbit_service_config service_config;
    service_probe probe;
    int initial_state;
} service_fixture;

static bool service_action(
    void *user, const void *state, const void *event,
    void *out_target_state, void *out_observation, const char **out_error) {
    service_probe *probe = (service_probe *)user;
    if (probe == NULL || state == NULL || event == NULL ||
        out_target_state == NULL || out_observation == NULL || out_error == NULL)
        return false;
    *(int *)out_target_state = *(const int *)state + 1;
    *(int *)out_observation = *(const int *)event;
    *out_error = NULL;
    atomic_fetch_add(&probe->executed, 1);
    return true;
}

static bool service_observe(void *user,
                            const cmeta_type_desc *type,
                            const void *value) {
    service_probe *probe = (service_probe *)user;
    if (probe == NULL || !cmeta_type_equal(type, &cmeta_type_int) ||
        value == NULL)
        return false;
    atomic_store(&probe->last, *(const int *)value);
    atomic_fetch_add(&probe->observed, 1);
    return true;
}

static void service_on_error(void *user, const char *message) {
    service_probe *probe = (service_probe *)user;
    (void)message;
    atomic_fetch_add(&probe->errors, 1);
}

static void service_on_done(void *user) {
    service_probe *probe = (service_probe *)user;
    atomic_fetch_add(&probe->done, 1);
}

static void fixture_prepare(service_fixture *fixture) {
    const cflow_machine_state states[] = {
        {10u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE}
    };
    const cflow_event_type events[] = {
        {100u, &cmeta_type_int}
    };
    const cflow_machine_action actions[] = {
        {300u, &cmeta_type_int, 100u, &cmeta_type_int, &cmeta_type_int,
         CMETA_EFFECT_MAY_FAIL, CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
         CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_int, 0u}
    };
    const cflow_machine_transition transitions[] = {
        {10u, 100u, 0u, 300u, 10u, 1u}
    };
    const cflow_machine_definition definition = {
        states, 1u, 10u, events, 1u, NULL, 0u,
        actions, 1u, transitions, 1u
    };
    memset(fixture, 0, sizeof(*fixture));
    atomic_init(&fixture->probe.executed, 0);
    atomic_init(&fixture->probe.observed, 0);
    atomic_init(&fixture->probe.done, 0);
    atomic_init(&fixture->probe.errors, 0);
    atomic_init(&fixture->probe.last, -1);

    assert(cflow_machine_build(&fixture->machine, &definition) == CFLOW_MACHINE_OK);
    /* Same single CFlow owner for Machine work and Subscription work. */
    assert(cflow_executor_owner_init_with_capacity(
        &fixture->executor, 64u, NULL, NULL));
    assert(cflow_scheduler_owner_bind(&fixture->scheduler,
                                      &fixture->executor, 32u));

    fixture->action_binding = (cflow_machine_action_binding){
        300u, service_action, &fixture->probe
    };
    fixture->actor_config.machine = (cflow_machine_instance_config){
        &fixture->machine, &fixture->initial_state, &cmeta_type_int,
        NULL, 0u, &fixture->action_binding, 1u, 1u, &fixture->executor
    };
    fixture->actor_config.scheduler = &fixture->scheduler;
    fixture->actor_config.callbacks = (cflow_subscriber_callbacks){
        service_observe, service_on_error, service_on_done, &fixture->probe
    };
    fixture->methods[0] = (orbit_service_method){
        10u, 100u, &cmeta_type_int
    };
    fixture->service_config = (orbit_service_config){
        &fixture->actor_config, fixture->methods, 1u
    };
}

static void fixture_destroy(service_fixture *fixture) {
    orbit_service_destroy(&fixture->service);
    cflow_scheduler_destroy(&fixture->scheduler);
    cflow_executor_destroy(&fixture->executor);
    cflow_machine_destroy(&fixture->machine);
}

int main(void) {
    service_fixture fixture;
    orbit_service_ref ref = {0};
    orbit_service_ref retained = {0};
    orbit_service_spawn_result spawn_result;
    cflow_actor_stats stats = {0};
    size_t attempts = 0u;
    int input = 42;

    fixture_prepare(&fixture);

    /* Fail before touching runtime: no guessed type / method ABI. */
    {
        orbit_service_config bad = fixture.service_config;
        orbit_service_method wrong = {10u, 100u, &cmeta_type_bool};
        bad.methods = &wrong;
        spawn_result = orbit_service_spawn(&fixture.service, &bad);
        assert(spawn_result.status == ORBIT_SERVICE_INVALID_METHOD);
        assert(fixture.service.actor.impl == NULL);
    }
    {
        orbit_service_config bad = fixture.service_config;
        bad.method_count = 0u;
        spawn_result = orbit_service_spawn(&fixture.service, &bad);
        assert(spawn_result.status == ORBIT_SERVICE_INVALID_METHOD);
    }

    spawn_result = orbit_service_spawn(&fixture.service,
                                      &fixture.service_config);
    assert(spawn_result.status == ORBIT_SERVICE_OK);
    assert(spawn_result.actor_status == CFLOW_ACTOR_OK);
    assert(orbit_service_ref_acquire(&fixture.service, &ref));
    assert(orbit_service_ref_retain(&ref, &retained));

    assert(orbit_service_ref_try_send(&ref, 0u, &input) ==
           CFLOW_ACTOR_SEND_INVALID_ARGUMENT);
    assert(orbit_service_ref_try_send(&ref, 999u, &input) ==
           CFLOW_ACTOR_SEND_INVALID_ARGUMENT);
    assert(orbit_service_ref_try_send(&ref, 10u, NULL) ==
           CFLOW_ACTOR_SEND_INVALID_ARGUMENT);

    /* Mailbox capacity is one. Nothing is driven yet, so FULL is exact. */
    assert(orbit_service_ref_try_send(&ref, 10u, &input) ==
           CFLOW_ACTOR_SEND_ACCEPTED);
    assert(orbit_service_ref_try_send(&ref, 10u, &input) ==
           CFLOW_ACTOR_SEND_FULL);
    assert(input == 42);

    while (atomic_load(&fixture.probe.observed) == 0 && attempts++ < 512u)
        (void)cflow_executor_run_one(&fixture.executor);
    assert(atomic_load(&fixture.probe.observed) == 1);
    assert(atomic_load(&fixture.probe.executed) == 1);
    assert(atomic_load(&fixture.probe.last) == 42);
    assert(atomic_load(&fixture.probe.errors) == 0);
    assert(orbit_service_get_stats(&fixture.service, &stats));
    assert(stats.machine.accepted == 1u);
    assert(stats.machine.completed == 1u);

    assert(orbit_service_request_stop(&fixture.service) == CFLOW_ACTOR_OK);
    attempts = 0u;
    while (cflow_actor_current_state(&fixture.service.actor) !=
                CFLOW_ACTOR_STATE_STOPPED && attempts++ < 512u)
        (void)cflow_executor_run_one(&fixture.executor);
    assert(orbit_service_wait(&fixture.service) == CFLOW_ACTOR_STATE_STOPPED);
    assert(atomic_load(&fixture.probe.done) == 1);

    assert(orbit_service_ref_try_send(&ref, 10u, &input) ==
           CFLOW_ACTOR_SEND_STOPPED);

    /* Existing CFlow producer refs are safe after Actor owner destruction. */
    orbit_service_destroy(&fixture.service);
    assert(orbit_service_ref_try_send(&retained, 10u, &input) ==
           CFLOW_ACTOR_SEND_STALE);
    orbit_service_ref_release(&retained);
    orbit_service_ref_release(&ref);
    assert(retained.actor.impl == NULL && retained.methods == NULL);
    assert(ref.actor.impl == NULL && ref.methods == NULL);

    fixture_destroy(&fixture);
    return 0;
}
