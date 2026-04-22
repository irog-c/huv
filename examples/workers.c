/* Workers example: spawns N worker processes sharing the same listen port via
 * SO_REUSEPORT. Each worker runs its own libuv loop and has its own pid; the
 * kernel load-balances new connections across them. /pid returns the handling
 * worker's pid so you can see the distribution. */
#include "huv/server.h"

#include <stdio.h>
#include <unistd.h>

static unsigned detect_worker_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        return 1;
    return (unsigned)n;
}

static void health(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)req;
    (void)next;
    huv_response_status(res, 200);
    huv_response_send(res, "OK", 2);
}

static void pid_handler(huv_request_t *req, huv_response_t *res,
                        huv_next_fn next)
{
    (void)req;
    (void)next;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "pid=%d\n", (int)getpid());
    huv_response_status(res, 200);
    huv_response_send(res, buf, (size_t)n);
}

int main(void)
{
    huv_server_config_t cfg = HUV_SERVER_CONFIG_DEFAULT;
    cfg.port = 8080;
    cfg.workers = detect_worker_count();
    cfg.log_cb = huv_log_stderr;
    fprintf(stderr, "spawning %u worker(s) (one per logical core)\n",
            cfg.workers);

    huv_server_t *app = huv_server_new(&cfg);
    if (!app) {
        fprintf(stderr, "failed to create server\n");
        return 1;
    }

    huv_server_get(app, "/health", health);
    huv_server_get(app, "/pid", pid_handler);

    int rc = huv_server_run(app);
    huv_server_free(app);
    return rc;
}
