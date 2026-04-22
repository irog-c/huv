/* Basic demo: logger middleware, health probe, request header
 * lookup/iteration, query param lookup + URL decoding. */
#include "huv/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void logger(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    printf("%s %s\n", huv_request_method(req), huv_request_path(req));
    next(req, res);
}

static void health(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)req;
    (void)next;
    huv_response_status(res, 200);
    huv_response_send(res, "OK", 2);
}

static void whoami(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)next;
    const char *ua = huv_request_header(req, "User-Agent");
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "ua=%s\n", ua ? ua : "(none)");
    huv_response_status(res, 200);
    huv_response_send(res, buf, (size_t)n);
}

static void headers(huv_request_t *req, huv_response_t *res,
                    huv_next_fn next)
{
    (void)next;
    char buf[4096];
    int off = 0;
    size_t n = huv_request_header_count(req);
    for (size_t i = 0; i < n && off < (int)sizeof(buf); i++) {
        const char *name, *value;
        huv_request_header_at(req, i, &name, &value);
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%s: %s\n", name,
                        value);
    }
    huv_response_status(res, 200);
    huv_response_send(res, buf, (size_t)off);
}

static void echo_query(huv_request_t *req, huv_response_t *res,
                       huv_next_fn next)
{
    (void)next;
    const char *msg = huv_request_query_param(req, "msg");
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "msg=%s\n", msg ? msg : "(none)");
    huv_response_status(res, 200);
    huv_response_send(res, buf, (size_t)n);
}

int main(void)
{
    huv_server_config_t cfg = HUV_SERVER_CONFIG_DEFAULT;
    cfg.port = 8080;
    cfg.idle_timeout_ms = 15000;
    cfg.request_timeout_ms = 10000;
    cfg.shutdown_timeout_ms = 5000;
    cfg.max_body_bytes = 1u << 20;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    cfg.workers = ncpu > 1 ? (unsigned)ncpu : 1;
    /* Env override is handy for leak-checking runs (HUV_WORKERS=1) and for
     * pinning worker count in containers. */
    const char *w = getenv("HUV_WORKERS");
    if (w && *w) {
        char *end = NULL;
        long v = strtol(w, &end, 10);
        if (end != w && v > 0 && v <= 1024)
            cfg.workers = (unsigned)v;
    }
    cfg.log_cb = huv_log_stderr;

    huv_server_t *app = huv_server_new(&cfg);
    if (!app) {
        fprintf(stderr, "failed to create server\n");
        return 1;
    }

    huv_server_use(app, logger);
    huv_server_get(app, "/health", health);
    huv_server_get(app, "/whoami", whoami);
    huv_server_get(app, "/headers", headers);
    huv_server_get(app, "/echo", echo_query);
    huv_server_post(app, "/echo", echo_query);

    int rc = huv_server_run(app);
    huv_server_free(app);
    return rc;
}
