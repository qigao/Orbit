#include <orbit/scope.h>
#include "fixtures/orbit_dso_contract.h"

#include <salts/service_host.h>
#include <salts/plugin.h>
#include <orbit/call.h>
#include <orbit/service.h>
#include <cflow/machine.h>
#include <cflow/executor.h>
#include <cflow/scheduler.h>
#include <cmeta/cmeta.h>
#include <salts/thread.h>

#define DATABIND_NATIVE_ENABLE_ORBIT 1
#include "game.service_native.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ORBIT_DSO_PROVIDER_N_PATH) || \
    !defined(ORBIT_DSO_PROVIDER_NEXT_PATH)
#error actual DSO target paths must be provided by the original CMake graph
#endif

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

typedef struct dso_generation {
    salts_service_host_generation generation;
    salts_configurator_deployment deployments[1];
    salts_configurator_instance instances[1];
    salts_component_dependency dependencies[1];
    size_t activation_order[1];
    salts_service_host_module modules[1];
} dso_generation;

static void build(
    dso_generation *g, uint64_t generation_id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref plugin) {
    const salts_service_host_generation_storage storage = {
        g->deployments, 1u, g->instances, 1u,
        g->dependencies, 1u, g->activation_order, 1u,
        g->modules, 1u
    };
    const salts_service_host_source source = {
        plugin, ORBIT_DSO_PROVIDER_EXPORT_ID, NULL, NULL
    };
    CHECK(salts_service_host_generation_build(
        &g->generation, generation_id, registry, &storage,
        NULL, 0u, &source, 1u, NULL, 0u) ==
        SALTS_SERVICE_HOST_OK);
}

static int actual_provider_value(orbit_ace_scope_guard *scope) {
    salts_configurator_service service;
    orbit_dso_value value = orbit_dso_value_bind(NULL, NULL);
    CHECK(orbit_ace_scope_guard_find_service(
        scope, orbit_dso_value_interface(), &service) ==
        SALTS_SERVICE_HOST_OK);
    CHECK(orbit_dso_value_borrow_from_object(
        service.object, service.interfaces, &value) == CMETA_OK);
    return orbit_dso_value_get(&value);
}

/*
 * The SAME NativeSourceIR business symbols emitted for Game.Room are used
 * here, but their instance receiver is the Salts Plugin Provider projection
 * pinned by the original ServiceHost Scope. The actual getter executes
 * inside the old DSO. There is no alternative Orbit handler ABI.
 */
typedef struct dso_room_receiver {
    orbit_dso_value projected;
    unsigned invocations;
} dso_room_receiver;

int databind_4_Game_4_Room_4_Ping(
    void *receiver, const PingRequest *request, PingResponse *response) {
    dso_room_receiver *room = (dso_room_receiver *)receiver;
    int value;
    if (!room || !request || !response) return 17;
    value = orbit_dso_value_get(&room->projected);
    if (value < 0) return 18;
    ++room->invocations;
    response->ack = request->seq + (uint64_t)value;
    return 0;
}

int databind_4_Game_4_Room_4_Pong(
    void *receiver, const PingRequest *request, PingResponse *response) {
    dso_room_receiver *room = (dso_room_receiver *)receiver;
    int value;
    if (!room || !request || !response) return 19;
    value = orbit_dso_value_get(&room->projected);
    if (value < 0) return 20;
    ++room->invocations;
    response->ack = request->seq + (uint64_t)value + UINT64_C(100);
    return 0;
}

/*
 * Genuine DSO-owned provider -> post-commit CFlow Actor -> bounded Orbit
 * CallPort: no private loader/scheduler/callback queue. A generation N
 * guard is kept live through Actor AND CallPort terminal after N+1 publishes.
 */
typedef struct dso_actor {
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    orbit_service service;
    orbit_service_ref producer;
    orbit_call_port port;
    cflow_machine_action_binding action[2];
    dso_room_receiver receiver;
    databind_4_Game_4_Room__orbit_binding binding;
    databind_4_Game_4_Room_4_Ping__orbit_context ping_context;
    databind_4_Game_4_Room_4_Pong__orbit_context pong_context;
    databind_4_Game_4_Room_4_Ping__orbit_context cancel_context;
    uint64_t first_result;
    unsigned cancelled;
    atomic_bool hold_terminal;
    atomic_bool terminal_entered;
    atomic_bool release_terminal;
    int state;
    unsigned observed;
    unsigned callbacks;
    unsigned errors;
    uint64_t last_result;
} dso_actor;

static bool dso_machine_action(
    void *user, const void *state, const void *event,
    void *out_state, void *observation, const char **error) {
    (void)user;
    if (state == NULL || event == NULL || out_state == NULL ||
        observation == NULL || error == NULL)
        return false;
    *(int *)out_state = *(const int *)state + 1;
    *(uint64_t *)observation = *(const uint64_t *)event;
    *error = NULL;
    return true;
}

static bool dso_actor_observe(
    void *user, const cmeta_type_desc *type, const void *value) {
    dso_actor *actor = (dso_actor *)user;
    uint64_t token;
    CHECK(actor != NULL && value != NULL);
    CHECK(cmeta_type_equal(type, &cmeta_type_uint64));
    token = *(const uint64_t *)value;
    /*
     * Real generated, contextual post-commit dispatch calls the original
     * native symbol with the scope-pinned DSO Provider receiver.
     */
    {
        const orbit_call_status step =
            databind_4_Game_4_Room__orbit_dispatch(&actor->binding, token);
        /* A previously cancelled local call is a stale generation token. */
        CHECK(step == ORBIT_CALL_OK || step == ORBIT_CALL_STALE);
    }
    ++actor->observed;
    return true;
}

static void dso_actor_on_error(void *user, const char *error) {
    dso_actor *actor = (dso_actor *)user;
    (void)error;
    ++actor->errors;
    CHECK(orbit_call_port_close(
        &actor->port, ORBIT_CALL_TERMINAL_ACTOR_FAILED) == ORBIT_CALL_OK);
}

static void dso_actor_on_done(void *user) {
    dso_actor *actor = (dso_actor *)user;
    CHECK(orbit_call_port_close(
        &actor->port, ORBIT_CALL_TERMINAL_STOPPED) == ORBIT_CALL_OK);
}

static void dso_generated_ping_done(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int application_status, const PingResponse *response) {
    dso_actor *actor = (dso_actor *)user;
    CHECK(actor != NULL && token != 0u && response != NULL);
    CHECK(terminal == ORBIT_CALL_TERMINAL_SUCCESS);
    CHECK(application_status == 0);
    actor->first_result = response->ack;
    ++actor->callbacks;
    if (atomic_load(&actor->hold_terminal)) {
        size_t checks = 0u;
        atomic_store(&actor->terminal_entered, true);
        while (!atomic_load(&actor->release_terminal) &&
               checks++ < 10000u)
            cmeta_sleep_ms(1u);
        CHECK(atomic_load(&actor->release_terminal));
    }
}

static void dso_generated_pong_done(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int application_status, const PingResponse *response) {
    dso_actor *actor = (dso_actor *)user;
    CHECK(actor != NULL && token != 0u && response != NULL);
    CHECK(terminal == ORBIT_CALL_TERMINAL_SUCCESS);
    CHECK(application_status == 0);
    actor->last_result = response->ack;
    ++actor->callbacks;
}

static void dso_generated_cancel_done(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int application_status, const PingResponse *response) {
    dso_actor *actor = (dso_actor *)user;
    CHECK(actor != NULL && token != 0u);
    CHECK(terminal == ORBIT_CALL_TERMINAL_CANCELLED);
    CHECK(application_status == 0 && response == NULL);
    ++actor->cancelled;
}

static void dso_actor_start(dso_actor *actor, orbit_ace_scope_guard *scope) {
    const cflow_machine_state states[] = {
        {1u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE}
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
        {1u, 100u, 0u, 300u, 1u, 1u},
        {1u, 101u, 0u, 301u, 1u, 1u}
    };
    const size_t event_count = 2u;
    const cflow_machine_definition definition = {
        states, 1u, 1u, events, event_count, NULL, 0u,
        actions, event_count, transitions, event_count
    };
    cflow_actor_config config = {0};
    orbit_service_config service_config;

    memset(actor, 0, sizeof(*actor));
    atomic_init(&actor->hold_terminal, false);
    atomic_init(&actor->terminal_entered, false);
    atomic_init(&actor->release_terminal, false);
    CHECK(cflow_machine_build(&actor->machine, &definition) == CFLOW_MACHINE_OK);
    CHECK(cflow_executor_owner_init_with_capacity(
        &actor->executor, 64u, NULL, NULL));
    CHECK(cflow_scheduler_owner_bind(
        &actor->scheduler, &actor->executor, 32u));
    actor->action[0] = (cflow_machine_action_binding){
        300u, dso_machine_action, NULL
    };
    actor->action[1] = (cflow_machine_action_binding){
        301u, dso_machine_action, NULL
    };
    config.machine = (cflow_machine_instance_config){
        &actor->machine, &actor->state, &cmeta_type_uint64,
        NULL, 0u, actor->action, event_count, 2u, &actor->executor
    };
    config.scheduler = &actor->scheduler;
    config.callbacks = (cflow_subscriber_callbacks){
        dso_actor_observe, dso_actor_on_error, dso_actor_on_done, actor
    };
    /* Use exactly the one ServiceHost Scope acquired before Actor start. */
    service_config = databind_4_Game_4_Room__orbit_service_config(&config);
    CHECK(orbit_service_spawn(&actor->service, &service_config).status ==
          ORBIT_SERVICE_OK);
    CHECK(orbit_call_port_init(&actor->port, &actor->service, 2u) ==
          ORBIT_CALL_OK);
    CHECK(orbit_service_ref_acquire(
        &actor->service, &actor->producer));

    {
        salts_configurator_service service;
        orbit_dso_value view = orbit_dso_value_bind(NULL, NULL);
        CHECK(orbit_ace_scope_guard_find_service(
            scope, orbit_dso_value_interface(), &service) ==
            SALTS_SERVICE_HOST_OK);
        CHECK(orbit_dso_value_borrow_from_object(
            service.object, service.interfaces, &view) == CMETA_OK);
        actor->receiver.projected = view;
        CHECK(databind_4_Game_4_Room__orbit_bind(
            &actor->binding, &actor->port, &actor->receiver));
        CHECK(actor->binding.receiver == &actor->receiver);
    }
    CHECK(orbit_ace_scope_guard_bind(
        scope, &actor->service, &actor->port) == ORBIT_ACE_SCOPE_OK);
}

static void dso_actor_stop(dso_actor *actor) {
    unsigned rounds = 0u;
    const cflow_actor_status status =
        orbit_service_request_stop(&actor->service);
    CHECK(status == CFLOW_ACTOR_OK || status == CFLOW_ACTOR_STOPPED);
    while (cflow_actor_current_state(&actor->service.actor) !=
           CFLOW_ACTOR_STATE_STOPPED && rounds++ < 2048u)
        (void)cflow_executor_run_one(&actor->executor);
    CHECK(cflow_actor_current_state(&actor->service.actor) ==
          CFLOW_ACTOR_STATE_STOPPED);
    CHECK(orbit_service_wait(&actor->service) ==
          CFLOW_ACTOR_STATE_STOPPED);
    orbit_service_ref_release(&actor->producer);
    orbit_service_destroy(&actor->service);
    cflow_scheduler_destroy(&actor->scheduler);
    cflow_executor_destroy(&actor->executor);
    cflow_machine_destroy(&actor->machine);
}


/*
 * An independent real native thread checks original ServiceHost Scope
 * ownership while the generated N callback still holds its CallPort credit.
 * Actor/port/guard destruction is serialized after this worker joins.
 */
typedef struct dso_live_terminal_probe {
    dso_actor *actor;
    orbit_ace_scope_guard *scope;
    salts_service_host_runtime *runtime;
    salts_service_host_generation *generation;
    cmeta_plugin_registry *registry;
    cmeta_plugin_ref plugin;
    atomic_int proved_busy;
} dso_live_terminal_probe;

static void dso_check_live_terminal_scope(void *user) {
    dso_live_terminal_probe *probe = (dso_live_terminal_probe *)user;
    orbit_call_port_stats stats = {0};
    salts_service_host_status exact = SALTS_SERVICE_HOST_OK;
    size_t checks = 0u;

    while (!atomic_load(&probe->actor->terminal_entered) &&
           checks++ < 10000u)
        cmeta_sleep_ms(1u);
    CHECK(atomic_load(&probe->actor->terminal_entered));
    CHECK(orbit_call_port_get_stats(&probe->actor->port, &stats));
    CHECK(stats.admitted == 2u && stats.callbacks_inflight == 1u);
    CHECK(stats.occupied == 2u && !stats.closed);
    CHECK(orbit_ace_scope_guard_try_release(probe->scope, &exact) ==
          ORBIT_ACE_SCOPE_BUSY);
    CHECK(salts_service_host_generation_drain(
        probe->runtime, probe->generation) == SALTS_SERVICE_HOST_BUSY);
    CHECK(cmeta_plugin_registry_unload(
        probe->registry, probe->plugin) == CMETA_PLUGIN_BUSY);
    CHECK(orbit_ace_scope_guard_generation_id(probe->scope) ==
          UINT64_C(21));
    atomic_store(&probe->proved_busy, 1);
    atomic_store(&probe->actor->release_terminal, true);
}

int main(void) {
    const cmeta_plugin_registry_config registry_config = {2u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref original = {0};
    cmeta_plugin_ref successor = {0};
    cmeta_plugin_lifecycle_info info;
    dso_generation n = {0};
    dso_generation next = {0};
    salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
    salts_service_host_generation *previous = NULL;
    orbit_ace_scope_guard old_scope = {0};
    orbit_ace_scope_guard next_scope = {0};
    salts_service_host_status exact = SALTS_SERVICE_HOST_OK;
    dso_actor actor;
    dso_actor successor_actor;
    dso_live_terminal_probe live_terminal_probe = {0};
    cmeta_thread_t live_terminal_thread = NULL;
    uint64_t successor_ping_token = 0u;
    uint64_t actor_token = 0u;
    cflow_actor_send_status actor_send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    unsigned rounds = 0u;
    bool quiet = false;

    CHECK(cmeta_plugin_registry_init(&registry, &registry_config) ==
          CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_load(
        &registry, ORBIT_DSO_PROVIDER_N_PATH, &original) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_load(
        &registry, ORBIT_DSO_PROVIDER_NEXT_PATH, &successor) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_start(
        &registry, original) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_start(
        &registry, successor) == CMETA_PLUGIN_OK);

    build(&n, UINT64_C(21), &registry, original);
    build(&next, UINT64_C(22), &registry, successor);
    CHECK(cmeta_plugin_registry_get_lifecycle(
        &registry, original, &info) == CMETA_PLUGIN_OK);
    CHECK(info.active_leases == 1u);

    CHECK(salts_service_host_runtime_init(&runtime) ==
          SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_publish(
        &runtime, &n.generation, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == NULL);
    CHECK(orbit_ace_scope_guard_open(
        &old_scope, &runtime, UINT64_C(21), &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(actual_provider_value(&old_scope) == 37);
    /* Bind a real CFlow Actor while the original DSO Provider is leased. */
    dso_actor_start(&actor, &old_scope);
    CHECK(orbit_ace_scope_guard_try_release(&old_scope, &exact) ==
          ORBIT_ACE_SCOPE_BUSY);

    CHECK(salts_service_host_runtime_publish(
        &runtime, &next.generation, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == &n.generation);
    CHECK(orbit_ace_scope_guard_open(
        &next_scope, &runtime, UINT64_C(22), &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(actual_provider_value(&next_scope) == 83);
    /*
     * Both generations own live, independently scheduled generated Rooms.
     * The second Room MUST acquire its receiver from the newly published
     * Scope, not rebind N's pre-existing receiver or method catalog.
     */
    dso_actor_start(&successor_actor, &next_scope);
    CHECK(successor_actor.binding.receiver != actor.binding.receiver);
    CHECK(successor_actor.producer.actor.impl != actor.producer.actor.impl);
    CHECK(orbit_ace_scope_guard_generation_id(&next_scope) == UINT64_C(22));
    CHECK(orbit_ace_scope_guard_try_release(&next_scope, &exact) ==
          ORBIT_ACE_SCOPE_BUSY);
    /* Old provider still executes from its original loaded DSO. */
    CHECK(actual_provider_value(&old_scope) == 37);
    CHECK(salts_service_host_generation_drain(
        &runtime, &n.generation) == SALTS_SERVICE_HOST_BUSY);

    CHECK(cmeta_plugin_registry_request_stop(
        &registry, original) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_unload(
        &registry, original) == CMETA_PLUGIN_BUSY);

    /* Block the first N generated Ping terminal until an independent thread
     * proves Scope BUSY, generation drain BUSY and real Plugin unload BUSY.
     */
    live_terminal_probe.actor = &actor;
    live_terminal_probe.scope = &old_scope;
    live_terminal_probe.runtime = &runtime;
    live_terminal_probe.generation = &n.generation;
    live_terminal_probe.registry = &registry;
    live_terminal_probe.plugin = original;
    atomic_init(&live_terminal_probe.proved_busy, 0);
    atomic_store(&actor.hold_terminal, true);
    CHECK(cmeta_thread_create(
        &live_terminal_thread, dso_check_live_terminal_scope,
        &live_terminal_probe) == 0);

    /* The OLD dynamically loaded Provider is invoked inside the Actor's
     * real post-commit CFlow Subscriber after N+1 has been published.
     */
    {
        PingRequest ping = {UINT64_C(5)};
        PingRequest pong = {UINT64_C(7)};
        PingRequest cancel_request = {UINT64_C(1)};
        uint64_t old_pong_token = 0u;
        uint64_t successor_pong_token = 0u;
        uint64_t cancel_token = 0u;

        /* Same IDL and method IDs; different immutable Scope generations. */
        CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
            &actor.port, &actor.producer, &ping, &actor.ping_context,
            dso_generated_ping_done, &actor, &actor_token, &actor_send) ==
            ORBIT_CALL_OK);
        CHECK(actor_send == CFLOW_ACTOR_SEND_ACCEPTED);
        CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
            &successor_actor.port, &successor_actor.producer, &ping,
            &successor_actor.ping_context, dso_generated_ping_done,
            &successor_actor, &successor_ping_token, &actor_send) ==
            ORBIT_CALL_OK);
        CHECK(actor_send == CFLOW_ACTOR_SEND_ACCEPTED);
        CHECK(databind_4_Game_4_Room_4_Pong__orbit_call_async(
            &actor.port, &actor.producer, &pong, &actor.pong_context,
            dso_generated_pong_done, &actor, &old_pong_token, &actor_send) ==
            ORBIT_CALL_OK);
        CHECK(actor_send == CFLOW_ACTOR_SEND_ACCEPTED);
        CHECK(databind_4_Game_4_Room_4_Pong__orbit_call_async(
            &successor_actor.port, &successor_actor.producer, &pong,
            &successor_actor.pong_context, dso_generated_pong_done,
            &successor_actor, &successor_pong_token, &actor_send) ==
            ORBIT_CALL_OK);
        CHECK(actor_send == CFLOW_ACTOR_SEND_ACCEPTED);
        CHECK(actor_token && old_pong_token &&
              successor_ping_token && successor_pong_token);

        while ((actor.callbacks < 2u || successor_actor.callbacks < 2u) &&
               rounds++ < 2048u) {
            (void)cflow_executor_run_one(&actor.executor);
            (void)cflow_executor_run_one(&successor_actor.executor);
        }
        CHECK(actor.callbacks == 2u && actor.observed == 2u);
        CHECK(successor_actor.callbacks == 2u &&
              successor_actor.observed == 2u);
        CHECK(actor.errors == 0u && successor_actor.errors == 0u);
        CHECK(actor.first_result == UINT64_C(42));
        CHECK(actor.last_result == UINT64_C(144));
        CHECK(successor_actor.first_result == UINT64_C(88));
        CHECK(successor_actor.last_result == UINT64_C(190));
        CHECK(actor.receiver.invocations == 2u);
        CHECK(successor_actor.receiver.invocations == 2u);
        CHECK(cmeta_thread_join(&live_terminal_thread) == 0);
        CHECK(atomic_load(&live_terminal_probe.proved_busy) == 1);
        atomic_store(&actor.hold_terminal, false);
        CHECK(orbit_call_u64_observe(&actor.port, actor_token) ==
              ORBIT_CALL_STALE);
        CHECK(orbit_call_u64_observe(
            &successor_actor.port, successor_ping_token) == ORBIT_CALL_STALE);

        /* Cancel an accepted N call before owner progress: exactly one
         * CANCELLED terminal, no N business invocation or N+1 mutation.
         */
        CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
            &actor.port, &actor.producer, &cancel_request,
            &actor.cancel_context, dso_generated_cancel_done,
            &actor, &cancel_token, &actor_send) == ORBIT_CALL_OK);
        CHECK(actor_send == CFLOW_ACTOR_SEND_ACCEPTED);
        CHECK(orbit_call_u64_cancel(&actor.port, cancel_token) ==
              ORBIT_CALL_OK);
        CHECK(actor.cancelled == 1u);
        rounds = 0u;
        while (actor.observed < 3u && rounds++ < 2048u)
            (void)cflow_executor_run_one(&actor.executor);
        CHECK(actor.observed == 3u && actor.callbacks == 2u);
        CHECK(actor.cancelled == 1u && actor.errors == 0u);
        CHECK(actor.receiver.invocations == 2u);
        CHECK(successor_actor.receiver.invocations == 2u);
        CHECK(orbit_call_u64_cancel(&actor.port, cancel_token) ==
              ORBIT_CALL_STALE);
    }
    CHECK(orbit_ace_scope_guard_generation_id(&old_scope) == UINT64_C(21));

    dso_actor_stop(&actor);
    CHECK(orbit_ace_scope_guard_try_release(
        &old_scope, &exact) == ORBIT_ACE_SCOPE_BUSY);
    CHECK(orbit_call_port_try_destroy(&actor.port) == ORBIT_CALL_OK);

    CHECK(orbit_ace_scope_guard_try_release(
        &old_scope, &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(salts_service_host_generation_drain(
        &runtime, &n.generation) == SALTS_SERVICE_HOST_OK);
    CHECK(cmeta_plugin_registry_get_lifecycle(
        &registry, original, &info) == CMETA_PLUGIN_OK);
    CHECK(info.active_leases == 0u);
    CHECK(cmeta_plugin_registry_poll_quiescent(
        &registry, original, &quiet) == CMETA_PLUGIN_OK);
    CHECK(quiet);
    CHECK(cmeta_plugin_registry_unload(
        &registry, original) == CMETA_PLUGIN_OK);

    {
        /*
         * Old N's real DSO is already unloaded. A still-active N+1
         * generated Room must continue through its own Scope-pinned vtable.
         */
        PingRequest after_old_unload = {UINT64_C(1)};
        uint64_t token = 0u;
        rounds = 0u;
        CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
            &successor_actor.port, &successor_actor.producer,
            &after_old_unload, &successor_actor.ping_context,
            dso_generated_ping_done, &successor_actor,
            &token, &actor_send) == ORBIT_CALL_OK);
        CHECK(actor_send == CFLOW_ACTOR_SEND_ACCEPTED);
        while (successor_actor.callbacks < 3u && rounds++ < 2048u)
            (void)cflow_executor_run_one(&successor_actor.executor);
        CHECK(successor_actor.callbacks == 3u);
        CHECK(successor_actor.first_result == UINT64_C(84));
        CHECK(successor_actor.receiver.invocations == 3u);
        CHECK(successor_actor.errors == 0u);
        CHECK(orbit_call_u64_observe(
            &successor_actor.port, successor_ping_token) ==
              ORBIT_CALL_STALE);
    }

    CHECK(salts_service_host_runtime_close(
        &runtime, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == &next.generation);
    CHECK(salts_service_host_generation_drain(
        &runtime, &next.generation) == SALTS_SERVICE_HOST_BUSY);
    dso_actor_stop(&successor_actor);
    CHECK(orbit_ace_scope_guard_try_release(
        &next_scope, &exact) == ORBIT_ACE_SCOPE_BUSY);
    CHECK(orbit_call_port_try_destroy(&successor_actor.port) ==
          ORBIT_CALL_OK);
    CHECK(orbit_ace_scope_guard_try_release(
        &next_scope, &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(salts_service_host_generation_drain(
        &runtime, &next.generation) == SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_destroy(&runtime) ==
          SALTS_SERVICE_HOST_OK);
    CHECK(cmeta_plugin_registry_request_stop(
        &registry, successor) == CMETA_PLUGIN_OK);
    quiet = false;
    CHECK(cmeta_plugin_registry_poll_quiescent(
        &registry, successor, &quiet) == CMETA_PLUGIN_OK);
    CHECK(quiet);
    CHECK(cmeta_plugin_registry_unload(
        &registry, successor) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_destroy(&registry) == CMETA_PLUGIN_OK);

    puts("Orbit true Salts Plugin DSO N/N+1 and Scope unload PASS");
    return 0;
}
