#include "internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* The payload buffer is allocated in one block right after this header, so
 * huv_conn_alloc_write returns (char *)(wctx + 1) and submit/free recover
 * the header via ((write_ctx_t *)buf) - 1. One malloc per write, one free. */
typedef struct {
    uv_write_t write_req;
    huv_conn_t *conn;
} write_ctx_t;

char *huv_conn_alloc_write(size_t len)
{
    write_ctx_t *wctx = malloc(sizeof(*wctx) + len);
    if (!wctx)
        return NULL;
    return (char *)(wctx + 1);
}

void huv_conn_free_write(char *buf)
{
    if (!buf)
        return;
    write_ctx_t *wctx = (write_ctx_t *)buf - 1;
    /* FP: analyzer can't see that libuv released ownership of `buf` before
     * any call site reaches this function. */
    free(wctx); /* NOLINT(clang-analyzer-unix.Malloc) */
}

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "";
    }
}

void huv_response_status(huv_response_t *res, int status)
{
    res->status = status;
}

void huv_response_header(huv_response_t *res, const char *name,
                          const char *value)
{
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    size_t add = nlen + 2 + vlen + 2;
    if (res->custom_headers_len + add + 1 > res->custom_headers_cap) {
        size_t newcap =
            res->custom_headers_cap ? res->custom_headers_cap * 2 : 256;
        while (newcap < res->custom_headers_len + add + 1)
            newcap *= 2;
        char *nb = realloc(res->custom_headers, newcap);
        if (!nb)
            return;
        res->custom_headers = nb;
        res->custom_headers_cap = newcap;
    }
    char *p = res->custom_headers + res->custom_headers_len;
    /* FP on both memcpy calls below: buffer is NUL-terminated explicitly at
     * the final assignment (`custom_headers[len] = '\0'`). */
    memcpy(p, name, nlen); /* NOLINT(bugprone-not-null-terminated-result) */
    p += nlen;
    *p++ = ':';
    *p++ = ' ';
    memcpy(p, value, vlen); /* NOLINT(bugprone-not-null-terminated-result) */
    p += vlen;
    *p++ = '\r';
    *p++ = '\n';
    res->custom_headers_len += add;
    res->custom_headers[res->custom_headers_len] = '\0';
}

static bool headers_contain(const char *hdrs, size_t len, const char *name)
{
    if (!hdrs || len == 0)
        return false;
    size_t nlen = strlen(name);
    const char *p = hdrs;
    const char *end = hdrs + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (line_len > nlen && (p[nlen] == ':' || p[nlen] == ' ') &&
            strncasecmp(p, name, nlen) == 0)
            return true;
        if (!nl)
            break;
        p = nl + 1;
    }
    return false;
}

static void on_write_end(uv_write_t *req, int status);

/* Raw uv_write submission — takes ownership of buf on success, applies the
 * write-queue cap, and bumps pending_writes. Used both by the plain-HTTP
 * path below and by tls.c when flushing encrypted bytes.
 *
 * Fast path: when no writes are in flight, try a non-blocking uv_try_write
 * first. On localhost / fast links the kernel accepts the whole buffer
 * synchronously, so we skip the async machinery and free the buffer inline.
 * Partial try_write is rare — we memmove the remainder and fall through to
 * uv_write. A try_write error (other than EAGAIN) is left for uv_write to
 * surface. */
int huv_conn_submit_raw_write(huv_conn_t *conn, char *buf, size_t len)
{
    size_t cap = conn->server->config.max_write_queue_bytes;
    if (cap) {
        size_t queued =
            uv_stream_get_write_queue_size((uv_stream_t *)&conn->tcp);
        if (queued + len > cap) {
            huv_log(conn->server, HUV_LOG_WARN,
                     "write %zu+%zu bytes would exceed "
                     "max_write_queue_bytes=%zu; closing",
                     queued, len, cap);
            return -1;
        }
    }

    if (conn->pending_writes == 0) {
        uv_buf_t b = uv_buf_init(buf, (unsigned int)len);
        int n = uv_try_write((uv_stream_t *)&conn->tcp, &b, 1);
        if (n >= 0 && (size_t)n == len) {
            huv_conn_free_write(buf);
            /* Match on_write_end's finalize condition. The async path runs it
             * from the write callback; with try_write we skipped the callback
             * entirely, so keep-alive reset must happen inline. */
            if (conn->res.ended && conn->pending_writes == 0)
                huv_response_finalize(conn);
            return 0;
        }
        if (n > 0) {
            memmove(buf, buf + n, len - (size_t)n);
            len -= (size_t)n;
        }
        /* n < 0 (EAGAIN or error) → fall through to async write. */
    }

    write_ctx_t *wctx = (write_ctx_t *)buf - 1;
    wctx->conn = conn;
    uv_buf_t buf_out = uv_buf_init(buf, (unsigned int)len);
    int r = uv_write(&wctx->write_req, (uv_stream_t *)&conn->tcp, &buf_out, 1,
                     on_write_end);
    if (r < 0)
        return -1;
    conn->pending_writes++;
    return 0;
}

/* Submit plaintext HTTP bytes to the conn. For plain HTTP, hands straight
 * to the raw writer. For TLS, encrypts first (ssl_write populates the outbox
 * via the send BIO) and then the raw writer pushes the encrypted bytes. */
static int submit_write(huv_conn_t *conn, char *payload, size_t len)
{
    if (conn->tls) {
        int rc = huv_tls_encrypt_and_flush(conn, payload, len);
        huv_conn_free_write(payload); /* encrypted copy lives in outbox */
        return rc;
    }
    return huv_conn_submit_raw_write(conn, payload, len);
}

/* Release the "response open" ref and either close or recycle the conn.
 * Called exactly once per response, when (res.ended && pending_writes==0). */
void huv_response_finalize(huv_conn_t *conn)
{
    int status = conn->res.status == 0 ? 200 : conn->res.status;
    huv_log(conn->server, HUV_LOG_INFO, "%s %s %d",
             conn->method_buf[0] ? conn->method_buf : "?",
             conn->path_buf[0] ? conn->path_buf : "?", status);

    if (conn->should_close_after_write || !conn->keep_alive ||
        conn->server->shutting_down || conn->closing) {
        huv_conn_close(conn); /* no-op if already closing */
    } else {
        huv_conn_reset_request_state(conn);
        huv_conn_set_phase(conn, PHASE_IDLE);
    }
    huv_conn_unref(conn);
}

static void on_write_end(uv_write_t *req, int status)
{
    (void)status;
    write_ctx_t *wctx = (write_ctx_t *)req;
    huv_conn_t *conn = wctx->conn;
    free(wctx); /* single allocation covers both the header and the payload */
    conn->pending_writes--;
    if (conn->res.ended && conn->pending_writes == 0)
        huv_response_finalize(conn);
}

/* Build the status line + user/auto headers + blank-line terminator into a
 * freshly allocated buffer. Returns NULL on OOM. *out_len is set on success. */
static char *build_head_payload(huv_response_t *res, size_t extra,
                                size_t *out_len)
{
    huv_conn_t *conn = res->conn;
    int status = res->status == 0 ? 200 : res->status;
    const char *reason = reason_phrase(status);

    char status_line[128];
    int sl = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n",
                      status, reason);

    char auto_hdrs[256];
    int al = 0;
    if (!headers_contain(res->custom_headers, res->custom_headers_len,
                         "Content-Type"))
        al += snprintf(auto_hdrs + al, sizeof(auto_hdrs) - (size_t)al,
                       "Content-Type: text/plain\r\n");
    if (res->chunked)
        al += snprintf(auto_hdrs + al, sizeof(auto_hdrs) - (size_t)al,
                       "Transfer-Encoding: chunked\r\n");
    al += snprintf(auto_hdrs + al, sizeof(auto_hdrs) - (size_t)al,
                   "Connection: %s\r\n",
                   conn->keep_alive ? "keep-alive" : "close");

    size_t total = (size_t)sl + res->custom_headers_len + (size_t)al + 2 + extra;
    char *p = huv_conn_alloc_write(total);
    if (!p)
        return NULL;
    size_t off = 0;
    memcpy(p + off, status_line, (size_t)sl);
    off += (size_t)sl;
    if (res->custom_headers_len) {
        memcpy(p + off, res->custom_headers, res->custom_headers_len);
        off += res->custom_headers_len;
    }
    memcpy(p + off, auto_hdrs, (size_t)al);
    off += (size_t)al;
    /* FP: this is a fixed-length wire buffer tracked by `off`; there is no
     * NUL terminator to produce. */
    memcpy(p + off, "\r\n", 2); /* NOLINT(bugprone-not-null-terminated-result) */
    off += 2;
    *out_len = off; /* caller fills extra bytes starting at p + off */
    return p;
}

void huv_response_send(huv_response_t *res, const char *body, size_t len)
{
    if (res->head_written)
        return;
    res->head_written = true;
    res->ended = true;

    huv_conn_t *conn = res->conn;

    /* Client disconnect / request timeout may have closed the conn while an
     * async handler was still holding res. Drop the write silently and
     * release the response-open ref so the conn can finally be freed. */
    if (conn->closing) {
        huv_conn_unref(conn);
        return;
    }

    /* send always carries a known body, so pin Content-Length. */
    char cl[48];
    snprintf(cl, sizeof(cl), "%zu", len);
    if (!headers_contain(res->custom_headers, res->custom_headers_len,
                         "Content-Length"))
        huv_response_header(res, "Content-Length", cl);
    res->chunked = false;

    size_t head_off;
    char *payload = build_head_payload(res, len, &head_off);
    if (!payload) {
        conn->should_close_after_write = true;
        huv_response_finalize(conn);
        return;
    }
    if (len)
        memcpy(payload + head_off, body, len);

    if (submit_write(conn, payload, head_off + len) < 0) {
        /* FP: submit_write returning <0 means it did NOT take ownership, so
         * we still own payload and must free it here. */
        huv_conn_free_write(payload); /* NOLINT(clang-analyzer-unix.Malloc) */
        conn->should_close_after_write = true;
        huv_response_finalize(conn);
        return;
    }
    /* on_write_end will finalize when this single write completes. */
}

void huv_response_write_head(huv_response_t *res)
{
    if (res->head_written)
        return;
    res->head_written = true;

    huv_conn_t *conn = res->conn;
    if (conn->closing)
        return; /* ended flag still false; end() will unref */

    bool have_cl = headers_contain(res->custom_headers,
                                   res->custom_headers_len, "Content-Length");
    bool have_te = headers_contain(res->custom_headers,
                                   res->custom_headers_len,
                                   "Transfer-Encoding");
    res->chunked = !have_cl && !have_te;

    size_t head_off;
    char *payload = build_head_payload(res, 0, &head_off);
    if (!payload) {
        conn->should_close_after_write = true;
        huv_conn_close(conn);
        return;
    }
    if (submit_write(conn, payload, head_off) < 0) {
        /* FP: see matching comment in huv_response_send — failure means
         * submit_write did not take ownership. */
        huv_conn_free_write(payload); /* NOLINT(clang-analyzer-unix.Malloc) */
        conn->should_close_after_write = true;
        huv_conn_close(conn);
    }
}

int huv_response_write(huv_response_t *res, const char *data, size_t len)
{
    if (!res->head_written || res->ended || len == 0)
        return 0;
    huv_conn_t *conn = res->conn;
    if (conn->closing)
        return -1;

    size_t total;
    char *payload;
    if (res->chunked) {
        char prefix[24];
        int plen = snprintf(prefix, sizeof(prefix), "%zx\r\n", len);
        total = (size_t)plen + len + 2;
        payload = huv_conn_alloc_write(total);
        if (!payload) {
            huv_conn_close(conn);
            return -1;
        }
        memcpy(payload, prefix, (size_t)plen);
        memcpy(payload + (size_t)plen, data, len);
        memcpy(payload + (size_t)plen + len, "\r\n", 2);
    } else {
        total = len;
        payload = huv_conn_alloc_write(total);
        if (!payload) {
            huv_conn_close(conn);
            return -1;
        }
        memcpy(payload, data, len);
    }

    if (submit_write(conn, payload, total) < 0) {
        huv_conn_free_write(payload);
        huv_conn_close(conn);
        return -1;
    }
    return 0;
}

void huv_response_end(huv_response_t *res)
{
    if (!res->head_written || res->ended)
        return;
    res->ended = true;

    huv_conn_t *conn = res->conn;

    if (conn->closing) {
        /* If writes are still in flight they'll fire on_write_end which
         * triggers finalize. Otherwise finalize inline. */
        if (conn->pending_writes == 0)
            huv_conn_unref(conn); /* response-open ref */
        return;
    }

    if (res->chunked) {
        char *term = huv_conn_alloc_write(5);
        if (!term) {
            conn->should_close_after_write = true;
            if (conn->pending_writes == 0)
                huv_response_finalize(conn);
            return;
        }
        memcpy(term, "0\r\n\r\n", 5);
        if (submit_write(conn, term, 5) < 0) {
            huv_conn_free_write(term);
            conn->should_close_after_write = true;
            if (conn->pending_writes == 0)
                huv_response_finalize(conn);
        }
        /* on success, on_write_end finalizes when the terminator flushes. */
        return;
    }

    /* Non-chunked: no terminator. Finalize when all previous writes drain. */
    if (conn->pending_writes == 0)
        huv_response_finalize(conn);
}
