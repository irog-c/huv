#ifndef HUV_SERVER_H
#define HUV_SERVER_H

#include "huv/async.h"
#include "huv/request.h"
#include "huv/response.h"
#include "huv/types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /* Plain-HTTP listen port. Set to 0 to disable the plain listener entirely
     * (e.g., for HTTPS-only deployments). In that case TLS must be configured
     * via the tls_* fields below, or huv_server_run() will return an error. */
    int port;
    const char *bind_addr;

    unsigned idle_timeout_ms;
    unsigned request_timeout_ms;
    unsigned shutdown_timeout_ms;

    unsigned max_connections;
    unsigned max_body_bytes;
    size_t max_write_queue_bytes;

    /* Multi-worker mode. 0 or 1 runs a single process. Values > 1 cause
     * huv_server_run() to fork N workers, each with its own libuv loop and
     * SO_REUSEPORT listener socket; the kernel load-balances new connections
     * across them. The calling (master) process only waits and forwards
     * signals. Note: register routes and middleware BEFORE huv_server_run
     * so every worker inherits the same configuration. */
    unsigned workers;

    /* Only meaningful when workers > 1. When true, the master re-forks a
     * worker that exits abnormally (signal or non-zero status). Clean
     * exit-0s — which only happen during shutdown — are never respawned.
     * A per-slot crash-loop guard (10 respawns in any 60s window) retires
     * a slot that keeps dying, so a consistently broken handler can't
     * fork-bomb the master. Default: true. Set to false if an external
     * supervisor (systemd, k8s) manages worker lifetime for you. */
    bool respawn_workers;

    /* TLS: when tls_cert_path AND tls_key_path are both non-NULL, the server
     * binds a second listener on tls_port (default 8443) and accepts HTTPS
     * connections. Plain HTTP on `port` remains available. */
    const char *tls_cert_path;
    const char *tls_key_path;
    int tls_port;

    huv_log_fn log_cb;
    void *log_user;
} huv_server_config_t;

#define HUV_SERVER_CONFIG_DEFAULT                                              \
    (huv_server_config_t)                                                      \
    {                                                                          \
        .port = 8080, .bind_addr = "0.0.0.0", .idle_timeout_ms = 30000,        \
        .request_timeout_ms = 30000, .shutdown_timeout_ms = 10000,             \
        .max_connections = 1024, .max_body_bytes = 1u << 20,                   \
        .max_write_queue_bytes = 16u << 20, .workers = 1,                      \
        .respawn_workers = true, .tls_cert_path = NULL, .tls_key_path = NULL,  \
        .tls_port = 8443, .log_cb = NULL, .log_user = NULL                     \
    }

huv_server_t *huv_server_new(const huv_server_config_t *config);
void huv_server_free(huv_server_t *server);

void huv_server_use(huv_server_t *server, huv_handler_fn middleware);

/* Route paths may contain :param segments. Example: "/users/:id/posts/:pid"
 * matches "/users/42/posts/7" and captures id="42", pid="7". Captures are
 * read in the handler via huv_request_param(req, name). Static routes
 * always win over parameterized routes when both could match. */
void huv_server_get(huv_server_t *server, const char *path,
                    huv_handler_fn handler);
void huv_server_post(huv_server_t *server, const char *path,
                     huv_handler_fn handler);
void huv_server_put(huv_server_t *server, const char *path,
                    huv_handler_fn handler);
void huv_server_delete(huv_server_t *server, const char *path,
                       huv_handler_fn handler);
void huv_server_patch(huv_server_t *server, const char *path,
                      huv_handler_fn handler);
void huv_server_head(huv_server_t *server, const char *path,
                     huv_handler_fn handler);

int huv_server_run(huv_server_t *server);

#ifdef __cplusplus
}
#endif

#endif
