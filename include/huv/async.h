#ifndef HUV_ASYNC_H
#define HUV_ASYNC_H

#include "huv/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Async handlers ----
 * A handler may return without calling huv_response_send; if it does, it
 * MUST eventually call huv_response_send on the same response, even if the
 * client has disconnected (send is a safe no-op in that case).
 * The req/res pointers remain valid until huv_response_send is called. */

typedef void (*huv_async_fn)(void *userdata);

/* Schedule cb(userdata) to run delay_ms from now, on the server's loop
 * thread. Typically used to defer response completion (timeout, rate limit,
 * artificial delay). Returns 0 on success, -1 on allocation failure — if
 * -1, the callback will not fire and the caller still owns the response. */
int huv_timer_defer(const huv_request_t *req, unsigned delay_ms,
                    huv_async_fn cb, void *userdata);

/* Run work_cb(userdata) on a background worker thread, then done_cb(userdata)
 * on the server loop thread. Use for blocking I/O or CPU-bound work that
 * would stall the event loop (DB calls, file reads, hashing, etc.).
 * work_cb MUST NOT touch huv_request_t / huv_response_t — those are owned
 * by the loop thread. done_cb can call huv_response_send safely.
 * Returns 0 on success, -1 on allocation failure — if -1, neither callback
 * will fire and the caller still owns the response. */
int huv_work_submit(const huv_request_t *req, huv_async_fn work_cb,
                    huv_async_fn done_cb, void *userdata);

#ifdef __cplusplus
}
#endif

#endif
