#ifndef HUV_INTERNAL_H
#define HUV_INTERNAL_H

#include "huv/server.h"

#include <llhttp.h>
#include <stdbool.h>
#include <stddef.h>
#include <uv.h>

#define HUV_MAX_MIDDLEWARES 32
#define HUV_MAX_ROUTES 64
#define HUV_METHOD_MAX 16
#define HUV_PATH_MAX 1024
#define HUV_URL_HARD_CAP (64u * 1024u)
#define HUV_HEADERS_HARD_CAP (32u * 1024u)
#define HUV_MAX_HEADERS 128
#define HUV_MAX_PARAMS 16
#define HUV_PARAM_NAME_MAX 32
#define HUV_MAX_QUERY_PARAMS 32

typedef enum
{
    HDR_NONE = 0, /* no pending header */
    HDR_IN_FIELD, /* accumulating name chunks into current slot */
    HDR_IN_VALUE, /* accumulating value chunks into current slot */
} hdr_state_t;

typedef struct
{
    size_t name_off, name_len;
    size_t value_off, value_len;
} header_slot_t;

typedef enum
{
    PHASE_IDLE,
    PHASE_RECEIVING,
    PHASE_RESPONDING,
} conn_phase_t;

typedef struct
{
    char path[HUV_PATH_MAX];
    char method[HUV_METHOD_MAX];
    huv_handler_fn handler;
    bool has_param; /* true if path contains any :name segment */
} huv_route_t;

typedef struct
{
    char name[HUV_PARAM_NAME_MAX];
    size_t value_off; /* offset into huv_conn.param_buf */
} param_slot_t;

typedef struct
{
    size_t name_off;  /* offset into huv_conn.query_buf */
    size_t value_off; /* offset into huv_conn.query_buf */
} query_slot_t;

typedef struct huv_tls_ctx huv_tls_ctx_t;
typedef struct huv_tls_conn huv_tls_conn_t;

struct huv_server
{
    huv_server_config_t config;
    uv_loop_t loop;
    uv_tcp_t listener;
    uv_tcp_t tls_listener;
    uv_signal_t sigint;
    uv_signal_t sigterm;
    uv_timer_t drain_timer;

    huv_tls_ctx_t *tls; /* NULL unless TLS configured */

    bool loop_initialized;
    bool listening;
    bool tls_listening;
    bool signals_installed;
    bool drain_timer_initialized;
    bool shutting_down;

    struct huv_conn *conns_head;
    unsigned num_conns;

    huv_handler_fn middlewares[HUV_MAX_MIDDLEWARES];
    int num_middlewares;
    huv_route_t routes[HUV_MAX_ROUTES];
    int num_routes;
};

struct huv_request
{
    const char *method;
    const char *path;
    const char *query;
    const char *body;
    size_t body_len;
    void *conn;
};

struct huv_response
{
    int status;
    char *custom_headers;
    size_t custom_headers_len;
    size_t custom_headers_cap;
    bool head_written; /* status+headers sent, no more header changes */
    bool ended;        /* caller committed to no more writes (send or end) */
    bool chunked;      /* true when using Transfer-Encoding: chunked */
    void *conn;
};

typedef struct huv_conn
{
    uv_tcp_t tcp;
    uv_timer_t timer;
    llhttp_t parser;
    llhttp_settings_t settings;
    huv_server_t *server;

    struct huv_conn *prev;
    struct huv_conn *next;

    conn_phase_t phase;

    char *url;
    size_t url_len, url_cap;
    char *body;
    size_t body_len, body_cap;
    bool body_limit_exceeded;

    /* Headers packed into a single buffer; slots store (name_off, value_off)
     * pairs. Pointers are only resolved at lookup time (post-parse), so
     * buffer reallocs during parsing are safe. */
    char *hdr_buf;
    size_t hdr_buf_len, hdr_buf_cap;
    header_slot_t hdr_slots[HUV_MAX_HEADERS];
    size_t hdr_slot_count;
    hdr_state_t hdr_state;

    char method_buf[HUV_METHOD_MAX];
    char path_buf[HUV_PATH_MAX];
    char *query_ptr;

    /* Path params captured by the matched route. value_off indexes into
     * param_buf, which stores all NUL-terminated values packed back-to-back. */
    param_slot_t params[HUV_MAX_PARAMS];
    size_t param_count;
    char *param_buf;
    size_t param_buf_len, param_buf_cap;

    /* Lazily-parsed query string: filled on first huv_request_query_param
     * call, then cached. query_buf holds decoded NUL-terminated key/value
     * strings; slots index into it. */
    query_slot_t query_slots[HUV_MAX_QUERY_PARAMS];
    size_t query_slot_count;
    char *query_buf;
    size_t query_buf_len, query_buf_cap;
    bool query_parsed;

    huv_request_t req;
    huv_response_t res;

    huv_tls_conn_t *tls; /* NULL on plain-HTTP conns */

    int middleware_idx;
    int pending_writes; /* uv_writes submitted but not yet completed */
    bool should_close_after_write;
    bool keep_alive;
    bool closing;

    /* Ref count for conn lifetime. Initial = 2 (tcp handle + timer handle).
     * +1 while a response is outstanding (from on_message_complete_cb entry
     *   until the response is finalized via on_write_end's last write, or
     *   inline in huv_response_end when there are no pending writes).
     * Each uv_close's on_handle_closed -> conn_unref. */
    int refs;
} huv_conn_t;

/* --- buf.c --- */
int huv_buf_append(char **buf, size_t *len, size_t *cap, const char *data,
                   size_t n, size_t hard_cap);
int huv_buf_append_nul(char **buf, size_t *len, size_t *cap, size_t hard_cap);

/* --- log.c --- */
void huv_log(huv_server_t *s, huv_log_level_t level, const char *fmt, ...);

/* --- conn.c --- */
void huv_conn_ref(huv_conn_t *conn);
void huv_conn_unref(huv_conn_t *conn);
void huv_conn_close(huv_conn_t *conn);
void huv_conn_set_phase(huv_conn_t *conn, conn_phase_t phase);
void huv_conn_reset_request_state(huv_conn_t *conn);
void huv_conn_send_simple_error(huv_conn_t *conn, int status, const char *body);
void huv_conn_on_accept(uv_stream_t *listener, int status);
void huv_conn_on_accept_tls(uv_stream_t *listener, int status);
/* Feeds raw plaintext bytes into the llhttp parser with the same error
 * handling as on_read's plain path. Used by tls.c after decrypting. */
void huv_conn_feed_parser(huv_conn_t *conn, const char *data, size_t len);

/* --- router.c --- */
void huv_dispatch_next(huv_request_t *req, huv_response_t *res);
void huv_router_add(huv_server_t *s, const char *method, const char *path,
                    huv_handler_fn handler);

/* --- response.c --- */
void huv_response_finalize(huv_conn_t *conn);
/* Write buffer allocator. The returned pointer is writable for `len` bytes;
 * the write_ctx header lives right before it and is recovered in submit/free
 * via pointer arithmetic, so every write is one malloc / one free instead of
 * two. On OOM returns NULL. */
char *huv_conn_alloc_write(size_t len);
void huv_conn_free_write(char *buf);
/* Submits a buffer (from huv_conn_alloc_write) as a uv_write on conn->tcp.
 * Takes ownership on success (0); caller must free via huv_conn_free_write
 * on failure (-1). Attempts a non-blocking uv_try_write first when there are
 * no pending writes, avoiding the async path entirely on fast sockets. */
int huv_conn_submit_raw_write(huv_conn_t *conn, char *buf, size_t len);

/* --- tls.c --- */
int huv_tls_ctx_init(huv_server_t *s);
void huv_tls_ctx_free(huv_server_t *s);
int huv_tls_conn_attach(huv_conn_t *conn);
void huv_tls_conn_detach(huv_conn_t *conn);
/* Called by on_read when conn->tls is set. Appends encrypted bytes to inbox,
 * drives handshake + decrypt, feeds plaintext into the parser via
 * huv_conn_feed_parser, and flushes any encrypted output. */
void huv_tls_on_read(huv_conn_t *conn, const char *buf, size_t len);
/* Encrypt plaintext via mbedtls_ssl_write and flush the outbox as a uv_write.
 * Returns 0 on success, -1 on fatal error. Does not take ownership of data. */
int huv_tls_encrypt_and_flush(huv_conn_t *conn, const char *data, size_t len);

#endif
