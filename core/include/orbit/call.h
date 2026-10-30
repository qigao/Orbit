#ifndef ORBIT_CALL_H
#define ORBIT_CALL_H

#include <orbit/service.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Orbit M0b: bounded unary CALL correlation over one existing CFlow Actor.
 *
 * The caller supplies a Service Method whose underlying CFlow Event payload
 * is exactly canonical cmeta_type_uint64. A generation-checked 64-bit token is
 * copied by CFlow's own mailbox. The request/reply (u64 in this qualified
 * first slice) lives in ONE preallocated call slot, not a second message queue.
 *
 * CFlow action callbacks run before Machine transition commit:
 *   1. orbit_call_u64_request(token) copies the request.
 *   2. Business action computes and orbit_call_u64_stage_reply(token, ...).
 *   3. The action sets its uint64 observation to token.
 *   4. CFlow publishes that observation AFTER commit; ONLY THEN the actor's
 *      on_value callback calls orbit_call_u64_observe(token).
 *
 * A staged reply is NEVER a successful terminal. Cancel and stop race with
 * observation through one lock and each admitted call has at most one local
 * callback, including cancellation and failed local work. No retry, worker,
 * transport, scheduler, timer, or second Actor is created.
 *
 * These API names and only-u64 request/reply types are intentionally a small
 * native contract slice, NOT the finished general/IDL-generated Service API.
 */
typedef struct orbit_call_port {
    void *impl;
} orbit_call_port;

typedef enum orbit_call_status {
    ORBIT_CALL_OK = 0,
    ORBIT_CALL_INVALID_ARGUMENT,
    ORBIT_CALL_ALLOCATION_FAILED,
    ORBIT_CALL_FULL,
    ORBIT_CALL_CLOSED,
    ORBIT_CALL_REJECTED,       /* exact cflow_actor_send_status in out_send */
    ORBIT_CALL_TYPE_MISMATCH,
    ORBIT_CALL_STALE,
    ORBIT_CALL_DUPLICATE,
    ORBIT_CALL_NO_REPLY,
    ORBIT_CALL_BUSY
} orbit_call_status;

typedef enum orbit_call_terminal {
    ORBIT_CALL_TERMINAL_SUCCESS = 0,
    ORBIT_CALL_TERMINAL_APPLICATION_ERROR,
    ORBIT_CALL_TERMINAL_NO_REPLY,
    ORBIT_CALL_TERMINAL_CANCELLED,
    ORBIT_CALL_TERMINAL_STOPPED,
    ORBIT_CALL_TERMINAL_ACTOR_FAILED
} orbit_call_terminal;

/*
 * Terminal callbacks run OUTSIDE the call-port lock, and may run on the
 * CFlow observation owner (success) or on the cancel/close caller (failure).
 * The token and scalar result are copied values. The callback may race with
 * call_async returning; callback user/provider lifetime must cover the entire
 * live call port. Callbacks must not destroy the port or its CFlow Actor.
 */
typedef void (*orbit_call_u64_terminal_fn)(
    void *user, uint64_t token, orbit_call_terminal outcome,
    int application_status, uint64_t response);

typedef struct orbit_call_port_stats {
    size_t capacity;
    size_t occupied;
    size_t admitting;
    size_t callbacks_inflight;
    uint64_t admitted;
    uint64_t completed_success;
    uint64_t completed_error;
    uint64_t cancelled;
    uint64_t stopped;
    uint64_t rejected;
    uint64_t rejected_full;
    bool closed;
} orbit_call_port_stats;

/*
 * Exactly capacity bounded slots, one Salts Platform mutex and no data-path
 * allocations. capacity is in [1, UINT32_MAX]; token generation never wraps.
 * port must be zero-initialized; do not copy or move it while live.
 */
orbit_call_status orbit_call_port_init(orbit_call_port *port, size_t capacity);

/*
 * Admit a scalar call. out_token is reset to zero on rejection. A SUCCESS
 * return means only that the CFlow Actor accepted one token Event.
 * out_send preserves the exact CFlow rejection for ORBIT_CALL_REJECTED.
 *
 * FULL leaves caller request/callback state untransferred; rejected calls
 * receive no callback. Every accepted call receives exactly one local
 * terminal callback when it is observed, cancelled or closed.
 *
 * This API requires the named method's Event type == cmeta_type_uint64 and
 * the Service's method catalog + CFlow producer ref to remain borrowed-live.
 * No async call is admitted if the pool has no free slot/credit.
 */
orbit_call_status orbit_call_u64_async(
    orbit_call_port *port, const orbit_service_ref *service,
    orbit_method_id method, uint64_t request,
    orbit_call_u64_terminal_fn terminal, void *user,
    uint64_t *out_token, cflow_actor_send_status *out_send);

/*
 * Actor action callbacks call these helpers on the existing CFlow owner.
 * Cancelled/reused tokens return STALE; all operations copy scalars under
 * the port mutex and never return raw borrowed slot storage.
 */
orbit_call_status orbit_call_u64_request(
    orbit_call_port *port, uint64_t token, uint64_t *out_request);
orbit_call_status orbit_call_u64_stage_reply(
    orbit_call_port *port, uint64_t token,
    int application_status, uint64_t response);

/*
 * Called ONLY from the CFlow Actor on_value observation callback (or an
 * equivalent authoritative post-commit observation). If no reply was staged,
 * settles with TERMINAL_NO_REPLY instead of silently leaking the call.
 * A stale token cannot affect a slot subsequently reused by another call.
 */
orbit_call_status orbit_call_u64_observe(orbit_call_port *port, uint64_t token);

/*
 * Request a local cancellation. If an accepted call is still in admission,
 * defer the terminal until the CFlow send result is known. Cancellation never
 * rolls back an action already running or its effects. Stale token -> STALE.
 */
orbit_call_status orbit_call_u64_cancel(orbit_call_port *port, uint64_t token);

/*
 * Withdraw new call admission and settle active calls exactly once. Only
 * STOPPED/ACTOR_FAILED are valid close reasons. In-flight send admission
 * settles itself after it knows whether CFlow accepted or rejected the Event.
 * Call from Actor on_done/on_error or the external owner shutdown path.
 * Calling close repeatedly is allowed. No callback under the internal mutex.
 */
orbit_call_status orbit_call_port_close(
    orbit_call_port *port, orbit_call_terminal reason);

bool orbit_call_port_get_stats(
    const orbit_call_port *port, orbit_call_port_stats *out);

/*
 * Non-blocking destruction, BUSY while an admission/callback/close remains.
 * The caller MUST have already destroyed/quiesced the CFlow Actor and stopped
 * ALL other API users, including producers and any callback provider.
 * This does not unload/retain a Plugin; keep its ServiceHost Scope alive
 * through Actor and call-port destruction, then release the Scope.
 */
orbit_call_status orbit_call_port_try_destroy(orbit_call_port *port);

#ifdef __cplusplus
}
#endif

#endif /* ORBIT_CALL_H */
