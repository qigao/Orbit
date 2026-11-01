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
#include <cmeta/data.h>
#include <salts/thread.h>

#define DATABIND_NATIVE_ENABLE_ORBIT 1
#include "game.service_native.h"
#include "owned_room.service_native.h"

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
    salts_configurator_dependency dependencies[1];
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
    /* Test-only: suspend the canonical native function BEFORE the real
     * DSO provider method is called. This never changes its ABI.
     */
    atomic_bool hold_before_provider;
    atomic_bool business_entered;
    atomic_bool release_business;
} dso_room_receiver;

int databind_4_Game_4_Room_4_Ping(
    void *receiver, const PingRequest *request, PingResponse *response) {
    dso_room_receiver *room = (dso_room_receiver *)receiver;
    int value;
    if (!room || !request || !response) return 17;
    if (atomic_load(&room->hold_before_provider)) {
        size_t checks = 0u;
        atomic_store(&room->business_entered, true);
        while (!atomic_load(&room->release_business) &&
               checks++ < 10000u)
            cmeta_sleep_ms(1u);
        CHECK(atomic_load(&room->release_business));
    }
    /* A racing local cancellation must never unload this old N vtable.
     * Cancellation does not roll back already-started native work.
     */
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
    orbit_ace_native_activation activation;
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

/* The Provider projection is minted by this ACTOR's original Scope, not
 * supplied as an unrelated native pointer by the application.
 */
static bool dso_actor_init_receiver(
    void *user, const cmeta_interface_projection *projection,
    void **out_receiver) {
    dso_actor *actor = (dso_actor *)user;
    if (!actor || !out_receiver ||
        !cmeta_interface_projection_valid_for(
            projection, orbit_dso_value_interface()))
        return false;
    *out_receiver = NULL;
    actor->receiver.projected = orbit_dso_value_bind(
        projection->self,
        (const orbit_dso_value_vtable *)projection->dispatch);
    if (!orbit_dso_value_valid(&actor->receiver.projected))
        return false;
    *out_receiver = &actor->receiver;
    return true;
}

static void dso_actor_start_with_value(
    dso_actor *actor, orbit_ace_scope_guard *scope,
    const orbit_call_value_contract *managed_contract) {
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
    atomic_init(&actor->receiver.hold_before_provider, false);
    atomic_init(&actor->receiver.business_entered, false);
    atomic_init(&actor->receiver.release_business, false);
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
    if (managed_contract != NULL) {
        /* An isolated negative test uses the SAME original Actor and Port
         * with one owning CMeta VALUE contract; no call is ever published
         * through the scalar generated business method in that instance.
         */
        CHECK(orbit_call_port_init_value(
            &actor->port, &actor->service, 1u, managed_contract) ==
              ORBIT_CALL_OK);
    } else {
        CHECK(orbit_call_port_init(&actor->port, &actor->service, 2u) ==
              ORBIT_CALL_OK);
    }
    CHECK(orbit_service_ref_acquire(
        &actor->service, &actor->producer));

    {
        void *receiver = NULL;
        CHECK(orbit_ace_scope_guard_activate_native(
            scope, &actor->service, &actor->port,
            orbit_dso_value_interface(), dso_actor_init_receiver,
            actor, &actor->activation) == ORBIT_ACE_SCOPE_OK);
        CHECK(orbit_ace_native_activation_receiver(
            scope, &actor->activation, &actor->service, &actor->port,
            &receiver) == ORBIT_ACE_SCOPE_OK);
        CHECK(receiver == &actor->receiver);
        /* Same generated Native Context-v1 Dispatcher and business symbol.
         * Only its activation-time receiver source is now Scope-minted.
         */
        CHECK(databind_4_Game_4_Room__orbit_bind(
            &actor->binding, &actor->port, receiver));
        CHECK(actor->binding.receiver == receiver);
    }
}

static void dso_actor_start(
    dso_actor *actor, orbit_ace_scope_guard *scope) {
    dso_actor_start_with_value(actor, scope, NULL);
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
    uint64_t cancel_token;
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


/*
 * M0j: the canonical N Ping native function has entered, but has not yet
 * invoked the borrowed N Provider vtable. An independent native thread
 * CANCELS the still-active call before releasing the method. It verifies
 * Scope/drain/real Plugin unload still refuse the early release.
 *
 * The owner resumes and invokes N's actual DSO method AFTER cancellation:
 * business side effects may complete, but no second SUCCESS terminal may
 * escape the already-cancelled generation token.
 */
static void dso_cancel_during_live_business(void *user) {
    dso_live_terminal_probe *probe = (dso_live_terminal_probe *)user;
    orbit_call_port_stats stats = {0};
    salts_service_host_status exact = SALTS_SERVICE_HOST_OK;
    size_t checks = 0u;

    while (!atomic_load(&probe->actor->receiver.business_entered) &&
           checks++ < 10000u)
        cmeta_sleep_ms(1u);
    CHECK(atomic_load(&probe->actor->receiver.business_entered));
    CHECK(orbit_call_port_get_stats(&probe->actor->port, &stats));
    CHECK(stats.occupied == 1u && stats.callbacks_inflight == 0u);
    CHECK(!stats.closed && probe->cancel_token != 0u);

    CHECK(orbit_call_u64_cancel(
        &probe->actor->port, probe->cancel_token) == ORBIT_CALL_OK);
    CHECK(orbit_call_u64_cancel(
        &probe->actor->port, probe->cancel_token) == ORBIT_CALL_STALE);
    CHECK(orbit_ace_scope_guard_try_release(probe->scope, &exact) ==
          ORBIT_ACE_SCOPE_BUSY);
    CHECK(salts_service_host_generation_drain(
        probe->runtime, probe->generation) == SALTS_SERVICE_HOST_BUSY);
    CHECK(cmeta_plugin_registry_unload(
        probe->registry, probe->plugin) == CMETA_PLUGIN_BUSY);
    CHECK(orbit_ace_scope_guard_generation_id(probe->scope) ==
          UINT64_C(21));
    atomic_store(&probe->proved_busy, 2);
    atomic_store(&probe->actor->receiver.release_business, true);
}


/*
 * M0k.2c: the very same native Context-v1 owning VALUE generated Client and
 * Dispatcher used by the real CFlow Room must also invoke actual old-N /
 * new-N Plugin DSO vtables. This is an additional accepted scenario in
 * orbit.ace.dso_native, not a second runtime, Plugin loader or CTest target.
 *
 * The scalar M0i/M0j acceptance runs and fully retires FIRST, so this
 * separate real ServiceHost instance cannot mask their original BUSY proofs.
 */
typedef struct dso_owned_receiver {
    orbit_dso_value projected;
    uint8_t marker;
    unsigned invocations;
    atomic_bool hold_before_provider;
    atomic_bool native_entered;
    atomic_bool release_native;
} dso_owned_receiver;

int databind_9_OwnedGame_4_Room_4_Echo(
    void *receiver, const OwnedPacket *request, OwnedPacket *response) {
    dso_owned_receiver *room = (dso_owned_receiver *)receiver;
    static const unsigned char expected[] = {0u, 0xffu, 0x42u, 0x80u};
    size_t spins = 0u;
    int provider_value;
    if (!room || !request || !response)
        return 17;
    if (atomic_load(&room->hold_before_provider)) {
        atomic_store(&room->native_entered, true);
        while (!atomic_load(&room->release_native) &&
               spins++ < 10000u)
            cmeta_sleep_ms(1u);
        CHECK(atomic_load(&room->release_native));
    }
    /* These bytes are Port-owned, even when a racing local CANCELLED
     * terminal callback has already returned to another native thread.
     */
    CHECK(request->label.data != NULL && request->label.size == 6u);
    CHECK(memcmp(request->label.data, "Room-A", 6u) == 0);
    CHECK(request->payload.data != NULL &&
          request->payload.size == sizeof(expected));
    CHECK(memcmp(request->payload.data, expected,
                 sizeof(expected)) == 0);

    provider_value = orbit_dso_value_get(&room->projected);
    CHECK(provider_value == (int)room->marker); /* actual old/next DSO vtable */
    if (OwnedPacket_clone(response, request) != 0)
        return 18;
    response->payload.data[0] = (unsigned char)provider_value;
    ++room->invocations;
    return 0;
}

typedef struct dso_owned_actor {
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    orbit_service service;
    orbit_service_ref producer;
    orbit_call_port port;
    cflow_machine_action_binding action[1];
    dso_owned_receiver receiver;
    orbit_ace_native_activation activation;
    databind_9_OwnedGame_4_Room_4_Echo__orbit_value_owner value_owner;
    databind_9_OwnedGame_4_Room_4_Echo__orbit_context context;
    databind_9_OwnedGame_4_Room_4_Echo__orbit_context rejected_context;
    databind_9_OwnedGame_4_Room__orbit_binding binding;
    int state;
    unsigned observed;
    unsigned successes;
    unsigned cancelled;
    unsigned errors;
    uint8_t last_marker;
} dso_owned_actor;

static bool dso_owned_observe(
    void *user, const cmeta_type_desc *type, const void *value) {
    dso_owned_actor *actor = (dso_owned_actor *)user;
    orbit_call_status status;
    CHECK(actor != NULL && value != NULL);
    CHECK(cmeta_type_equal(type, &cmeta_type_uint64));
    status = databind_9_OwnedGame_4_Room__orbit_dispatch(
        &actor->binding, *(const uint64_t *)value);
    CHECK(status == ORBIT_CALL_OK || status == ORBIT_CALL_STALE);
    ++actor->observed;
    return true;
}

static void dso_owned_on_error(void *user, const char *message) {
    dso_owned_actor *actor = (dso_owned_actor *)user;
    (void)message;
    ++actor->errors;
    CHECK(orbit_call_port_close(
        &actor->port, ORBIT_CALL_TERMINAL_ACTOR_FAILED) == ORBIT_CALL_OK);
}

static void dso_owned_on_done(void *user) {
    dso_owned_actor *actor = (dso_owned_actor *)user;
    CHECK(orbit_call_port_close(
        &actor->port, ORBIT_CALL_TERMINAL_STOPPED) == ORBIT_CALL_OK);
}

static void dso_owned_done(
    void *user, uint64_t token, orbit_call_terminal terminal,
    int application_status, const OwnedPacket *response) {
    dso_owned_actor *actor = (dso_owned_actor *)user;
    static const unsigned char suffix[] = {0xffu, 0x42u, 0x80u};
    orbit_call_port_stats stats = {0};
    CHECK(actor != NULL && token != 0u && application_status == 0);
    CHECK(orbit_call_u64_cancel(&actor->port, token) == ORBIT_CALL_STALE);
    CHECK(orbit_call_port_get_stats(&actor->port, &stats));
    CHECK(stats.callbacks_inflight == 1u);
    if (terminal == ORBIT_CALL_TERMINAL_CANCELLED) {
        CHECK(response == NULL && stats.executing == 1u);
        ++actor->cancelled;
    } else {
        CHECK(terminal == ORBIT_CALL_TERMINAL_SUCCESS && response != NULL);
        CHECK(response->label.size == 6u && response->label.data != NULL);
        CHECK(memcmp(response->label.data, "Room-A", 6u) == 0);
        CHECK(response->payload.size == 4u && response->payload.data != NULL);
        CHECK(response->payload.data[0] == actor->receiver.marker);
        CHECK(memcmp(response->payload.data + 1u, suffix, sizeof(suffix)) == 0);
        actor->last_marker = response->payload.data[0];
        ++actor->successes;
    }
}

static bool dso_owned_actor_init_receiver(
    void *user, const cmeta_interface_projection *projection,
    void **out_receiver) {
    dso_owned_actor *actor = (dso_owned_actor *)user;
    if (!actor || !out_receiver ||
        !cmeta_interface_projection_valid_for(
            projection, orbit_dso_value_interface()))
        return false;
    *out_receiver = NULL;
    actor->receiver.projected = orbit_dso_value_bind(
        projection->self,
        (const orbit_dso_value_vtable *)projection->dispatch);
    if (!orbit_dso_value_valid(&actor->receiver.projected))
        return false;
    *out_receiver = &actor->receiver;
    return true;
}

static void dso_owned_actor_start(
    dso_owned_actor *actor, orbit_ace_scope_guard *scope, uint8_t marker) {
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
    cflow_actor_config config = {0};
    orbit_service_config service_config;
    void *bound_receiver = NULL;
    memset(actor, 0, sizeof(*actor));
    actor->receiver.marker = marker;
    atomic_init(&actor->receiver.hold_before_provider, false);
    atomic_init(&actor->receiver.native_entered, false);
    atomic_init(&actor->receiver.release_native, false);
    CHECK(cflow_machine_build(&actor->machine, &definition) == CFLOW_MACHINE_OK);
    CHECK(cflow_executor_owner_init_with_capacity(
        &actor->executor, 64u, NULL, NULL));
    CHECK(cflow_scheduler_owner_bind(&actor->scheduler, &actor->executor, 32u));
    actor->action[0] = (cflow_machine_action_binding){
        300u, dso_machine_action, NULL
    };
    config.machine = (cflow_machine_instance_config){
        &actor->machine, &actor->state, &cmeta_type_uint64,
        NULL, 0u, actor->action, 1u, 1u, &actor->executor
    };
    config.scheduler = &actor->scheduler;
    config.callbacks = (cflow_subscriber_callbacks){
        dso_owned_observe, dso_owned_on_error, dso_owned_on_done, actor
    };
    service_config =
        databind_9_OwnedGame_4_Room__orbit_service_config(&config);
    CHECK(service_config.method_count == 1u);
    CHECK(orbit_service_spawn(&actor->service, &service_config).status ==
          ORBIT_SERVICE_OK);
    CHECK(databind_9_OwnedGame_4_Room_4_Echo__orbit_port_init_value(
        &actor->port, &actor->service, 1u, &actor->value_owner) ==
          ORBIT_CALL_OK);
    CHECK(orbit_service_ref_acquire(&actor->service, &actor->producer));

    CHECK(orbit_ace_scope_guard_activate_native(
        scope, &actor->service, &actor->port,
        orbit_dso_value_interface(), dso_owned_actor_init_receiver,
        actor, &actor->activation) == ORBIT_ACE_SCOPE_OK);
    CHECK(orbit_ace_native_activation_receiver(
        scope, &actor->activation, &actor->service, &actor->port,
        &bound_receiver) == ORBIT_ACE_SCOPE_OK);
    CHECK(bound_receiver == &actor->receiver);
    CHECK(databind_9_OwnedGame_4_Room__orbit_bind(
        &actor->binding, &actor->port, bound_receiver));
}

static void dso_owned_actor_run_until(
    dso_owned_actor *actor, unsigned observed) {
    unsigned rounds = 0u;
    while (actor->observed < observed && rounds++ < 2048u)
        (void)cflow_executor_run_one(&actor->executor);
    CHECK(actor->observed == observed);
    CHECK(actor->errors == 0u);
}

static void dso_owned_actor_stop(dso_owned_actor *actor) {
    unsigned rounds = 0u;
    cflow_actor_status status =
        orbit_service_request_stop(&actor->service);
    CHECK(status == CFLOW_ACTOR_OK || status == CFLOW_ACTOR_STOPPED);
    while (cflow_actor_current_state(&actor->service.actor) !=
           CFLOW_ACTOR_STATE_STOPPED && rounds++ < 2048u)
        (void)cflow_executor_run_one(&actor->executor);
    CHECK(orbit_service_wait(&actor->service) ==
          CFLOW_ACTOR_STATE_STOPPED);
    orbit_service_ref_release(&actor->producer);
    orbit_service_destroy(&actor->service);
    CHECK(orbit_call_port_try_destroy(&actor->port) == ORBIT_CALL_OK);
    cflow_scheduler_destroy(&actor->scheduler);
    cflow_executor_destroy(&actor->executor);
    cflow_machine_destroy(&actor->machine);
}

typedef struct dso_owned_cancel_probe {
    dso_owned_actor *actor;
    orbit_ace_scope_guard *scope;
    salts_service_host_runtime *runtime;
    salts_service_host_generation *generation;
    cmeta_plugin_registry *registry;
    cmeta_plugin_ref original;
    uint64_t token;
    atomic_int proved_busy;
} dso_owned_cancel_probe;

static void dso_owned_cancel_worker(void *user) {
    dso_owned_cancel_probe *probe = (dso_owned_cancel_probe *)user;
    orbit_call_port_stats stats = {0};
    salts_service_host_status exact = SALTS_SERVICE_HOST_OK;
    size_t spins = 0u;
    while (!atomic_load(&probe->actor->receiver.native_entered) &&
           spins++ < 10000u)
        cmeta_sleep_ms(1u);
    CHECK(atomic_load(&probe->actor->receiver.native_entered));
    CHECK(orbit_call_port_get_stats(&probe->actor->port, &stats));
    CHECK(stats.executing == 1u && stats.occupied == 1u);
    CHECK(orbit_call_u64_cancel(
        &probe->actor->port, probe->token) == ORBIT_CALL_OK);
    CHECK(probe->actor->cancelled == 1u);
    CHECK(orbit_call_port_get_stats(&probe->actor->port, &stats));
    CHECK(stats.executing == 1u && stats.occupied == 1u);
    CHECK(stats.callbacks_inflight == 0u && stats.cancelled == 1u);
    CHECK(orbit_ace_scope_guard_try_release(
        probe->scope, &exact) == ORBIT_ACE_SCOPE_BUSY);
    CHECK(salts_service_host_generation_drain(
        probe->runtime, probe->generation) == SALTS_SERVICE_HOST_BUSY);
    CHECK(cmeta_plugin_registry_unload(
        probe->registry, probe->original) == CMETA_PLUGIN_BUSY);
    CHECK(orbit_ace_scope_guard_generation_id(probe->scope) ==
          UINT64_C(21));
    atomic_store(&probe->proved_busy, 1);
    atomic_store(&probe->actor->receiver.release_native, true);
}

static void test_managed_dso_lifecycle(void) {
    const cmeta_plugin_registry_config registry_config = {2u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref original = {0}, successor = {0};
    cmeta_plugin_lifecycle_info lifecycle = {0};
    dso_generation n = {0}, next = {0};
    salts_service_host_runtime runtime = SALTS_SERVICE_HOST_RUNTIME_INIT;
    salts_service_host_generation *previous = NULL;
    salts_service_host_status exact = SALTS_SERVICE_HOST_OK;
    orbit_ace_scope_guard n_scope = {0}, next_scope = {0};
    dso_owned_actor old_actor, new_actor;
    dso_owned_cancel_probe probe = {0};
    cmeta_thread_t cancel_thread = NULL;
    char label[] = "Room-A";
    unsigned char data[] = {0u, 0xffu, 0x42u, 0x80u};
    OwnedPacket scratch = {
        {label, sizeof(label) - 1u}, {data, sizeof(data)}
    };
    OwnedPacket owned_request = {0};
    uint64_t old_token = 0u, next_token = 0u, rejected = UINT64_MAX;
    cflow_actor_send_status send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    orbit_call_port_stats stats = {0};
    bool quiet = false;

    CHECK(cmeta_plugin_registry_init(&registry, &registry_config) ==
          CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_load(
        &registry, ORBIT_DSO_PROVIDER_N_PATH, &original) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_load(
        &registry, ORBIT_DSO_PROVIDER_NEXT_PATH, &successor) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_start(&registry, original) == CMETA_PLUGIN_OK);
    CHECK(cmeta_plugin_registry_start(&registry, successor) == CMETA_PLUGIN_OK);
    build(&n, UINT64_C(21), &registry, original);
    build(&next, UINT64_C(22), &registry, successor);
    CHECK(salts_service_host_runtime_init(&runtime) ==
          SALTS_SERVICE_HOST_OK);
    CHECK(salts_service_host_runtime_publish(
        &runtime, &n.generation, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == NULL);
    CHECK(orbit_ace_scope_guard_open(
        &n_scope, &runtime, UINT64_C(21), &exact) == ORBIT_ACE_SCOPE_OK);
    dso_owned_actor_start(&old_actor, &n_scope, 37u);
    CHECK(salts_service_host_runtime_publish(
        &runtime, &next.generation, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == &n.generation);
    CHECK(orbit_ace_scope_guard_open(
        &next_scope, &runtime, UINT64_C(22), &exact) == ORBIT_ACE_SCOPE_OK);
    dso_owned_actor_start(&new_actor, &next_scope, 83u);
    CHECK(old_actor.binding.receiver != new_actor.binding.receiver);
    CHECK(old_actor.activation.source != new_actor.activation.source);
    {
        void *wrong = &old_actor;
        CHECK(orbit_ace_native_activation_receiver(
            &n_scope, &new_actor.activation, &old_actor.service,
            &old_actor.port, &wrong) == ORBIT_ACE_SCOPE_INVALID_ARGUMENT);
        CHECK(wrong == NULL);
    }
    CHECK(old_actor.producer.actor.impl != new_actor.producer.actor.impl);
    CHECK(orbit_ace_scope_guard_generation_id(&n_scope) == UINT64_C(21));
    CHECK(orbit_ace_scope_guard_generation_id(&next_scope) == UINT64_C(22));
    CHECK(cmeta_plugin_registry_request_stop(
        &registry, original) == CMETA_PLUGIN_OK);

    CHECK(OwnedPacket_clone(&owned_request, &scratch) == 0);
    CHECK(databind_9_OwnedGame_4_Room_4_Echo__orbit_call_async(
        &old_actor.port, &old_actor.producer, &owned_request,
        &old_actor.context, dso_owned_done, &old_actor, &old_token, &send) ==
          ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED && old_token != 0u);
    CHECK(databind_9_OwnedGame_4_Room_4_Echo__orbit_call_async(
        &new_actor.port, &new_actor.producer, &owned_request,
        &new_actor.context, dso_owned_done, &new_actor, &next_token, &send) ==
          ORBIT_CALL_OK);
    CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED && next_token != 0u);
    CHECK(databind_9_OwnedGame_4_Room_4_Echo__orbit_call_async(
        &old_actor.port, &old_actor.producer, &owned_request,
        &old_actor.rejected_context, dso_owned_done, &old_actor,
        &rejected, &send) == ORBIT_CALL_FULL);
    CHECK(rejected == 0u && old_actor.successes == 0u);
    OwnedPacket_clear(&owned_request);
    label[0] = 'X'; data[1] = 0x11u; /* source no longer alive */

    dso_owned_actor_run_until(&old_actor, 1u);
    dso_owned_actor_run_until(&new_actor, 1u);
    CHECK(old_actor.successes == 1u && old_actor.receiver.invocations == 1u);
    CHECK(new_actor.successes == 1u && new_actor.receiver.invocations == 1u);
    CHECK(old_actor.last_marker == 37u && new_actor.last_marker == 83u);

    {
        char retry_label[] = "Room-A";
        unsigned char retry_bytes[] = {0u, 0xffu, 0x42u, 0x80u};
        OwnedPacket retry = {
            {retry_label, sizeof(retry_label) - 1u},
            {retry_bytes, sizeof(retry_bytes)}
        };
        uint64_t cancel_token = 0u;
        atomic_store(&old_actor.receiver.hold_before_provider, true);
        atomic_init(&probe.proved_busy, 0);
        CHECK(databind_9_OwnedGame_4_Room_4_Echo__orbit_call_async(
            &old_actor.port, &old_actor.producer, &retry,
            &old_actor.context, dso_owned_done, &old_actor,
            &cancel_token, &send) == ORBIT_CALL_OK);
        CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED && cancel_token != 0u);
        probe.actor = &old_actor;
        probe.scope = &n_scope;
        probe.runtime = &runtime;
        probe.generation = &n.generation;
        probe.registry = &registry;
        probe.original = original;
        probe.token = cancel_token;
        CHECK(cmeta_thread_create(
            &cancel_thread, dso_owned_cancel_worker, &probe) == 0);
        dso_owned_actor_run_until(&old_actor, 2u);
        CHECK(cmeta_thread_join(&cancel_thread) == 0);
        CHECK(atomic_load(&probe.proved_busy) == 1);
        CHECK(old_actor.successes == 1u && old_actor.cancelled == 1u);
        CHECK(old_actor.receiver.invocations == 2u);
        CHECK(new_actor.receiver.invocations == 1u);
        CHECK(orbit_call_port_get_stats(&old_actor.port, &stats));
        CHECK(stats.admitted == 2u && stats.completed_success == 1u);
        CHECK(stats.cancelled == 1u && stats.occupied == 0u);
        CHECK(stats.executing == 0u && stats.callbacks_inflight == 0u);
        CHECK(orbit_call_u64_cancel(
            &old_actor.port, cancel_token) == ORBIT_CALL_STALE);
    }

    dso_owned_actor_stop(&old_actor);
    CHECK(orbit_ace_scope_guard_try_release(
        &n_scope, &exact) == ORBIT_ACE_SCOPE_OK);
    CHECK(salts_service_host_generation_drain(
        &runtime, &n.generation) == SALTS_SERVICE_HOST_OK);
    CHECK(cmeta_plugin_registry_get_lifecycle(
        &registry, original, &lifecycle) == CMETA_PLUGIN_OK);
    CHECK(lifecycle.active_leases == 0u);
    CHECK(cmeta_plugin_registry_poll_quiescent(
        &registry, original, &quiet) == CMETA_PLUGIN_OK);
    CHECK(quiet);
    CHECK(cmeta_plugin_registry_unload(
        &registry, original) == CMETA_PLUGIN_OK);

    /* The old N image has REALLY unloaded. The managed N+1 generated
     * Room must still use its independently bound original Plugin Scope.
     */
    {
        char again_label[] = "Room-A";
        unsigned char again_bytes[] = {0u, 0xffu, 0x42u, 0x80u};
        OwnedPacket again = {
            {again_label, sizeof(again_label) - 1u},
            {again_bytes, sizeof(again_bytes)}
        };
        uint64_t token = 0u;
        CHECK(databind_9_OwnedGame_4_Room_4_Echo__orbit_call_async(
            &new_actor.port, &new_actor.producer, &again,
            &new_actor.context, dso_owned_done, &new_actor,
            &token, &send) == ORBIT_CALL_OK);
        CHECK(send == CFLOW_ACTOR_SEND_ACCEPTED && token != 0u);
        dso_owned_actor_run_until(&new_actor, 2u);
        CHECK(new_actor.successes == 2u);
        CHECK(new_actor.receiver.invocations == 2u);
        CHECK(new_actor.last_marker == 83u);
    }
    CHECK(salts_service_host_runtime_close(
        &runtime, &previous) == SALTS_SERVICE_HOST_OK);
    CHECK(previous == &next.generation);
    CHECK(salts_service_host_generation_drain(
        &runtime, &next.generation) == SALTS_SERVICE_HOST_BUSY);
    dso_owned_actor_stop(&new_actor);
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
    CHECK(successor_actor.activation.source != actor.activation.source);
    {
        void *wrong = &actor;
        CHECK(orbit_ace_native_activation_receiver(
            &old_scope, &successor_actor.activation,
            &actor.service, &actor.port, &wrong) ==
              ORBIT_ACE_SCOPE_INVALID_ARGUMENT);
        CHECK(wrong == NULL);
        CHECK(orbit_ace_native_activation_receiver(
            &next_scope, &actor.activation,
            &successor_actor.service, &successor_actor.port, &wrong) ==
              ORBIT_ACE_SCOPE_INVALID_ARGUMENT);
        CHECK(wrong == NULL);
    }
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

    /*
     * One more N Ping is now accepted. The CFlow owner executes the real
     * generated Context-v1 dispatcher, pauses INSIDE the canonical native
     * function, and the original native probe thread races cancellation
     * against the pre-acquired real N DSO Provider.
     */
    {
        PingRequest during_business = {UINT64_C(1)};
        uint64_t during_token = 0u;
        cmeta_thread_t business_thread = NULL;
        orbit_call_port_stats stats = {0};
        live_terminal_probe.cancel_token = 0u;
        atomic_store(&live_terminal_probe.proved_busy, 0);
        atomic_store(&actor.receiver.hold_before_provider, true);
        CHECK(databind_4_Game_4_Room_4_Ping__orbit_call_async(
            &actor.port, &actor.producer, &during_business,
            &actor.cancel_context, dso_generated_cancel_done,
            &actor, &during_token, &actor_send) == ORBIT_CALL_OK);
        CHECK(actor_send == CFLOW_ACTOR_SEND_ACCEPTED);
        CHECK(during_token != 0u);
        live_terminal_probe.cancel_token = during_token;
        CHECK(cmeta_thread_create(
            &business_thread, dso_cancel_during_live_business,
            &live_terminal_probe) == 0);
        /* The worker cannot cancel until the owner entered the business
         * symbol, so this is not the earlier pre-dispatch cancel case.
         */
        rounds = 0u;
        while (actor.observed < 4u && rounds++ < 2048u)
            (void)cflow_executor_run_one(&actor.executor);
        CHECK(actor.observed == 4u);
        CHECK(cmeta_thread_join(&business_thread) == 0);
        CHECK(atomic_load(&live_terminal_probe.proved_busy) == 2);
        atomic_store(&actor.receiver.hold_before_provider, false);
        CHECK(actor.cancelled == 2u && actor.callbacks == 2u);
        CHECK(actor.observed == 4u && actor.errors == 0u);
        CHECK(actor.receiver.invocations == 3u);
        CHECK(successor_actor.receiver.invocations == 2u);
        CHECK(orbit_call_port_get_stats(&actor.port, &stats));
        CHECK(stats.admitted == 4u && stats.cancelled == 2u);
        CHECK(stats.completed_success == 2u);
        CHECK(stats.occupied == 0u && stats.callbacks_inflight == 0u);
        CHECK(orbit_call_u64_observe(
            &actor.port, during_token) == ORBIT_CALL_STALE);
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

    test_managed_dso_lifecycle();
    puts("Orbit true Salts Plugin DSO N/N+1 and Scope unload PASS");
    return 0;
}
