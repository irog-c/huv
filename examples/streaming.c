/* Streaming demo: chunked Transfer-Encoding (/count) and atomic bulk send
 * (/numbers) for comparison. */
#include "huv/server.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_NUMBERS 1000000

/* Bounded, error-checked string→int for query-param parsing. Returns fallback
 * on missing/malformed/out-of-range input, then caller clamps. */
static int parse_int(const char *s, int fallback)
{
    if (!s || !*s)
        return fallback;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno || end == s || v < INT_MIN || v > INT_MAX)
        return fallback;
    return (int)v;
}

static void health(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)req;
    (void)next;
    huv_response_status(res, 200);
    huv_response_send(res, "OK", 2);
}

static void numbers(huv_request_t *req, huv_response_t *res,
                    huv_next_fn next)
{
    (void)next;
    const char *nstr = huv_request_query_param(req, "n");
    int n = parse_int(nstr, 0);
    if (n < 0)
        n = 0;
    if (n > MAX_NUMBERS)
        n = MAX_NUMBERS;

    size_t cap = (size_t)n * 12 + 1;
    char *body = malloc(cap);
    if (!body) {
        huv_response_status(res, 500);
        huv_response_send(res, "oom", 3);
        return;
    }
    size_t off = 0;
    for (int i = 1; i <= n; i++)
        off += (size_t)snprintf(body + off, cap - off, "%d\n", i);

    huv_response_status(res, 200);
    huv_response_send(res, body, off);
    free(body);
}

/* Streams 1..n as individual chunks, demonstrating Transfer-Encoding: chunked.
 */
static void count(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)next;
    const char *to = huv_request_query_param(req, "to");
    int n = parse_int(to, 10);
    if (n < 0)
        n = 0;
    if (n > 100000)
        n = 100000;

    huv_response_status(res, 200);
    huv_response_write_head(res); /* no Content-Length → chunked */

    char line[16];
    for (int i = 1; i <= n; i++) {
        int len = snprintf(line, sizeof(line), "%d\n", i);
        if (huv_response_write(res, line, (size_t)len) < 0)
            break; /* conn closed or backpressure limit hit */
    }
    huv_response_end(res);
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
    huv_server_get(app, "/numbers", numbers);
    huv_server_get(app, "/count", count);

    int rc = huv_server_run(app);
    huv_server_free(app);
    return rc;
}
