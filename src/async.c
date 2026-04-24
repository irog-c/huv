#include "internal.h"

#include <stdlib.h>

typedef struct
{
    uv_timer_t timer;
    huv_async_fn cb;
    void *userdata;
} timer_defer_ctx_t;

static void timer_defer_on_closed(uv_handle_t *h) { free(h->data); }

static void timer_defer_on_tick(uv_timer_t *t)
{
    timer_defer_ctx_t *ctx = t->data;
    huv_async_fn cb = ctx->cb;
    void *ud = ctx->userdata;
    /* Close before invoking user cb: ctx stays alive until close_cb frees it,
     * and the user cb can safely call huv_timer_defer again without
     * colliding with our cleanup. */
    uv_close((uv_handle_t *)&ctx->timer, timer_defer_on_closed);
    cb(ud);
}

int huv_timer_defer(const huv_request_t *req, unsigned delay_ms,
                    huv_async_fn cb, void *userdata)
{
    huv_conn_t *conn = req->conn;
    timer_defer_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    if (uv_timer_init(&conn->server->loop, &ctx->timer) != 0) {
        free(ctx);
        return -1;
    }
    ctx->timer.data = ctx;
    ctx->cb = cb;
    ctx->userdata = userdata;
    if (uv_timer_start(&ctx->timer, timer_defer_on_tick, delay_ms, 0) != 0) {
        uv_close((uv_handle_t *)&ctx->timer, timer_defer_on_closed);
        return -1;
    }
    return 0;
}

typedef struct
{
    uv_work_t req; /* must be first — we cast uv_work_t* back to this type */
    huv_async_fn work_cb;
    huv_async_fn done_cb;
    void *userdata;
} work_submit_ctx_t;

static void work_submit_on_work(uv_work_t *req)
{
    work_submit_ctx_t *ctx = (work_submit_ctx_t *)req;
    ctx->work_cb(ctx->userdata);
}

static void work_submit_on_done(uv_work_t *req, int status)
{
    (void)status; /* UV_ECANCELED on loop teardown — still invoke done_cb
                     so user can clean up / respond. */
    work_submit_ctx_t *ctx = (work_submit_ctx_t *)req;
    ctx->done_cb(ctx->userdata);
    free(ctx);
}

int huv_work_submit(const huv_request_t *req, huv_async_fn work_cb,
                    huv_async_fn done_cb, void *userdata)
{
    huv_conn_t *conn = req->conn;
    work_submit_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    ctx->work_cb = work_cb;
    ctx->done_cb = done_cb;
    ctx->userdata = userdata;
    if (uv_queue_work(&conn->server->loop, &ctx->req, work_submit_on_work,
                      work_submit_on_done) != 0) {
        free(ctx);
        return -1;
    }
    return 0;
}
