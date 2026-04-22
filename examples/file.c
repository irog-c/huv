/* File-read demo: serves files from a fixed directory on disk using
 * huv_work_submit so the blocking fopen/fread happens on a libuv thread,
 * not on the loop. The handler returns immediately after submitting; the
 * work thread reads the bytes, and the done callback sends them back on
 * the loop thread.
 *
 * Usage:
 *     ./example_file ./public
 *     curl 'http://localhost:8080/file?name=hello.txt'
 */
#include "huv/server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILE_BYTES (4u << 20) /* 4 MiB — keep the example's memory bounded */

static const char *g_serve_root = ".";

typedef struct {
    huv_response_t *res;
    char *path;
    char *data;
    size_t len;
    int status;
    const char *content_type;
} read_ctx_t;

static const char *content_type_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".txt")) return "text/plain; charset=utf-8";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".css")) return "text/css";
    if (!strcmp(dot, ".js")) return "application/javascript";
    return "application/octet-stream";
}

/* Runs on a libuv worker thread. MUST NOT touch req/res — only ctx. */
static void read_work(void *ud)
{
    read_ctx_t *ctx = ud;
    FILE *f = fopen(ctx->path, "rb");
    if (!f) {
        ctx->status = (errno == ENOENT)  ? 404
                      : (errno == EACCES) ? 403
                                          : 500;
        return;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        ctx->status = 404;
        return;
    }
    if ((size_t)st.st_size > MAX_FILE_BYTES) {
        fclose(f);
        ctx->status = 413;
        return;
    }
    ctx->data = malloc((size_t)st.st_size);
    if (!ctx->data) {
        fclose(f);
        ctx->status = 500;
        return;
    }
    size_t n = fread(ctx->data, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) {
        free(ctx->data);
        ctx->data = NULL;
        ctx->status = 500;
        return;
    }
    ctx->len = n;
    ctx->status = 200;
}

/* Back on the loop thread — safe to touch res. */
static void read_done(void *ud)
{
    read_ctx_t *ctx = ud;
    if (ctx->status == 200) {
        huv_response_header(ctx->res, "Content-Type", ctx->content_type);
        huv_response_status(ctx->res, 200);
        huv_response_send(ctx->res, ctx->data, ctx->len);
    } else {
        const char *msg;
        switch (ctx->status) {
        case 404: msg = "not found\n"; break;
        case 403: msg = "forbidden\n"; break;
        case 413: msg = "file too large\n"; break;
        default:  msg = "read failed\n"; break;
        }
        huv_response_status(ctx->res, ctx->status);
        huv_response_send(ctx->res, msg, strlen(msg));
    }
    free(ctx->data);
    free(ctx->path);
    free(ctx);
}

/* Reject anything that could escape g_serve_root. No slashes, no leading
 * dots — keeps the example focused on the async pattern instead of the
 * path-traversal rabbit hole. */
static int safe_name(const char *name)
{
    if (!name || !*name)
        return 0;
    if (name[0] == '.')
        return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\')
            return 0;
    }
    return 1;
}

static void file_handler(huv_request_t *req, huv_response_t *res,
                         huv_next_fn next)
{
    (void)next;
    const char *name = huv_request_query_param(req, "name");
    if (!safe_name(name)) {
        const char *msg = "need ?name=filename (no slashes, no dotfiles)\n";
        huv_response_status(res, 400);
        huv_response_send(res, msg, strlen(msg));
        return;
    }

    read_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        huv_response_status(res, 500);
        huv_response_send(res, "oom", 3);
        return;
    }
    size_t pathlen = strlen(g_serve_root) + 1 + strlen(name) + 1;
    ctx->path = malloc(pathlen);
    if (!ctx->path) {
        free(ctx);
        huv_response_status(res, 500);
        huv_response_send(res, "oom", 3);
        return;
    }
    snprintf(ctx->path, pathlen, "%s/%s", g_serve_root, name);
    ctx->res = res;
    ctx->content_type = content_type_for(name);

    if (huv_work_submit(req, read_work, read_done, ctx) != 0) {
        free(ctx->path);
        free(ctx);
        huv_response_status(res, 500);
        huv_response_send(res, "submit failed", 13);
    }
}

static void health(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)req;
    (void)next;
    huv_response_status(res, 200);
    huv_response_send(res, "OK", 2);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        g_serve_root = argv[1];

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
    huv_server_get(app, "/file", file_handler);

    fprintf(stderr, "serving files from %s\n", g_serve_root);
    fprintf(stderr, "try: curl 'http://localhost:8080/file?name=FILENAME'\n");

    int rc = huv_server_run(app);
    huv_server_free(app);
    return rc;
}
