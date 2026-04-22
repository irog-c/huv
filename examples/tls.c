/* TLS demo: runs both plain HTTP on :8080 and HTTPS on :8443, sharing routes.
 * Cert paths are passed on argv so CMake can point at build/tls/server.{crt,key}. */
#include "huv/server.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void health(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)req;
    (void)next;
    huv_response_status(res, 200);
    huv_response_send(res, "OK", 2);
}

static void hello(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)next;
    const char *who = huv_request_query_param(req, "who");
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "hello %s over %s\n",
                     who ? who : "world",
                     huv_request_header(req, "Host") ? "tls-or-plain" : "?");
    huv_response_status(res, 200);
    huv_response_send(res, buf, (size_t)n);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <cert-path> <key-path>\n", argv[0]);
        return 1;
    }

    huv_server_config_t cfg = HUV_SERVER_CONFIG_DEFAULT;
    cfg.port = 8080;
    cfg.tls_port = 8443;
    cfg.tls_cert_path = argv[1];
    cfg.tls_key_path = argv[2];
    cfg.idle_timeout_ms = 15000;
    cfg.request_timeout_ms = 10000;
    cfg.shutdown_timeout_ms = 5000;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    cfg.workers = ncpu > 1 ? (unsigned)ncpu : 1;
    cfg.log_cb = huv_log_stderr;

    huv_server_t *app = huv_server_new(&cfg);
    if (!app) {
        fprintf(stderr, "failed to create server\n");
        return 1;
    }

    huv_server_get(app, "/health", health);
    huv_server_get(app, "/hello", hello);

    int rc = huv_server_run(app);
    huv_server_free(app);
    return rc;
}
