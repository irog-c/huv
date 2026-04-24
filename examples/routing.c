/* Routing demo: path params, all method helpers
 * (GET/POST/PUT/DELETE/PATCH/HEAD), 405 Method Not Allowed with Allow header.
 */
#include "huv/server.h"

#include <stdio.h>
#include <unistd.h>

static void health(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)req;
    (void)next;
    huv_response_status(res, 200);
    huv_response_send(res, "OK", 2);
}

static void user_detail(huv_request_t *req, huv_response_t *res,
                        huv_next_fn next)
{
    (void)next;
    const char *id = huv_request_param(req, "id");
    const char *pid = huv_request_param(req, "pid");
    char buf[256];
    int n;
    if (pid)
        n = snprintf(buf, sizeof(buf), "user=%s post=%s\n", id, pid);
    else
        n = snprintf(buf, sizeof(buf), "user=%s\n", id);
    huv_response_status(res, 200);
    huv_response_send(res, buf, (size_t)n);
}

/* Each method handler echoes the method name so tests can verify dispatch. */
static void method_echo(huv_request_t *req, huv_response_t *res,
                        huv_next_fn next)
{
    (void)next;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "method=%s\n", huv_request_method(req));
    huv_response_status(res, 200);
    huv_response_send(res, buf, (size_t)n);
}

int main(void)
{
    huv_server_config_t cfg = HUV_SERVER_CONFIG_DEFAULT;
    cfg.port = 8080;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    cfg.workers = ncpu > 1 ? (unsigned)ncpu : 1;
    cfg.log_cb = huv_log_stderr;

    huv_server_t *app = huv_server_new(&cfg);
    if (!app) {
        fprintf(stderr, "failed to create server\n");
        return 1;
    }

    huv_server_get(app, "/health", health);

    huv_server_get(app, "/users/:id", user_detail);
    huv_server_get(app, "/users/:id/posts/:pid", user_detail);

    /* /item registered under every method → tests confirm each helper
     * dispatches. */
    huv_server_get(app, "/item", method_echo);
    huv_server_post(app, "/item", method_echo);
    huv_server_put(app, "/item", method_echo);
    huv_server_delete(app, "/item", method_echo);
    huv_server_patch(app, "/item", method_echo);
    huv_server_head(app, "/item", method_echo);

    int rc = huv_server_run(app);
    huv_server_free(app);
    return rc;
}
