#include <orbit/call.h>

#include <cmeta/cmeta.h>
#include <cmeta/data.h>
#include <salts/thread.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One preallocated response-credit/correlation slot, NOT a message queue. */
typedef enum orbit_call_slot_state {
    ORBIT_SLOT_FREE = 0,
    ORBIT_SLOT_ADMITTING,
    ORBIT_SLOT_ACTIVE,
    /*
     * Terminal claimed but callback still executing outside port mutex.
     * Holds the response credit until callback returns; otherwise repeated
     * callbacks can accumulate without respect to configured capacity.
     */
    ORBIT_SLOT_COMPLETING,
    ORBIT_SLOT_RETAINED, /* terminal ended, native execution still borrows */
    ORBIT_SLOT_RECLAIMING /* CMeta Provider cleanup without Port mutex */
} orbit_call_slot_state;

typedef struct orbit_call_slot {
    uint32_t generation;       /* never wraps; UINT32_MAX retires the slot */
    orbit_call_slot_state state;
    bool cancel_requested;
    bool staged;
    bool staging; /* response copy in flight outside Port mutex */
    bool observed;    orbit_method_id method;
    uint64_t request;
    uint64_t response;
    int application_status;
    orbit_call_u64_terminal_fn callback;
    orbit_call_value_terminal_fn value_callback;
    void *callback_user;
    bool managed;
    unsigned executing; /* one exclusive post-commit borrower */
    cmeta_data_temp owned_request;
    cmeta_data_temp owned_response;
} orbit_call_slot;

typedef struct orbit_call_impl {
    const orbit_service *owner; /* address-stable, borrowed until port destroy */
    /*
     * Retain one complete Orbit producer ref: it pins BOTH the original
     * CFlow Actor control identity and the Core-owned immutable catalog
     * while this CallPort lives, even after Service owner destruction.
     */
    orbit_service_ref actor_pin;
    const void *actor_identity;
    const orbit_service_method *methods;  /* immutable catalog identity */
    size_t method_count;
    cmeta_mutex_t mutex;
    orbit_call_slot *slots;
    size_t capacity;
    size_t submitting;          /* protects in-flight CFlow try_send */
    size_t closing_calls;       /* protects close callbacks and iterations */
    size_t callbacks_inflight;  /* callbacks always invoked outside mutex */
    size_t executing;           /* independent of local terminal callback */
    orbit_call_value_contract value_contract; /* one immutable managed method */
    bool value_enabled;
    bool destroying; /* final provider temp_close may reenter */
    bool cleanup_failed; /* latch permanent no-fail provider violation */
    bool closed;
    orbit_call_terminal close_reason;
    uint64_t admitted;
    uint64_t completed_success;
    uint64_t completed_error;
    uint64_t cancelled;
    uint64_t stopped;
    uint64_t rejected;
    uint64_t rejected_full;
} orbit_call_impl;

typedef struct orbit_call_delivery {
    orbit_call_u64_terminal_fn callback;
    orbit_call_value_terminal_fn value_callback;
    const void *borrowed_response; /* only valid during callback */
    void *user;
    uint64_t token;
    orbit_call_terminal terminal;
    int application_status;
    uint64_t response;
} orbit_call_delivery;

static orbit_call_impl *orbit_call_live(const orbit_call_port *port) {
    return port == NULL ? NULL : (orbit_call_impl *)port->impl;
}

static uint64_t orbit_call_token(size_t slot, uint32_t generation) {
    return ((uint64_t)generation << 32) | (uint64_t)(slot + 1u);
}

/* Caller holds the port mutex. Returns NULL for any expired generation. */
static orbit_call_slot *orbit_call_lookup_locked(
    orbit_call_impl *impl, uint64_t token) {
    const uint32_t one_based = (uint32_t)token;
    const uint32_t generation = (uint32_t)(token >> 32);
    orbit_call_slot *slot;
    if (one_based == 0u || generation == 0u ||
        (size_t)(one_based - 1u) >= impl->capacity)
        return NULL;
    slot = &impl->slots[one_based - 1u];
    if (slot->generation != generation ||
        slot->state == ORBIT_SLOT_FREE ||
        slot->state == ORBIT_SLOT_COMPLETING ||
        slot->state == ORBIT_SLOT_RETAINED ||
        slot->state == ORBIT_SLOT_RECLAIMING)
        return NULL;
    return slot;
}

/* Each terminal owns one callback credit until callback returns. */
static void orbit_call_terminal_locked(
    orbit_call_impl *impl, orbit_call_slot *slot, uint64_t token,
    orbit_call_terminal terminal, orbit_call_delivery *out) {
    out->callback = slot->callback;
    out->value_callback = slot->value_callback;
    out->borrowed_response = slot->managed &&
        terminal == ORBIT_CALL_TERMINAL_SUCCESS ?
        slot->owned_response.storage : NULL;
    out->user = slot->callback_user;
    out->token = token;
    out->terminal = terminal;
    out->application_status = terminal == ORBIT_CALL_TERMINAL_SUCCESS ||
            terminal == ORBIT_CALL_TERMINAL_APPLICATION_ERROR
        ? slot->application_status : 0;
    out->response = terminal == ORBIT_CALL_TERMINAL_SUCCESS ||
            terminal == ORBIT_CALL_TERMINAL_APPLICATION_ERROR
        ? slot->response : 0u;

    /* The slot cannot be reused while its callback is outstanding. */
    slot->state = ORBIT_SLOT_COMPLETING;
    slot->cancel_requested = false;
    slot->staged = false;
    slot->observed = false;
    /* Preserve method and managed storage through a concurrent executing
     * borrow. They are retired only after callback AND business completion.
     */
    slot->callback = NULL;
    slot->value_callback = NULL;
    slot->callback_user = NULL;

    ++impl->callbacks_inflight;
    switch (terminal) {
        case ORBIT_CALL_TERMINAL_SUCCESS: ++impl->completed_success; break;
        case ORBIT_CALL_TERMINAL_APPLICATION_ERROR:
        case ORBIT_CALL_TERMINAL_NO_REPLY:
            ++impl->completed_error;
            break;
        case ORBIT_CALL_TERMINAL_CANCELLED: ++impl->cancelled; break;
        case ORBIT_CALL_TERMINAL_STOPPED:
        case ORBIT_CALL_TERMINAL_ACTOR_FAILED:
            ++impl->stopped;
            break;
        default: break;
    }
}

/* External CMeta VALUE Providers may reenter the same Port. The caller
 * marks this credit RECLAIMING under the mutex and unlocks BEFORE running
 * either owning restore. Until both finish, the token is STALE and no
 * producer can reuse the slot or destroy the Port.
 */
static void orbit_call_reclaim_slot(orbit_call_impl *impl,
                                    orbit_call_slot *slot) {
    cmeta_status req = CMETA_OK, resp = CMETA_OK;
    if (slot->managed) {
        req = cmeta_data_value_restore_zero(
            slot->owned_request.data, slot->owned_request.storage);
        resp = cmeta_data_value_restore_zero(
            slot->owned_response.data, slot->owned_response.storage);
    }
    /* Attempt BOTH restores; failed lifecycle cleanup is fail-stop, never
     * permission to recycle a partially freed owned VALUE.
     */
    if (req != CMETA_OK || resp != CMETA_OK) abort();
    cmeta_mutex_lock(&impl->mutex);
    if (slot->state != ORBIT_SLOT_RECLAIMING ||
        slot->executing != 0u || slot->staging) abort();
    slot->managed = false;
    slot->method = 0u;
    slot->request = 0u;
    slot->response = 0u;
    slot->application_status = 0;
    slot->cancel_requested = false;
    slot->staged = false;
    slot->observed = false;
    slot->callback = NULL;
    slot->value_callback = NULL;
    slot->callback_user = NULL;
    slot->state = ORBIT_SLOT_FREE;
    cmeta_mutex_unlock(&impl->mutex);
}

/*
 * Never call application code while holding the port mutex. Retain this
 * exact slot's response credit during the entire callback (COMPLETING).
 * A callback may re-enter get_stats/cancel/close; its token is already
 * terminal and no new call may reuse the slot until this function returns.
 */
static void orbit_call_deliver(orbit_call_impl *impl,
                               const orbit_call_delivery *delivery) {
    orbit_call_slot *slot;
    uint32_t one_based;
    uint32_t generation;
    if (delivery->token == 0u) return;
    if (delivery->callback != NULL) {
        delivery->callback(delivery->user, delivery->token,
                           delivery->terminal, delivery->application_status,
                           delivery->response);
    } else if (delivery->value_callback != NULL) {
        delivery->value_callback(delivery->user, delivery->token,
                                 delivery->terminal,
                                 delivery->application_status,
                                 delivery->borrowed_response);
    }
    one_based = (uint32_t)delivery->token;
    generation = (uint32_t)(delivery->token >> 32);
    cmeta_mutex_lock(&impl->mutex);
    if (one_based == 0u || (size_t)(one_based - 1u) >= impl->capacity)
        abort(); /* Internal invariant, never a caller-controlled path. */
    slot = &impl->slots[one_based - 1u];
    if (slot->state != ORBIT_SLOT_COMPLETING ||
        slot->generation != generation || impl->callbacks_inflight == 0u)
        abort();
    --impl->callbacks_inflight;
    const bool reclaim = slot->executing == 0u;
    slot->state = reclaim ? ORBIT_SLOT_RECLAIMING : ORBIT_SLOT_RETAINED;
    cmeta_mutex_unlock(&impl->mutex);
    if (reclaim) orbit_call_reclaim_slot(impl, slot);
}

static bool orbit_call_method_is_token(
    const orbit_service_ref *service, orbit_method_id method,
    bool *out_found, cflow_event_id *out_event_id) {
    size_t first = 0u;
    size_t last;
    *out_found = false;
    *out_event_id = 0u;
    if (service == NULL || service->actor.impl == NULL ||
        service->methods == NULL || method == 0u)
        return false;
    last = service->method_count;
    while (first < last) {
        const size_t index = first + (last - first) / 2u;
        const orbit_service_method *candidate = &service->methods[index];
        if (candidate->id == method) {
            *out_found = true;
            if (candidate->flags != ORBIT_METHOD_CALL_TOKEN ||
                !cmeta_type_equal(candidate->request_type,
                                  &cmeta_type_uint64))
                return false;
            *out_event_id = candidate->event_id;
            return true;
        }
        if (candidate->id < method) first = index + 1u;
        else last = index;
    }
    return false;
}

orbit_call_status orbit_call_port_init(
    orbit_call_port *port, const orbit_service *owner, size_t capacity) {
    orbit_call_impl *impl;
    if (port == NULL || port->impl != NULL || owner == NULL ||
        owner->actor.impl == NULL || owner->methods == NULL ||
        owner->method_count == 0u || capacity == 0u ||
        capacity > (size_t)UINT32_MAX ||
        capacity > SIZE_MAX / sizeof(orbit_call_slot))
        return ORBIT_CALL_INVALID_ARGUMENT;

    impl = (orbit_call_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL) return ORBIT_CALL_ALLOCATION_FAILED;
    impl->slots = (orbit_call_slot *)calloc(capacity, sizeof(*impl->slots));
    if (impl->slots == NULL) {
        free(impl);
        return ORBIT_CALL_ALLOCATION_FAILED;
    }
    cmeta_mutex_init(&impl->mutex);
    if (impl->mutex == NULL) {
        free(impl->slots);
        free(impl);
        return ORBIT_CALL_ALLOCATION_FAILED;
    }
    if (!orbit_service_ref_acquire(owner, &impl->actor_pin)) {
        cmeta_mutex_destroy(&impl->mutex);
        free(impl->slots);
        free(impl);
        return ORBIT_CALL_INVALID_ARGUMENT;
    }
    impl->capacity = capacity;
    impl->owner = owner;
    impl->actor_identity = impl->actor_pin.actor.impl;
    impl->methods = impl->actor_pin.methods;
    impl->method_count = impl->actor_pin.method_count;
    impl->close_reason = ORBIT_CALL_TERMINAL_STOPPED;
    port->impl = impl;
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_u64_async(
    orbit_call_port *port, const orbit_service_ref *service,
    orbit_method_id method, uint64_t request,
    orbit_call_u64_terminal_fn terminal, void *user,
    uint64_t *out_token, cflow_actor_send_status *out_send) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot = NULL;
    orbit_call_delivery delivery = {0};
    size_t index = 0u;
    uint64_t token = 0u;
    cflow_event_id event_id = 0u;
    bool found;
    cflow_actor_send_status send_status;
    bool accepted;

    if (out_token != NULL) *out_token = 0u;
    if (out_send != NULL) *out_send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    if (impl == NULL || out_token == NULL || out_send == NULL ||
        terminal == NULL || service == NULL || method == 0u ||
        impl->actor_identity != service->actor.impl ||
        impl->methods != service->methods ||
        impl->method_count != service->method_count)
        return ORBIT_CALL_INVALID_ARGUMENT;
    if (!orbit_call_method_is_token(service, method, &found, &event_id))
        return found ? ORBIT_CALL_TYPE_MISMATCH : ORBIT_CALL_INVALID_ARGUMENT;
    if (impl->value_enabled && method == impl->value_contract.method)
        return ORBIT_CALL_TYPE_MISMATCH; /* managed method cannot use u64 */

    cmeta_mutex_lock(&impl->mutex);
    if (impl->closed) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_CLOSED;
    }
    for (index = 0u; index < impl->capacity; ++index) {
        orbit_call_slot *candidate = &impl->slots[index];
        if (candidate->state == ORBIT_SLOT_FREE &&
            candidate->generation < UINT32_MAX) {
            slot = candidate;
            break;
        }
    }
    if (slot == NULL) {
        ++impl->rejected_full;
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_FULL;
    }

    ++slot->generation;
    token = orbit_call_token(index, slot->generation);
    slot->state = ORBIT_SLOT_ADMITTING;
    slot->cancel_requested = false;
    slot->staged = false;
    slot->staging = false;
    slot->observed = false;
    slot->method = method;
    slot->request = request;
    slot->response = 0u;
    slot->application_status = 0;
    slot->callback = terminal;
    slot->value_callback = NULL;
    slot->managed = false;
    slot->executing = 0u;
    slot->callback_user = user;
    ++impl->submitting;
    cmeta_mutex_unlock(&impl->mutex);

    /*
     * Reserved CALL_TOKEN methods intentionally reject public generic send.
     * The trusted port bridge submits ONE trivially-copied Event to the
     * existing CFlow mailbox using the catalog-verified Event ID.
     */
    {
        const cflow_event_view event = {
            event_id, &cmeta_type_uint64, &token
        };
        send_status = cflow_actor_ref_try_send(&service->actor, &event);
    }
    accepted = send_status == CFLOW_ACTOR_SEND_ACCEPTED;

    cmeta_mutex_lock(&impl->mutex);
    --impl->submitting;
    /* This slot is still ADMITTING: close/cancel may only mark it pending. */
    if (!accepted) {
        slot->state = ORBIT_SLOT_FREE;
        slot->method = 0u;
        slot->request = 0u;
        slot->callback = NULL;
        slot->callback_user = NULL;
        ++impl->rejected;
    } else {
        ++impl->admitted;
        slot->state = ORBIT_SLOT_ACTIVE;
        if (impl->closed) {
            orbit_call_terminal_locked(impl, slot, token,
                                       impl->close_reason, &delivery);
        } else if (slot->cancel_requested) {
            orbit_call_terminal_locked(impl, slot, token,
                                       ORBIT_CALL_TERMINAL_CANCELLED, &delivery);
        } else if (slot->observed) {
            const orbit_call_terminal outcome = !slot->staged
                ? ORBIT_CALL_TERMINAL_NO_REPLY
                : (slot->application_status == 0
                    ? ORBIT_CALL_TERMINAL_SUCCESS
                    : ORBIT_CALL_TERMINAL_APPLICATION_ERROR);
            orbit_call_terminal_locked(impl, slot, token, outcome, &delivery);
        }
    }
    cmeta_mutex_unlock(&impl->mutex);

    *out_send = send_status;
    if (!accepted) return ORBIT_CALL_REJECTED;
    *out_token = token;
    orbit_call_deliver(impl, &delivery);
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_u64_method(
    orbit_call_port *port, uint64_t token, orbit_method_id *out_method) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot;
    if (impl == NULL || token == 0u || out_method == NULL)
        return ORBIT_CALL_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->mutex);
    slot = orbit_call_lookup_locked(impl, token);
    /* The method-id lookup has no borrowed VALUE pointer or reply state.
     * One canonical generated dispatcher routes both u64 and managed Calls.
     * Only the separate scalar request/stage_reply APIs reject managed
     * storage; cancellation and generation invalidation still return STALE.
     */
    if (slot == NULL || impl->closed ||
        slot->cancel_requested || slot->observed) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_STALE;
    }
    *out_method = slot->method;
    cmeta_mutex_unlock(&impl->mutex);
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_u64_request(
    orbit_call_port *port, uint64_t token,
    orbit_method_id expected_method, uint64_t *out_request) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot;
    if (impl == NULL || out_request == NULL || token == 0u ||
        expected_method == 0u)
        return ORBIT_CALL_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->mutex);
    slot = orbit_call_lookup_locked(impl, token);
    if (slot == NULL || impl->closed || slot->cancel_requested) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_STALE;
    }
    if (slot->method != expected_method || slot->managed) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_TYPE_MISMATCH;
    }
    *out_request = slot->request;
    cmeta_mutex_unlock(&impl->mutex);
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_u64_stage_reply(
    orbit_call_port *port, uint64_t token, orbit_method_id expected_method,
    int application_status, uint64_t response) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot;
    if (impl == NULL || token == 0u || expected_method == 0u)
        return ORBIT_CALL_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->mutex);
    slot = orbit_call_lookup_locked(impl, token);
    if (slot == NULL || impl->closed || slot->cancel_requested) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_STALE;
    }
    if (slot->method != expected_method || slot->managed) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_TYPE_MISMATCH;
    }
    if (slot->staged || slot->observed) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_DUPLICATE;
    }
    slot->staged = true;
    slot->application_status = application_status;
    slot->response = response;
    cmeta_mutex_unlock(&impl->mutex);
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_u64_observe(
    orbit_call_port *port, uint64_t token) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot;
    orbit_call_delivery delivery = {0};
    if (impl == NULL || token == 0u) return ORBIT_CALL_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->mutex);
    slot = orbit_call_lookup_locked(impl, token);
    if (slot == NULL || impl->closed || slot->cancel_requested) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_STALE;
    }
    if (slot->staging) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_BUSY;
    }
    if (slot->observed) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_DUPLICATE;
    }
    slot->observed = true;
    if (slot->state == ORBIT_SLOT_ACTIVE) {
        const orbit_call_terminal outcome = !slot->staged
            ? ORBIT_CALL_TERMINAL_NO_REPLY
            : (slot->application_status == 0
                ? ORBIT_CALL_TERMINAL_SUCCESS
                : ORBIT_CALL_TERMINAL_APPLICATION_ERROR);
        orbit_call_terminal_locked(impl, slot, token, outcome, &delivery);
    }
    cmeta_mutex_unlock(&impl->mutex);
    orbit_call_deliver(impl, &delivery);
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_u64_cancel(
    orbit_call_port *port, uint64_t token) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot;
    orbit_call_delivery delivery = {0};
    if (impl == NULL || token == 0u) return ORBIT_CALL_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->mutex);
    slot = orbit_call_lookup_locked(impl, token);
    if (slot == NULL) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_STALE;
    }
    if (slot->state == ORBIT_SLOT_ADMITTING) {
        slot->cancel_requested = true;
    } else {
        orbit_call_terminal_locked(impl, slot, token,
                                   ORBIT_CALL_TERMINAL_CANCELLED, &delivery);
    }
    cmeta_mutex_unlock(&impl->mutex);
    orbit_call_deliver(impl, &delivery);
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_port_close(
    orbit_call_port *port, orbit_call_terminal reason) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_delivery delivery = {0};
    size_t index;
    if (impl == NULL || (reason != ORBIT_CALL_TERMINAL_STOPPED &&
                         reason != ORBIT_CALL_TERMINAL_ACTOR_FAILED))
        return ORBIT_CALL_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->mutex);
    if (!impl->closed) {
        impl->closed = true;
        impl->close_reason = reason;
    }
    ++impl->closing_calls;
    /*
     * Each slot is scanned exactly once: O(capacity) rather than repeatedly
     * searching from slot zero after every callback (O(capacity^2)).
     * New admissions are already sealed by closed=true. An ADMITTING slot
     * is settled by its submitting thread after CFlow returns; a concurrent
     * cancel/observe/close may also consume an ACTIVE slot before this scan.
     *
     * Releasing the mutex to invoke a callback does not permit reuse:
     * COMPLETING retains that exact credit until the callback returns.
     */
    for (index = 0u; index < impl->capacity; ++index) {
        orbit_call_slot *slot = &impl->slots[index];
        delivery = (orbit_call_delivery){0};
        if (slot->state != ORBIT_SLOT_ACTIVE)
            continue;
        orbit_call_terminal_locked(
            impl, slot, orbit_call_token(index, slot->generation),
            impl->close_reason, &delivery);
        cmeta_mutex_unlock(&impl->mutex);
        orbit_call_deliver(impl, &delivery);
        cmeta_mutex_lock(&impl->mutex);
    }
    --impl->closing_calls;
    cmeta_mutex_unlock(&impl->mutex);
    return ORBIT_CALL_OK;
}

bool orbit_call_port_is_bound_to(
    const orbit_call_port *port, const orbit_service *service) {
    orbit_call_impl *impl = orbit_call_live(port);
    return impl != NULL && service != NULL &&
        impl->owner == service &&
        impl->actor_identity == service->actor.impl &&
        impl->methods == service->methods &&
        impl->method_count == service->method_count;
}

bool orbit_call_port_get_stats(const orbit_call_port *port,
                              orbit_call_port_stats *out) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_port_stats stats = {0};
    size_t i;
    if (impl == NULL || out == NULL) return false;
    cmeta_mutex_lock(&impl->mutex);
    stats.capacity = impl->capacity;
    stats.callbacks_inflight = impl->callbacks_inflight;
    stats.executing = impl->executing;
    stats.admitted = impl->admitted;
    stats.completed_success = impl->completed_success;
    stats.completed_error = impl->completed_error;
    stats.cancelled = impl->cancelled;
    stats.stopped = impl->stopped;
    stats.rejected = impl->rejected;
    stats.rejected_full = impl->rejected_full;
    stats.closed = impl->closed;
    for (i = 0u; i < impl->capacity; ++i) {
        if (impl->slots[i].state != ORBIT_SLOT_FREE) ++stats.occupied;
        if (impl->slots[i].state == ORBIT_SLOT_ADMITTING) ++stats.admitting;
    }
    cmeta_mutex_unlock(&impl->mutex);
    *out = stats;
    return true;
}

orbit_call_status orbit_call_port_try_destroy(orbit_call_port *port) {
    orbit_call_impl *impl = orbit_call_live(port);
    size_t index;
    if (impl == NULL) return ORBIT_CALL_INVALID_ARGUMENT;
    /* Actor, producer, observer and all external API callers MUST be quiescent
     * before this owner-side call; close callbacks/submissions are tracked
     * solely to reject destruction during those known live obligations. */
    cmeta_mutex_lock(&impl->mutex);
    if (impl->cleanup_failed) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_VALUE_CLEANUP_FAILED;
    }
    if (impl->destroying || !impl->closed ||
        impl->owner->actor.impl != NULL ||
        impl->submitting != 0u || impl->executing != 0u ||
        impl->closing_calls != 0u || impl->callbacks_inflight != 0u) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_BUSY;
    }
    for (index = 0u; index < impl->capacity; ++index) {
        if (impl->slots[index].state != ORBIT_SLOT_FREE) {
            cmeta_mutex_unlock(&impl->mutex);
            return ORBIT_CALL_BUSY;
        }
    }
    /* Even final cmeta_data_temp_close may invoke the external provider. */
    impl->destroying = true;
    cmeta_mutex_unlock(&impl->mutex);
    /*
     * Owner Actor is already destroyed and no submissions/callbacks remain.
     * Release the retained upstream producer identity ONLY at this boundary.
     */
    for (index = 0u; index < impl->capacity; ++index) {
        /*
         * CMeta's checked close is the ONLY cleanup authority. If a
         * Provider violates its no-fail restore-to-semantic-zero contract,
         * DO NOT release a potentially owned value, the saved descriptors
         * or the original Plugin/Scope owner. An already-closed earlier
         * temporary remains empty; the violating temp remains allocated.
         * No retry is permitted by this Port after a contract violation.
         */
        if (cmeta_data_temp_try_close(
                &impl->slots[index].owned_request) != CMETA_OK ||
            cmeta_data_temp_try_close(
                &impl->slots[index].owned_response) != CMETA_OK) {
            cmeta_mutex_lock(&impl->mutex);
            impl->cleanup_failed = true;
            impl->destroying = false;
            cmeta_mutex_unlock(&impl->mutex);
            return ORBIT_CALL_VALUE_CLEANUP_FAILED;
        }
    }
    orbit_service_ref_release(&impl->actor_pin);
    cmeta_mutex_destroy(&impl->mutex);
    free(impl->slots);
    free(impl);
    port->impl = NULL;
    return ORBIT_CALL_OK;
}


/*
 * The only managed Native slice: canonical owning byte buffers or one CMeta
 * V2 VALUE record whose leaves are primitive or owned byte buffers. A VIEW,
 * nested record, collection, nullable overlay or unknown provider is NOT
 * promoted to an owning request by guessing at its physical representation.
 */
static bool orbit_value_buffer_shape(const cmeta_data_desc *data) {
    const cmeta_data_buffer_ops *ops;
    if (data == NULL || !cmeta_data_desc_valid(data) ||
        (data->kind != CMETA_DATA_STRING &&
         data->kind != CMETA_DATA_BYTES) ||
        data->buffer_ops == NULL)
        return false;
    ops = data->buffer_ops;
    return ops->struct_size >= sizeof(*ops) &&
        ops->abi_version == CMETA_DATA_BUFFER_OPS_ABI_VERSION &&
        ops->ownership == CMETA_DATA_BUFFER_OWNED &&
        ops->read != NULL && ops->init_zero != NULL &&
        ops->restore_zero != NULL && ops->move != NULL;
}

static bool orbit_value_type_admitted(const cmeta_data_desc *data,
                                      size_t limit) {
    const cmeta_data_reflection_shape *shape;
    size_t index;
    bool has_buffer = false;
    if (data == NULL || !cmeta_data_desc_valid(data) ||
        data->storage_type == NULL ||
        !cmeta_data_value_traits_supported(data) ||
        data->storage_type->size == 0u ||
        data->storage_type->size > 65536u ||
        /* Hard limit deliberately conservative; every activation must also
         * provide its own smaller explicit total dynamic byte budget.
         */
        limit == 0u || limit > 65536u)
        return false;
    if (orbit_value_buffer_shape(data))
        return true;
    if (data->kind != CMETA_DATA_STRUCT ||
        data->abi_version != CMETA_DATA_DESC_REFLECTION_ABI_VERSION ||
        data->shape == NULL)
        return false;
    shape = (const cmeta_data_reflection_shape *)data->shape;
    if (shape->struct_size < sizeof(*shape) ||
        shape->mode != CMETA_DATA_REFLECTION_VALUE ||
        shape->structure.field_count == 0u ||
        shape->structure.field_count > CMETA_DATA_REFLECTION_MAX_FIELDS ||
        shape->structure.fields == NULL)
        return false;
    for (index = 0u; index < shape->structure.field_count; ++index) {
        const cmeta_data_field_desc *field = &shape->structure.fields[index];
        const cmeta_data_desc *member = field->value;
        if (member == NULL || !cmeta_data_desc_valid(member) ||
            member->storage_type == NULL ||
            field->offset > data->storage_type->size ||
            member->storage_type->size >
                data->storage_type->size - field->offset)
            return false;
        if (member->kind == CMETA_DATA_STRING ||
            member->kind == CMETA_DATA_BYTES) {
            if (!orbit_value_buffer_shape(member)) return false;
            has_buffer = true;
        } else if (member->kind != CMETA_DATA_BOOL &&
                   member->kind != CMETA_DATA_SINT &&
                   member->kind != CMETA_DATA_UINT &&
                   member->kind != CMETA_DATA_FLOAT) {
            return false; /* no nested/collection/VIEW fallback */
        }
    }
    return has_buffer;
}

/* Read-only pre-copy budget validation using the canonical BufferOps. */
static orbit_call_status orbit_value_check_size(
    const cmeta_data_desc *data, const void *value, size_t budget) {
    const cmeta_data_reflection_shape *shape;
    size_t i, used = 0u;
    if (value == NULL) return ORBIT_CALL_INVALID_ARGUMENT;
    if (data->kind == CMETA_DATA_STRING || data->kind == CMETA_DATA_BYTES) {
        const unsigned char *bytes = NULL;
        size_t size = 0u;
        const cmeta_status s = cmeta_data_buffer_read(
            data, value, budget, &bytes, &size);
        return s == CMETA_OK ? ORBIT_CALL_OK :
            (s == CMETA_CAPACITY_EXCEEDED ? ORBIT_CALL_VALUE_TOO_LARGE :
                                           ORBIT_CALL_VALUE_COPY_FAILED);
    }
    shape = (const cmeta_data_reflection_shape *)data->shape;
    for (i = 0u; i < shape->structure.field_count; ++i) {
        const cmeta_data_field_desc *field = &shape->structure.fields[i];
        if (field->value->kind == CMETA_DATA_STRING ||
            field->value->kind == CMETA_DATA_BYTES) {
            const unsigned char *bytes = NULL;
            size_t size = 0u;
            const cmeta_status s = cmeta_data_buffer_read(
                field->value, (const unsigned char *)value + field->offset,
                budget - used, &bytes, &size);
            if (s != CMETA_OK)
                return s == CMETA_CAPACITY_EXCEEDED ?
                    ORBIT_CALL_VALUE_TOO_LARGE :
                    ORBIT_CALL_VALUE_COPY_FAILED;
            used += size;
        }
    }
    return ORBIT_CALL_OK;
}

/* Executed only on the unexposed initial Port after partial init failure. */
static void orbit_value_init_abort(orbit_call_port *port) {
    orbit_call_impl *impl = orbit_call_live(port);
    size_t i;
    if (impl == NULL) return;
    for (i = 0u; i < impl->capacity; ++i) {
        cmeta_data_temp_close(&impl->slots[i].owned_request);
        cmeta_data_temp_close(&impl->slots[i].owned_response);
    }
    orbit_service_ref_release(&impl->actor_pin);
    cmeta_mutex_destroy(&impl->mutex);
    free(impl->slots);
    free(impl);
    port->impl = NULL;
}

orbit_call_status orbit_call_port_init_value(
    orbit_call_port *port, const orbit_service *owner, size_t capacity,
    const orbit_call_value_contract *contract) {
    orbit_call_impl *impl;
    orbit_call_status result;
    size_t i;
    bool found = false;
    if (contract == NULL || owner == NULL ||
        contract->method == 0u ||
        !orbit_value_type_admitted(contract->request,
                                  contract->max_request_bytes) ||
        !orbit_value_type_admitted(contract->response,
                                  contract->max_response_bytes))
        return ORBIT_CALL_INVALID_ARGUMENT;
    for (i = 0u; i < owner->method_count; ++i) {
        const orbit_service_method *method = &owner->methods[i];
        if (method->id == contract->method &&
            method->flags == ORBIT_METHOD_CALL_TOKEN &&
            cmeta_type_equal(method->request_type, &cmeta_type_uint64)) {
            found = true;
            break;
        }
    }
    if (!found) return ORBIT_CALL_TYPE_MISMATCH;
    result = orbit_call_port_init(port, owner, capacity);
    if (result != ORBIT_CALL_OK) return result;
    impl = orbit_call_live(port);
    impl->value_contract = *contract; /* only canonical descriptors borrowed */
    for (i = 0u; i < capacity; ++i) {
        if (cmeta_data_temp_open(
                contract->request, contract->request->storage_type->size,
                &impl->slots[i].owned_request) != CMETA_OK ||
            cmeta_data_temp_open(
                contract->response, contract->response->storage_type->size,
                &impl->slots[i].owned_response) != CMETA_OK) {
            orbit_value_init_abort(port);
            return ORBIT_CALL_ALLOCATION_FAILED;
        }
    }
    impl->value_enabled = true;
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_value_async(
    orbit_call_port *port, const orbit_service_ref *service,
    orbit_method_id method, const void *request,
    orbit_call_value_terminal_fn terminal, void *user,
    uint64_t *out_token, cflow_actor_send_status *out_send) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot = NULL;
    orbit_call_delivery delivery = {0};
    cflow_event_id event_id = 0u;
    bool found = false;
    size_t index;
    uint64_t token;
    cflow_actor_send_status send_status;
    orbit_call_status bound;
    bool accepted;

    if (out_token != NULL) *out_token = 0u;
    if (out_send != NULL) *out_send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    if (impl == NULL || !impl->value_enabled || request == NULL ||
        terminal == NULL || out_token == NULL || out_send == NULL ||
        service == NULL || method != impl->value_contract.method ||
        impl->actor_identity != service->actor.impl ||
        impl->methods != service->methods ||
        impl->method_count != service->method_count)
        return ORBIT_CALL_INVALID_ARGUMENT;
    if (!orbit_call_method_is_token(service, method, &found, &event_id))
        return found ? ORBIT_CALL_TYPE_MISMATCH :
                       ORBIT_CALL_INVALID_ARGUMENT;
    /* The caller owns this immutable value until admission returns. */
    bound = orbit_value_check_size(
        impl->value_contract.request, request,
        impl->value_contract.max_request_bytes);
    if (bound != ORBIT_CALL_OK) return bound;

    cmeta_mutex_lock(&impl->mutex);
    if (impl->closed) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_CLOSED;
    }
    for (index = 0u; index < impl->capacity; ++index) {
        orbit_call_slot *candidate = &impl->slots[index];
        if (candidate->state == ORBIT_SLOT_FREE &&
            candidate->generation < UINT32_MAX) {
            slot = candidate;
            break;
        }
    }
    if (slot == NULL) {
        ++impl->rejected_full;
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_FULL;
    }
    ++slot->generation;
    token = orbit_call_token(index, slot->generation);
    slot->state = ORBIT_SLOT_ADMITTING;
    slot->managed = true;
    slot->executing = 0u;
    slot->cancel_requested = false;
    slot->staged = false;
    slot->staging = false;
    slot->observed = false;
    slot->method = method;
    slot->callback = NULL;
    slot->value_callback = terminal;
    slot->callback_user = user;
    ++impl->submitting;
    cmeta_mutex_unlock(&impl->mutex);

    /* Borrowed provider DataDesc executes while the slot is ADMITTING.
     * No accepted Event exists yet, and the submitting credit prevents
     * destroy while the canonical owning deep-copy runs.
     */
    if (cmeta_data_value_copy(
            impl->value_contract.request,
            slot->owned_request.storage, request) != CMETA_OK) {
        cmeta_mutex_lock(&impl->mutex);
        --impl->submitting;
        ++impl->rejected;
        slot->state = ORBIT_SLOT_RECLAIMING;
        cmeta_mutex_unlock(&impl->mutex);
        orbit_call_reclaim_slot(impl, slot);
        return ORBIT_CALL_VALUE_COPY_FAILED; /* zero token, no callback */
    }

    /* Close might have won during the deep clone, before any CFlow send. */
    cmeta_mutex_lock(&impl->mutex);
    if (impl->closed) {
        --impl->submitting;
        slot->state = ORBIT_SLOT_RECLAIMING;
        cmeta_mutex_unlock(&impl->mutex);
        orbit_call_reclaim_slot(impl, slot);
        return ORBIT_CALL_CLOSED;
    }
    cmeta_mutex_unlock(&impl->mutex);

    {
        const cflow_event_view event = {
            event_id, &cmeta_type_uint64, &token
        };
        send_status = cflow_actor_ref_try_send(&service->actor, &event);
    }
    accepted = send_status == CFLOW_ACTOR_SEND_ACCEPTED;
    cmeta_mutex_lock(&impl->mutex);
    --impl->submitting;
    if (!accepted) {
        ++impl->rejected;
        slot->state = ORBIT_SLOT_RECLAIMING;
    } else {
        ++impl->admitted;
        slot->state = ORBIT_SLOT_ACTIVE;
        if (impl->closed)
            orbit_call_terminal_locked(
                impl, slot, token, impl->close_reason, &delivery);
        else if (slot->cancel_requested)
            orbit_call_terminal_locked(
                impl, slot, token, ORBIT_CALL_TERMINAL_CANCELLED, &delivery);
        else if (slot->observed)
            orbit_call_terminal_locked(impl, slot, token,
                !slot->staged ? ORBIT_CALL_TERMINAL_NO_REPLY :
                (slot->application_status == 0 ?
                    ORBIT_CALL_TERMINAL_SUCCESS :
                    ORBIT_CALL_TERMINAL_APPLICATION_ERROR), &delivery);
    }
    cmeta_mutex_unlock(&impl->mutex);
    *out_send = send_status;
    if (!accepted) {
        orbit_call_reclaim_slot(impl, slot);
        return ORBIT_CALL_REJECTED;
    }
    *out_token = token;
    orbit_call_deliver(impl, &delivery);
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_value_begin(
    orbit_call_port *port, uint64_t token,
    orbit_method_id expected_method, const void **out_request) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot;
    if (out_request != NULL) *out_request = NULL;
    if (impl == NULL || !impl->value_enabled ||
        token == 0u || expected_method == 0u || out_request == NULL)
        return ORBIT_CALL_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->mutex);
    slot = orbit_call_lookup_locked(impl, token);
    if (slot == NULL || !slot->managed ||
        impl->closed || slot->cancel_requested || slot->observed) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_STALE;
    }
    if (slot->method != expected_method) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_TYPE_MISMATCH;
    }
    if (slot->executing != 0u) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_DUPLICATE;
    }
    slot->executing = 1u; /* sole Actor post-commit owner */
    ++impl->executing;
    *out_request = slot->owned_request.storage;
    cmeta_mutex_unlock(&impl->mutex);
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_value_stage_reply(
    orbit_call_port *port, uint64_t token, orbit_method_id expected_method,
    int application_status, const void *response) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot;
    orbit_call_status bounded = ORBIT_CALL_OK;
    const uint32_t generation = (uint32_t)(token >> 32);
    if (impl == NULL || !impl->value_enabled ||
        token == 0u || expected_method == 0u)
        return ORBIT_CALL_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->mutex);
    slot = orbit_call_lookup_locked(impl, token);
    if (slot == NULL || !slot->managed || impl->closed ||
        slot->cancel_requested) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_STALE;
    }
    if (slot->method != expected_method) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_TYPE_MISMATCH;
    }
    if (slot->staged || slot->staging || slot->observed ||
        slot->executing != 1u) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_DUPLICATE;
    }
    if (application_status != 0) {
        /* No CMeta provider is called for an application-error terminal. */
        slot->staged = true;
        slot->application_status = application_status;
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_OK;
    }
    if (response == NULL) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_INVALID_ARGUMENT;
    }
    /* The original post-commit borrow pins both Port storage objects.
     * Reserve STAGING before invoking any provider outside the Port mutex.
     * A reentrant cancel/close may settle the local terminal, but it cannot
     * recycle this slot while the original business borrow still executes.
     */
    slot->staging = true;
    cmeta_mutex_unlock(&impl->mutex);

    bounded = orbit_value_check_size(
        impl->value_contract.response, response,
        impl->value_contract.max_response_bytes);
    if (bounded == ORBIT_CALL_OK &&
        cmeta_data_value_copy(
            impl->value_contract.response,
            slot->owned_response.storage, response) != CMETA_OK)
        bounded = ORBIT_CALL_VALUE_COPY_FAILED;

    cmeta_mutex_lock(&impl->mutex);
    if (slot->generation != generation || !slot->managed ||
        !slot->staging || slot->executing != 1u)
        abort(); /* only the original post-commit execution owner can end */
    slot->staging = false;
    if (impl->closed || slot->state != ORBIT_SLOT_ACTIVE ||
        slot->cancel_requested) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_STALE;
    }
    if (bounded != ORBIT_CALL_OK) {
        cmeta_mutex_unlock(&impl->mutex);
        return bounded; /* the generated owner emits NO_REPLY on observe */
    }
    slot->staged = true;
    slot->application_status = application_status;
    cmeta_mutex_unlock(&impl->mutex);
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_value_end(
    orbit_call_port *port, uint64_t token, orbit_method_id expected_method) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot;
    const uint32_t index = (uint32_t)token;
    const uint32_t generation = (uint32_t)(token >> 32);
    if (impl == NULL || !impl->value_enabled || index == 0u ||
        (size_t)(index - 1u) >= impl->capacity || generation == 0u ||
        expected_method == 0u)
        return ORBIT_CALL_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->mutex);
    slot = &impl->slots[index - 1u];
    if (slot->generation != generation || !slot->managed ||
        slot->executing != 1u ||
        slot->state == ORBIT_SLOT_FREE) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_STALE;
    }
    if (slot->method != expected_method) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_TYPE_MISMATCH;
    }
    if (slot->staging) {
        /* A reentrant Provider cannot drop the lifetime pin while its
         * response is still being deep-copied into Port-owned storage.
         */
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_BUSY;
    }
    --impl->executing;
    slot->executing = 0u;
    const bool reclaim = slot->state == ORBIT_SLOT_RETAINED;
    if (reclaim) slot->state = ORBIT_SLOT_RECLAIMING;
    cmeta_mutex_unlock(&impl->mutex);
    if (reclaim) orbit_call_reclaim_slot(impl, slot);
    return ORBIT_CALL_OK;
}

orbit_call_status orbit_call_value_observe(
    orbit_call_port *port, uint64_t token) {
    orbit_call_impl *impl = orbit_call_live(port);
    orbit_call_slot *slot;
    if (impl == NULL || !impl->value_enabled || token == 0u)
        return ORBIT_CALL_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->mutex);
    slot = orbit_call_lookup_locked(impl, token);
    if (slot == NULL || !slot->managed) {
        cmeta_mutex_unlock(&impl->mutex);
        return ORBIT_CALL_STALE;
    }
    cmeta_mutex_unlock(&impl->mutex);
    return orbit_call_u64_observe(port, token); /* same terminal owner */
}
