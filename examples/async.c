/* Async demo: timer-deferred response (/slow) and CPU-bound work offload
 * to the thread pool (/sum), showing the loop stays responsive. */
#include "huv/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void health(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)req;
    (void)next;
    huv_response_status(res, 200);
    huv_response_send(res, "OK", 2);
}

static void on_slow(void *ud)
{
    huv_response_t *res = ud;
    huv_response_status(res, 200);
    huv_response_send(res, "slow response\n", 14);
}

static void slow(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)next;
    if (huv_timer_defer(req, 1000, on_slow, res) != 0) {
        huv_response_status(res, 500);
        huv_response_send(res, "defer failed", 12);
    }
}

typedef struct {
    huv_response_t *res;
    unsigned long iters;
    unsigned long result;
} sum_ctx_t;

static void sum_work(void *ud)
{
    sum_ctx_t *ctx = ud;
    unsigned long s = 0;
    for (unsigned long i = 1; i <= ctx->iters; i++)
        s += i;
    ctx->result = s;
}

static void sum_done(void *ud)
{
    sum_ctx_t *ctx = ud;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "sum=%lu\n", ctx->result);
    huv_response_status(ctx->res, 200);
    huv_response_send(ctx->res, buf, (size_t)n);
    free(ctx);
}

static void sum(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)next;
    sum_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        huv_response_status(res, 500);
        huv_response_send(res, "oom", 3);
        return;
    }
    ctx->res = res;
    ctx->iters = 50000000; /* ~50M adds — noticeably off-loop */
    ctx->result = 0;
    if (huv_work_submit(req, sum_work, sum_done, ctx) != 0) {
        free(ctx);
        huv_response_status(res, 500);
        huv_response_send(res, "submit failed", 13);
    }
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
    huv_server_get(app, "/slow", slow);
    huv_server_get(app, "/sum", sum);

    int rc = huv_server_run(app);
    huv_server_free(app);
    return rc;
}
