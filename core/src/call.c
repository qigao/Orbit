#include <orbit/call.h>

#include <cmeta/cmeta.h>
#include <salts/thread.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One preallocated response-credit/correlation slot, NOT a message queue. */
typedef enum orbit_call_slot_state {
    ORBIT_SLOT_FREE = 0,
    ORBIT_SLOT_ADMITTING,
    ORBIT_SLOT_ACTIVE
} orbit_call_slot_state;

typedef struct orbit_call_slot {
    uint32_t generation;       /* never wraps; UINT32_MAX retires the slot */
    orbit_call_slot_state state;
    bool cancel_requested;
    bool staged;
    bool observed;
    orbit_method_id method;
    uint64_t request;
    uint64_t response;
    int application_status;
    orbit_call_u64_terminal_fn callback;
    void *callback_user;
} orbit_call_slot;

typedef struct orbit_call_impl {
    const orbit_service *owner; /* address-stable, borrowed until port destroy */
    /*
     * RETAIN a real CFlow producer reference while this port lives.
     * Merely caching actor.impl would allow allocator-address reuse after
     * the Actor owner was destroyed and all other refs were released.
     * This pin keeps the CFlow control block (and its identity) alive.
     */
    cflow_actor_ref actor_pin;
    const void *actor_identity;
    cmeta_mutex_t mutex;
    orbit_call_slot *slots;
    size_t capacity;
    size_t submitting;          /* protects in-flight CFlow try_send */
    size_t closing_calls;       /* protects close callbacks and iterations */
    size_t callbacks_inflight;  /* callbacks always invoked outside mutex */
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
    if (slot->generation != generation || slot->state == ORBIT_SLOT_FREE)
        return NULL;
    return slot;
}

/* Each terminal owns one callback credit until callback returns. */
static void orbit_call_terminal_locked(
    orbit_call_impl *impl, orbit_call_slot *slot, uint64_t token,
    orbit_call_terminal terminal, orbit_call_delivery *out) {
    out->callback = slot->callback;
    out->user = slot->callback_user;
    out->token = token;
    out->terminal = terminal;
    out->application_status = terminal == ORBIT_CALL_TERMINAL_SUCCESS ||
            terminal == ORBIT_CALL_TERMINAL_APPLICATION_ERROR
        ? slot->application_status : 0;
    out->response = terminal == ORBIT_CALL_TERMINAL_SUCCESS ||
            terminal == ORBIT_CALL_TERMINAL_APPLICATION_ERROR
        ? slot->response : 0u;

    slot->state = ORBIT_SLOT_FREE;
    slot->cancel_requested = false;
    slot->staged = false;
    slot->observed = false;
    slot->method = 0u;
    slot->request = 0u;
    slot->response = 0u;
    slot->application_status = 0;
    slot->callback = NULL;
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

/* Never call application code while holding the port mutex. */
static void orbit_call_deliver(orbit_call_impl *impl,
                               const orbit_call_delivery *delivery) {
    if (delivery->callback == NULL) return;
    delivery->callback(delivery->user, delivery->token,
                       delivery->terminal, delivery->application_status,
                       delivery->response);
    cmeta_mutex_lock(&impl->mutex);
    --impl->callbacks_inflight;
    cmeta_mutex_unlock(&impl->mutex);
}

static bool orbit_call_method_is_u64(
    const orbit_service_ref *service, orbit_method_id method,
    bool *out_found) {
    size_t first = 0u;
    size_t last;
    *out_found = false;
    if (service == NULL || service->actor.impl == NULL ||
        service->methods == NULL || method == 0u)
        return false;
    last = service->method_count;
    while (first < last) {
        const size_t index = first + (last - first) / 2u;
        const orbit_service_method *candidate = &service->methods[index];
        if (candidate->id == method) {
            *out_found = true;
            return cmeta_type_equal(candidate->request_type,
                                    &cmeta_type_uint64);
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
    if (!cflow_actor_ref_acquire(&owner->actor, &impl->actor_pin)) {
        cmeta_mutex_destroy(&impl->mutex);
        free(impl->slots);
        free(impl);
        return ORBIT_CALL_INVALID_ARGUMENT;
    }
    impl->capacity = capacity;
    impl->owner = owner;
    impl->actor_identity = impl->actor_pin.impl;
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
    bool found;
    cflow_actor_send_status send_status;
    bool accepted;

    if (out_token != NULL) *out_token = 0u;
    if (out_send != NULL) *out_send = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    if (impl == NULL || out_token == NULL || out_send == NULL ||
        terminal == NULL || service == NULL || method == 0u ||
        impl->actor_identity != service->actor.impl)
        return ORBIT_CALL_INVALID_ARGUMENT;
    if (!orbit_call_method_is_u64(service, method, &found))
        return found ? ORBIT_CALL_TYPE_MISMATCH : ORBIT_CALL_INVALID_ARGUMENT;

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
    slot->observed = false;
    slot->method = method;
    slot->request = request;
    slot->response = 0u;
    slot->application_status = 0;
    slot->callback = terminal;
    slot->callback_user = user;
    ++impl->submitting;
    cmeta_mutex_unlock(&impl->mutex);

    /* Borrowed token is copied into CFlow's ONE bounded Actor Mailbox. */
    send_status = orbit_service_ref_try_send(service, method, &token);
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
    if (slot->method != expected_method) {
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
    if (slot->method != expected_method) {
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
    for (;;) {
        delivery = (orbit_call_delivery){0};
        for (index = 0u; index < impl->capacity; ++index) {
            orbit_call_slot *slot = &impl->slots[index];
            if (slot->state == ORBIT_SLOT_ACTIVE) {
                const uint64_t token = orbit_call_token(index, slot->generation);
                orbit_call_terminal_locked(
                    impl, slot, token, impl->close_reason, &delivery);
                break;
            }
        }
        if (delivery.callback == NULL) break;
        cmeta_mutex_unlock(&impl->mutex);
        orbit_call_deliver(impl, &delivery);
        cmeta_mutex_lock(&impl->mutex);
    }
    --impl->closing_calls;
    cmeta_mutex_unlock(&impl->mutex);
    return ORBIT_CALL_OK;
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
    if (!impl->closed || impl->owner->actor.impl != NULL ||
        impl->submitting != 0u ||
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
    cmeta_mutex_unlock(&impl->mutex);
    /*
     * Owner Actor is already destroyed and no submissions/callbacks remain.
     * Release the retained upstream producer identity ONLY at this boundary.
     */
    cflow_actor_ref_release(&impl->actor_pin);
    cmeta_mutex_destroy(&impl->mutex);
    free(impl->slots);
    free(impl);
    port->impl = NULL;
    return ORBIT_CALL_OK;
}
