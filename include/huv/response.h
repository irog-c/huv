#ifndef HUV_RESPONSE_H
#define HUV_RESPONSE_H

#include "huv/types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void huv_response_status(huv_response_t *res, int status);
void huv_response_header(huv_response_t *res, const char *name,
                         const char *value);

/* Atomic send: status line + headers + body in a single write. Use for
 * complete responses where the body fits in memory. Consumes the response;
 * after calling, no further huv_response_* calls will do anything. */
void huv_response_send(huv_response_t *res, const char *body, size_t len);

/* ---- Streaming ----
 * Alternative to huv_response_send for responses where the body is produced
 * incrementally or whose total size isn't known upfront. Usage:
 *   huv_response_status(res, 200);
 *   huv_response_header(res, "Content-Type", "text/event-stream");
 *   huv_response_write_head(res);
 *   huv_response_write(res, chunk1, n1);
 *   huv_response_write(res, chunk2, n2);
 *   huv_response_end(res);
 *
 * If the caller set a "Content-Length" header before write_head, the body
 * is sent raw. Otherwise the server auto-uses Transfer-Encoding: chunked. */

/* Commit status line + headers to the wire. After this, headers can no
 * longer be modified. Calling twice is a no-op. */
void huv_response_write_head(huv_response_t *res);

/* Append a body chunk. Must be called after write_head and before end.
 * Zero-length writes are ignored. Returns 0 on success, -1 on error
 * (allocation failure, bad state, or connection already closed) — in that
 * case the response is aborted and the connection is closed. */
int huv_response_write(huv_response_t *res, const char *data, size_t len);

/* Finalize the response. Must be called exactly once per streaming response
 * (otherwise the connection hangs until request_timeout_ms). */
void huv_response_end(huv_response_t *res);

#ifdef __cplusplus
}
#endif

#endif
