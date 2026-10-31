#include <orbit/scope.h>
#include <orbit/call.h>
#include <orbit/service.h>

#include <salts/service_host.h>
#include <cflow/executor.h>
#include <cflow/machine.h>
#include <cflow/scheduler.h>
#include <cmeta/cmeta.h>
#include <cmeta/component.h>
#include <cmeta/data.h>
#include <cmeta/object.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #x); \
    abort(); \
} } while (0)

/*
 * One genuinely activated Salts Component Provider. This is NOT a fake
 * ServiceHost/Scope or an invented Plugin loader. The Provider is a native
 * static capability; the later Plugin DSO/unload gate remains independent.
 */
#define STATIC_VALUE_METHODS(X,I) X(I,R0,int,get,_)
CMETA_INTERFACE(orbit_test_value, STATIC_VALUE_METHODS);
cmeta_component(OrbitTestValueProvider, cmeta_provides(orbit_test_value));

static int provider_get(void *self) { return *(const int *)self; }
CMETA_IMPLEMENTS(orbit_test_value, orbit_provider_impl, 0u,
    .get = provider_get);
CMETA_OBJECT_INTERFACE_ADAPTER(orbit_test_value);

typedef struct provider_state {
    int value;
    unsigned created;
    unsigned activated;
    unsigned deactivated;
    unsigned destroyed;
    cmeta_object_lifecycle lifetime;
} provider_state;

static void provider_destroy(void *user, void *object) {
    provider_state *p = (provider_state *)user;
    (void)object;
    ++p->destroyed;
}
static cmeta_status provider_project(
    void *context, const cmeta_object_ref *object,
    const cmeta_interface_desc *expected, cmeta_interface_projection *out) {
    (void)context;
    if (object == NULL || out == NULL || expected == NULL)
        return CMETA_INVALID_ARGUMENT;
    if (!cmeta_interface_desc_equal(expected, orbit_test_value_interface()))
        return CMETA_TRAIT_MISSING;
    out->size = sizeof(*out);
    out->interface = orbit_test_value_interface();
    out->self = object->object;
    out->dispatch = &orbit_provider_impl_vtable;
    return CMETA_OK;
}
static const cmeta_object_interface_provider provider_interfaces = {
    sizeof(cmeta_object_interface_provider), NULL, provider_project
};
static cmeta_status provider_create(
    void *user, const cmeta_data_desc *config_data, const void *config_value,
    const salts_configurator_dependency *deps, size_t dep_count,
    cmeta_object_ref *out) {
    provider_state *p = (provider_state *)user;
    cmeta_status result;
    if (p == NULL || out == NULL || dep_count != 0u ||
        deps != NULL || config_data != NULL || config_value != NULL)
        return CMETA_INVALID_ARGUMENT;
    ++p->created;
    result = cmeta_object_borrow(out, &p->value, &cmeta_data_int, NULL);
    if (result != CMETA_OK) return result;
    return cmeta_object_take(out, &p->lifetime);
}
static cmeta_status provider_activate(
    void *user, const cmeta_object_ref *object) {
    provider_state *p = (provider_state *)user;
    if (p == NULL || !cmeta_object_ref_valid(object))
        return CMETA_INVALID_ARGUMENT;
    ++p->activated;
    return CMETA_OK;
}
static void provider_deactivate(
    void *user, const cmeta_object_ref *object) {
    provider_state *p = (provider_state *)user;
    (void)object;
    ++p->deactivated;
}
static void provider_init(provider_state *p, int value) {
    memset(p, 0, sizeof(*p));
    p->value = value;
    p->lifetime.size = sizeof(p->lifetime);
    p->lifetime.context = p;
    p->lifetime.destroy = provider_destroy;
}

typedef struct static_generation_fixture {
    salts_service_host_generation generation;
    salts_configurator_deployment deployments[1];
    salts_configurator_instance instances[1];
    salts_configurator_dependency dependencies[1];
    size_t activation_order[1];
    salts_configurator_provider_binding binding;
    provider_state provider;
} static_generation_fixture;

static void generation_prepare(static_generation_fixture *g,
                               uint64_t generation, int value) {
    salts_service_host_generation_storage storage;
    salts_configurator_deployment deployment;
    memset(g, 0, sizeof(*g));
    provider_init(&g->provider, value);
    g->binding = (salts_configurator_provider_binding){
        sizeof(salts_configurator_provider_binding),
        SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
        cmeta_component_meta(OrbitTestValueProvider),
        &g->provider, &provider_interfaces,
        provider_create, provider_activate, provider_deactivate
    };
    deployment = (salts_configurator_deployment){&g->binding, NULL, NULL};
    storage = (salts_service_host_generation_storage){
        g->deployments, 1u, g->instances, 1u,
        g->dependencies, 1u, g->activation_order, 1u,
        NULL, 0u
    };
    CHECK(salts_service_host_generation_build(
        &g->generation, generation, NULL, &storage,
        &deployment, 1u, NULL, 0u, NULL, 0u) == SALTS_SERVICE_HOST_OK);
    CHECK(g->provider.created == 1u);
    CHECK(g->provider.activated == 1u);
}

typedef struct actor_fixture {
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    orbit_service service;
    orbit_service_ref producer;
    orbit_call_port calls;
    orbit_ace_scope_guard *guard;
    cflow_machine_action_binding action;
    orbit_service_method method;
    int state;
    unsigned terminals;
    unsigned observed;
    unsigned errors;
    uint64_t response;
} actor_fixture;

static bool machine_token(
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

static bool actor_observe(
    void *user, const cmeta_type_desc *type, const void *value) {
    actor_fixture *a = (actor_fixture *)user;
    salts_configurator_service service;
    orbit_test_value provider;
    uint64_t request = 0u;
    uint64_t token;
    int old_value;
    CHECK(a != NULL && value != NULL);
    CHECK(cmeta_type_equal(type, &cmeta_type_uint64));
    token = *(const uint64_t *)value;
    CHECK(orbit_call_u64_request(
        &a->calls, token, UINT64_C(10), &request) == ORBIT_CALL_OK);

    CHECK(orbit_ace_scope_guard_find_service(
        a->guard, orbit_test_value_interface(), &service) ==
        SALTS_SERVICE_HOST_OK);
    provider = orbit_test_value_bind(NULL, NULL);
    CHECK(orbit_test_value_borrow_from_object(
        service.object, service.interfaces, &provider) == CMETA_OK);
    old_value = orbit_test_value_get(&provider);

    CHECK(orbit_call_u64_stage_reply(
        &a->calls, token, UINT64_C(10), 0,
        request + (uint64_t)old_value) == ORBIT_CALL_OK);
    CHECK(orbit_call_u64_observe(&a->calls, token) == ORBIT_CALL_OK);
    ++a->observed;
    return true;
}

static void actor_error(void *user, const char *reason) {
    actor_fixture *a = (actor_fixture *)user;
    (void)reason;
    ++a->errors;
    CHECK(orbit_call_port_close(
        &a->calls, ORBIT_CALL_TERMINAL_ACTOR_FAILED) == ORBIT_CALL_OK);
}

static void actor_done(void *user) {
    actor_fixture *a = (actor_fixture *)user;
    CHECK(orbit_call_port_close(
        &a->calls, ORBIT_CALL_TERMINAL_STOPPED) == ORBIT_CALL_OK);
}

static void call_finished(
    void *user, uint64_t token, orbit_call_terminal status,
    int application_status, uint64_t response) {
    actor_fixture *a = (actor_fixture *)user;
    CHECK(token != 0u);
    CHECK(status == ORBIT_CALL_TERMINAL_SUCCESS);
    CHECK(application_status == 0);
    ++a->terminals;
    a->response = response;
}

static void actor_prepare(actor_fixture *a, orbit_ace_scope_guard *guard,
                          bool bind_scope) {
    const cflow_machine_state states[] = {
        {1u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE}
    };
    const cflow_event_type events[] = {{100u, &cmeta_type_uint64}};
    const cflow_machine_action actions[] = {
        {300u, &cmeta_type_int, 100u, &cmeta_type_uint64,
         &cmeta_type_int, CMETA_EFFECT_STATEFUL,
         CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
         CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_uint64, 0u}
    };
    const cflow_machine_transition transitions[] = {
        {1u, 100u, 0u, 300u, 1u, 1u}
    };
    const cflow_machine_definition graph = {
        states, 1u, 1u, events, 1u, NULL, 0u,
        actions, 1u, transitions, 1u
    };
    cflow_actor_config config = {0};
    orbit_service_config service_config;
    memset(a, 0, sizeof(*a));
    a->guard = guard;
    CHECK(cflow_machine_build(&a->machine, &graph) == CFLOW_MACHINE_OK);
    CHECK(cflow_executor_owner_init_with_capacity(
        &a->executor, 64u, NULL, NULL));
    CHECK(cflow_scheduler_owner_bind(
        &a->scheduler, &a->executor, 32u));
    a->action = (cflow_machine_action_binding){
        300u, machine_token, NULL
    };
    config.machine = (cflow_machine_instance_config){
        &a->machine, &a->state, &cmeta_type_uint64,
        NULL, 0u, &a->action, 1u, 4u, &a->executor
    };
    config.scheduler = &a->scheduler;
    config.callbacks = (cflow_subscriber_callbacks){
        actor_observe, actor_error, actor_done, a
    };
    a->method = (orbit_service_method){
        UINT64_C(10), 100u, &cmeta_type_uint64, ORBIT_METHOD_CALL_TOKEN
    };
    service_config = (orbit_service_config){&config, &a->method, 1u};
    CHECK(orbit_service_spawn(&a->service, &service_config).status ==
          ORBIT_SERVICE_OK);
    CHECK(orbit_call_port_init(&a->calls, &a->service, 2u) ==
          ORBIT_CALL_OK);
    CHECK(orbit_service_ref_acquire(&a->service, &a->producer));
    if (bind_scope) {
        CHECK(orbit_ace_scope_guard_bind(
            guard, &a->service, &a->calls) == ORBIT_ACE_SCOPE_OK);
        CHECK(orbit_ace_scope_guard_bind(
            guard, &a->service, &a->calls) == ORBIT_ACE_SCOPE_ALREADY_BOUND);
    }
}

static void actor_close(actor_fixture *a) {
    unsigned steps = 0u;
    cflow_actor_status result;
    result = orbit_service_request_stop(&a->service);
    CHECK(result == CFLOW_ACTOR_OK || result == CFLOW_ACTOR_STOPPED);
    while (cflow_actor_current_state(&a->service.actor) !=
           CFLOW_ACTOR_STATE_STOPPED && steps++ < 2048u)
        (void)cflow_executor_run_one(&a->executor);
    CHECK(cflow_actor_current_state(&a->service.actor) ==
          CFLOW_ACTOR_STATE_STOPPED);
    CHECK(orbit_service_wait(&a->service) == CFLOW_ACTOR_STATE_STOPPED);
    orbit_service_ref_release(&a->producer);
    orbit_service_destroy(&a->service);
    cflow_scheduler_destroy(&a->scheduler);
    cflow_executor_destroy(&a->executor);
    cflow_machine_destroy(&a->machine);
}

static void test_provider_actor_scope_pin(void) {
    static_generation_fixture n = {0};
    static_generation_fixture next = {0};
    salts_service_host_runtime host = SALTS_SERVICE_HOST_RUNTIME_INIT;
    salts_service_host_generation *retired = NULL;
    salts_service_host_status native = SALTS_SERVICE_HOST_OK;
    orbit_ace_scope_guard guard = {0};
    actor_fixture actor;
    uint64_t token = 0u;
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    unsigned steps = 0u;

    generation_prepare(&n, UINT64_C(11), 7);
    generation_prepare(&next, UINT64_C(12), 101);
    CHECK(salts_service_host_runtime_init(&host) == SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_publish(
        &host, &n.generation, &retired) == SALTS_SERVICE_HOST_OK);
    CHECK(retired == NULL);

    CHECK(orbit_ace_scope_guard_open(
        &guard, &host, UINT64_C(11), &native) == ORBIT_ACE_SCOPE_OK);
    actor_prepare(&actor, &guard, true);
    CHECK(orbit_ace_scope_guard_try_release(&guard, &native) ==
          ORBIT_ACE_SCOPE_BUSY);

    CHECK(salts_service_host_runtime_publish(
        &host, &next.generation, &retired) == SALTS_SERVICE_HOST_OK);
    CHECK(retired == &n.generation);
    CHECK(salts_service_host_generation_drain(
        &host, &n.generation) == SALTS_SERVICE_HOST_BUSY);

    CHECK(orbit_call_u64_async(
        &actor.calls, &actor.producer, UINT64_C(10), UINT64_C(5),
        call_finished, &actor, &token, &send) == ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED);
    while (actor.terminals == 0u && steps++ < 2048u)
        (void)cflow_executor_run_one(&actor.executor);
    CHECK(actor.terminals == 1u);
    CHECK(actor.observed == 1u);
    CHECK(actor.errors == 0u);
    CHECK(actor.response == UINT64_C(12)); /* N (7), not new N+1 (101) */
    CHECK(orbit_ace_scope_guard_generation_id(&guard) == UINT64_C(11));

    actor_close(&actor);
    CHECK(orbit_ace_scope_guard_try_release(&guard, &native) ==
          ORBIT_ACE_SCOPE_BUSY); /* CallPort still owns terminal/control. */
    CHECK(orbit_call_port_try_destroy(&actor.calls) == ORBIT_CALL_OK);
    CHECK(orbit_ace_scope_guard_try_release(&guard, &native) ==
          ORBIT_ACE_SCOPE_OK);
    CHECK(native == SALTS_SERVICE_HOST_OK);
    CHECK(orbit_ace_scope_guard_generation_id(&guard) == 0u);
    {
        salts_configurator_service expired;
        CHECK(orbit_ace_scope_guard_find_service(
            &guard, orbit_test_value_interface(), &expired) ==
            SALTS_SERVICE_HOST_INVALID_ARGUMENT);
    }
    CHECK(salts_service_host_generation_drain(
        &host, &n.generation) == SALTS_SERVICE_HOST_OK);
    CHECK(n.provider.deactivated == 1u && n.provider.destroyed == 1u);

    CHECK(salts_service_host_runtime_close(
        &host, &retired) == SALTS_SERVICE_HOST_OK);
    CHECK(retired == &next.generation);
    CHECK(salts_service_host_generation_drain(
        &host, &next.generation) == SALTS_SERVICE_HOST_OK);
    CHECK(next.provider.deactivated == 1u &&
          next.provider.destroyed == 1u);
    CHECK(salts_service_host_runtime_destroy(&host) ==
          SALTS_SERVICE_HOST_OK);
}


/*
 * M0k.2h: Scope binding is an activation-only operation. Two original
 * ServiceHost generations own two actual independent Actor/CallPort pairs.
 * A foreign Port must be rejected, as must the correct Port once CLOSED or
 * after even one CFlow admission (including when the call has completed).
 * The intentionally unbound test Actors keep their original scopes alive
 * manually until every Actor and Port is destroyed.
 */
static void test_scope_binding_requires_unused_live_port(void) {
    static_generation_fixture n = {0};
    static_generation_fixture next = {0};
    salts_service_host_runtime host = SALTS_SERVICE_HOST_RUNTIME_INIT;
    salts_service_host_generation *retired = NULL;
    salts_service_host_status exact = SALTS_SERVICE_HOST_OK;
    orbit_ace_scope_guard old_scope = {0}, next_scope = {0};
    actor_fixture closed_actor, late_actor;
    uint64_t token = 0u;
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    unsigned rounds = 0u;

    generation_prepare(&n, UINT64_C(21), 7);
    generation_prepare(&next, UINT64_C(22), 101);
    CHECK(salts_service_host_runtime_init(&host) == SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_publish(
        &host, &n.generation, &retired) == SALTS_SERVICE_HOST_OK);
    CHECK(retired == NULL);
    CHECK(orbit_ace_scope_guard_open(
        &old_scope, &host, UINT64_C(21), &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(salts_service_host_runtime_publish(
        &host, &next.generation, &retired) == SALTS_SERVICE_HOST_OK);
    CHECK(retired == &n.generation);
    CHECK(orbit_ace_scope_guard_open(
        &next_scope, &host, UINT64_C(22), &exact) == ORBIT_ACE_SCOPE_OK);

    actor_prepare(&closed_actor, &old_scope, false);
    actor_prepare(&late_actor, &next_scope, false);

    /* Both ports are live, but neither belongs to the foreign Actor. */
    CHECK(!orbit_call_port_is_bound_to(
        &closed_actor.calls, &late_actor.service));
    CHECK(!orbit_call_port_is_bound_to(
        &late_actor.calls, &closed_actor.service));
    CHECK(orbit_ace_scope_guard_bind(
        &old_scope, &closed_actor.service, &late_actor.calls) ==
          ORBIT_ACE_SCOPE_INVALID_ARGUMENT);
    CHECK(orbit_ace_scope_guard_bind(
        &next_scope, &late_actor.service, &closed_actor.calls) ==
          ORBIT_ACE_SCOPE_INVALID_ARGUMENT);

    /* No calls have been admitted; a CLOSED Port still cannot bind. */
    CHECK(orbit_call_port_close(
        &closed_actor.calls, ORBIT_CALL_TERMINAL_STOPPED) == ORBIT_CALL_OK);
    CHECK(orbit_ace_scope_guard_bind(
        &old_scope, &closed_actor.service, &closed_actor.calls) ==
          ORBIT_ACE_SCOPE_INVALID_ARGUMENT);

    /* An accepted call must not be retroactively attached to a Scope. */
    CHECK(orbit_call_u64_async(
        &late_actor.calls, &late_actor.producer, UINT64_C(10), UINT64_C(5),
        call_finished, &late_actor, &token, &send) == ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED && token != 0u);
    CHECK(orbit_ace_scope_guard_bind(
        &next_scope, &late_actor.service, &late_actor.calls) ==
          ORBIT_ACE_SCOPE_INVALID_ARGUMENT);
    while (late_actor.terminals == 0u && rounds++ < 2048u)
        (void)cflow_executor_run_one(&late_actor.executor);
    CHECK(late_actor.terminals == 1u && late_actor.observed == 1u);
    CHECK(late_actor.response == UINT64_C(106));
    CHECK(orbit_ace_scope_guard_bind(
        &next_scope, &late_actor.service, &late_actor.calls) ==
          ORBIT_ACE_SCOPE_INVALID_ARGUMENT);

    /* Test-only incorrect activation; preserve each borrowed Scope manually
     * until after the native Actor and CallPort have fully retired.
     */
    actor_close(&closed_actor);
    actor_close(&late_actor);
    CHECK(orbit_call_port_try_destroy(&closed_actor.calls) == ORBIT_CALL_OK);
    CHECK(orbit_call_port_try_destroy(&late_actor.calls) == ORBIT_CALL_OK);
    CHECK(orbit_ace_scope_guard_try_release(
        &old_scope, &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(orbit_ace_scope_guard_try_release(
        &next_scope, &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(salts_service_host_generation_drain(
        &host, &n.generation) == SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_close(
        &host, &retired) == SALTS_SERVICE_HOST_OK);
    CHECK(retired == &next.generation);
    CHECK(salts_service_host_generation_drain(
        &host, &next.generation) == SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_destroy(&host) == SALTS_SERVICE_HOST_OK);
    CHECK(n.provider.destroyed == 1u && next.provider.destroyed == 1u);
}

int main(void) {
    test_provider_actor_scope_pin();
    test_scope_binding_requires_unused_live_port();
    return 0;
}
