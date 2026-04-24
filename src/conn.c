#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void conn_list_insert(huv_server_t *s, huv_conn_t *conn)
{
    conn->prev = NULL;
    conn->next = s->conns_head;
    if (s->conns_head)
        s->conns_head->prev = conn;
    s->conns_head = conn;
    s->num_conns++;
}

static void conn_list_remove(huv_conn_t *conn)
{
    huv_server_t *s = conn->server;
    if (conn->prev)
        conn->prev->next = conn->next;
    else if (s->conns_head == conn)
        s->conns_head = conn->next;
    if (conn->next)
        conn->next->prev = conn->prev;
    conn->prev = conn->next = NULL;
    if (s->num_conns)
        s->num_conns--;
}

static void conn_free_buffers(huv_conn_t *conn)
{
    free(conn->url);
    free(conn->body);
    free(conn->hdr_buf);
    free(conn->param_buf);
    free(conn->query_buf);
    free(conn->res.custom_headers);
    conn->url = NULL;
    conn->body = NULL;
    conn->hdr_buf = NULL;
    conn->param_buf = NULL;
    conn->query_buf = NULL;
    conn->res.custom_headers = NULL;
    conn->url_len = conn->url_cap = 0;
    conn->body_len = conn->body_cap = 0;
    conn->hdr_buf_len = conn->hdr_buf_cap = 0;
    conn->hdr_slot_count = 0;
    conn->param_buf_len = conn->param_buf_cap = 0;
    conn->param_count = 0;
    conn->query_buf_len = conn->query_buf_cap = 0;
    conn->query_slot_count = 0;
    conn->query_parsed = false;
    conn->res.custom_headers_len = conn->res.custom_headers_cap = 0;
}

void huv_conn_ref(huv_conn_t *conn) { conn->refs++; }

void huv_conn_unref(huv_conn_t *conn)
{
    if (--conn->refs > 0)
        return;
    huv_server_t *s = conn->server;
    conn_list_remove(conn);
    conn_free_buffers(conn);
    huv_tls_conn_detach(conn);
    free(conn);
    if (s->shutting_down && s->num_conns == 0 && s->drain_timer_initialized &&
        !uv_is_closing((uv_handle_t *)&s->drain_timer)) {
        uv_timer_stop(&s->drain_timer);
        uv_close((uv_handle_t *)&s->drain_timer, NULL);
    }
}

static void on_handle_closed(uv_handle_t *handle)
{
    huv_conn_unref(handle->data);
}

/* Initiate close of the conn's handles. Does not free — the conn is freed
 * only when refs hits 0 (after all handle close_cbs fire and any pending
 * response/write releases its ref). Safe to call from any callback, safe
 * to call multiple times. */
void huv_conn_close(huv_conn_t *conn)
{
    if (conn->closing)
        return;
    conn->closing = true;

    if (!uv_is_closing((uv_handle_t *)&conn->tcp))
        uv_close((uv_handle_t *)&conn->tcp, on_handle_closed);
    if (!uv_is_closing((uv_handle_t *)&conn->timer)) {
        uv_timer_stop(&conn->timer);
        uv_close((uv_handle_t *)&conn->timer, on_handle_closed);
    }
}

static void on_conn_timeout(uv_timer_t *timer);

static void timer_arm(huv_conn_t *conn, unsigned ms)
{
    uv_timer_stop(&conn->timer);
    if (ms == 0)
        return;
    uv_timer_start(&conn->timer, on_conn_timeout, ms, 0);
}

void huv_conn_set_phase(huv_conn_t *conn, conn_phase_t phase)
{
    conn->phase = phase;
    unsigned ms = 0;
    switch (phase) {
    case PHASE_IDLE:
        ms = conn->server->config.idle_timeout_ms;
        break;
    case PHASE_RECEIVING:
    case PHASE_RESPONDING:
        ms = conn->server->config.request_timeout_ms;
        break;
    }
    timer_arm(conn, ms);
}

static void on_conn_timeout(uv_timer_t *timer)
{
    huv_conn_t *conn = timer->data;
    huv_log(conn->server, HUV_LOG_DEBUG, "connection timed out in phase %d",
            (int)conn->phase);
    huv_conn_close(conn);
}

void huv_conn_reset_request_state(huv_conn_t *conn)
{
    conn->url_len = 0;
    if (conn->url)
        conn->url[0] = '\0';
    conn->body_len = 0;
    if (conn->body)
        conn->body[0] = '\0';
    conn->body_limit_exceeded = false;
    conn->hdr_buf_len = 0;
    conn->hdr_slot_count = 0;
    conn->hdr_state = HDR_NONE;
    conn->param_count = 0;
    conn->param_buf_len = 0;
    conn->query_slot_count = 0;
    conn->query_buf_len = 0;
    conn->query_parsed = false;
    conn->method_buf[0] = '\0';
    conn->path_buf[0] = '\0';
    conn->query_ptr = NULL;
    conn->middleware_idx = 0;
    conn->keep_alive = false;

    free(conn->res.custom_headers);
    conn->res.custom_headers = NULL;
    conn->res.custom_headers_len = 0;
    conn->res.custom_headers_cap = 0;
    conn->res.status = 0;
    conn->res.head_written = false;
    conn->res.ended = false;
    conn->res.chunked = false;

    memset(&conn->req, 0, sizeof(conn->req));
}

static int on_message_begin_cb(llhttp_t *p)
{
    huv_conn_t *conn = p->data;
    huv_conn_set_phase(conn, PHASE_RECEIVING);
    return 0;
}

static int on_url_cb(llhttp_t *p, const char *at, size_t length)
{
    huv_conn_t *conn = p->data;
    return huv_buf_append(&conn->url, &conn->url_len, &conn->url_cap, at,
                          length, HUV_URL_HARD_CAP);
}

static int on_header_field_cb(llhttp_t *p, const char *at, size_t length)
{
    huv_conn_t *c = p->data;
    if (c->hdr_state == HDR_IN_VALUE) {
        /* Previous value complete — terminate it, then start new slot. */
        if (huv_buf_append_nul(&c->hdr_buf, &c->hdr_buf_len, &c->hdr_buf_cap,
                               HUV_HEADERS_HARD_CAP) < 0)
            return -1;
        c->hdr_slots[c->hdr_slot_count - 1].value_len =
            c->hdr_buf_len - 1 - c->hdr_slots[c->hdr_slot_count - 1].value_off;
        c->hdr_state = HDR_NONE;
    }
    if (c->hdr_state == HDR_NONE) {
        if (c->hdr_slot_count >= HUV_MAX_HEADERS)
            return -1;
        header_slot_t *s = &c->hdr_slots[c->hdr_slot_count++];
        s->name_off = c->hdr_buf_len;
        s->name_len = 0;
        s->value_off = 0;
        s->value_len = 0;
        c->hdr_state = HDR_IN_FIELD;
    }
    return huv_buf_append(&c->hdr_buf, &c->hdr_buf_len, &c->hdr_buf_cap, at,
                          length, HUV_HEADERS_HARD_CAP);
}

static int on_header_value_cb(llhttp_t *p, const char *at, size_t length)
{
    huv_conn_t *c = p->data;
    if (c->hdr_state == HDR_IN_FIELD) {
        /* Name complete — terminate, record length, open value. */
        header_slot_t *s = &c->hdr_slots[c->hdr_slot_count - 1];
        s->name_len = c->hdr_buf_len - s->name_off;
        if (huv_buf_append_nul(&c->hdr_buf, &c->hdr_buf_len, &c->hdr_buf_cap,
                               HUV_HEADERS_HARD_CAP) < 0)
            return -1;
        s->value_off = c->hdr_buf_len;
        c->hdr_state = HDR_IN_VALUE;
    }
    return huv_buf_append(&c->hdr_buf, &c->hdr_buf_len, &c->hdr_buf_cap, at,
                          length, HUV_HEADERS_HARD_CAP);
}

static int on_headers_complete_cb(llhttp_t *p)
{
    huv_conn_t *c = p->data;
    if (c->hdr_state == HDR_IN_VALUE) {
        header_slot_t *s = &c->hdr_slots[c->hdr_slot_count - 1];
        if (huv_buf_append_nul(&c->hdr_buf, &c->hdr_buf_len, &c->hdr_buf_cap,
                               HUV_HEADERS_HARD_CAP) < 0)
            return -1;
        s->value_len = c->hdr_buf_len - 1 - s->value_off;
        c->hdr_state = HDR_NONE;
    }
    return 0;
}

static int on_body_cb(llhttp_t *p, const char *at, size_t length)
{
    huv_conn_t *conn = p->data;
    size_t cap = conn->server->config.max_body_bytes;
    if (cap == 0)
        cap = 1u << 20;
    if (huv_buf_append(&conn->body, &conn->body_len, &conn->body_cap, at,
                       length, cap) < 0) {
        conn->body_limit_exceeded = true;
        return -1;
    }
    return 0;
}

static int on_message_complete_cb(llhttp_t *p)
{
    huv_conn_t *conn = p->data;

    conn->keep_alive = llhttp_should_keep_alive(p) != 0;
    if (conn->server->shutting_down)
        conn->keep_alive = false;

    const char *method_name = llhttp_method_name((llhttp_method_t)p->method);
    snprintf(conn->method_buf, sizeof(conn->method_buf), "%s", method_name);

    snprintf(conn->path_buf, sizeof(conn->path_buf), "%s",
             conn->url ? conn->url : "");
    char *q = strchr(conn->path_buf, '?');
    if (q) {
        *q = '\0';
        conn->query_ptr = q + 1;
    } else {
        conn->query_ptr = NULL;
    }

    conn->req.method = conn->method_buf;
    conn->req.path = conn->path_buf;
    conn->req.query = conn->query_ptr;
    conn->req.body = conn->body;
    conn->req.body_len = conn->body_len;
    conn->req.conn = conn;

    conn->res.status = 0;
    conn->res.head_written = false;
    conn->res.ended = false;
    conn->res.chunked = false;
    conn->res.conn = conn;

    conn->middleware_idx = 0;
    huv_conn_set_phase(conn, PHASE_RESPONDING);

    /* Keep the conn alive across the response — the handler may hold res
     * and call huv_response_send later from an async callback. This ref
     * is consumed by huv_response_send (or inherited by the in-flight
     * write, which on_write_end releases). */
    huv_conn_ref(conn);
    huv_dispatch_next(&conn->req, &conn->res);
    return HPE_PAUSED;
}

static void init_parser_settings(huv_conn_t *conn)
{
    llhttp_settings_init(&conn->settings);
    conn->settings.on_message_begin = on_message_begin_cb;
    conn->settings.on_url = on_url_cb;
    conn->settings.on_header_field = on_header_field_cb;
    conn->settings.on_header_value = on_header_value_cb;
    conn->settings.on_headers_complete = on_headers_complete_cb;
    conn->settings.on_body = on_body_cb;
    conn->settings.on_message_complete = on_message_complete_cb;
    llhttp_init(&conn->parser, HTTP_REQUEST, &conn->settings);
    conn->parser.data = conn;
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size,
                         uv_buf_t *buf)
{
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len = buf->base ? suggested_size : 0;
}

void huv_conn_feed_parser(huv_conn_t *conn, const char *data, size_t len)
{
    enum llhttp_errno err = llhttp_execute(&conn->parser, data, len);

    if (err == HPE_PAUSED) {
        llhttp_resume(&conn->parser);
    } else if (err != HPE_OK) {
        bool during_send = conn->pending_writes > 0 || conn->res.head_written;
        if (!during_send) {
            int status = conn->body_limit_exceeded ? 413 : 400;
            const char *msg =
                conn->body_limit_exceeded ? "Payload Too Large" : "Bad Request";
            huv_conn_send_simple_error(conn, status, msg);
        } else {
            conn->should_close_after_write = true;
        }
    }
}

void huv_conn_send_simple_error(huv_conn_t *conn, int status, const char *body)
{
    conn->keep_alive = false;
    conn->res.status = status;
    conn->res.head_written = false;
    conn->res.ended = false;
    conn->res.chunked = false;
    conn->res.conn = conn;
    free(conn->res.custom_headers);
    conn->res.custom_headers = NULL;
    conn->res.custom_headers_len = 0;
    conn->res.custom_headers_cap = 0;
    /* huv_response_send consumes a response-open ref; bump one here since
     * the normal on_message_complete_cb path wasn't taken. */
    huv_conn_ref(conn);
    huv_response_send(&conn->res, body, strlen(body));
}

static void on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf)
{
    huv_conn_t *conn = client->data;

    if (nread > 0) {
        if (conn->tls)
            huv_tls_on_read(conn, buf->base, (size_t)nread);
        else
            huv_conn_feed_parser(conn, buf->base, (size_t)nread);
    } else if (nread < 0) {
        huv_conn_close(conn);
    }

    if (buf->base)
        free(buf->base);
}

static void accept_common(uv_stream_t *listener, int status, bool is_tls)
{
    if (status < 0)
        return;

    huv_server_t *server = listener->data;

    if (server->shutting_down) {
        uv_tcp_t scrap;
        uv_tcp_init(&server->loop, &scrap);
        uv_accept(listener, (uv_stream_t *)&scrap);
        uv_close((uv_handle_t *)&scrap, NULL);
        return;
    }

    if (server->config.max_connections &&
        server->num_conns >= server->config.max_connections) {
        uv_tcp_t *scrap = malloc(sizeof(*scrap));
        if (!scrap)
            return;
        uv_tcp_init(&server->loop, scrap);
        if (uv_accept(listener, (uv_stream_t *)scrap) == 0) {
            huv_log(server, HUV_LOG_WARN,
                    "rejecting connection: at max_connections=%u",
                    server->config.max_connections);
        }
        uv_close((uv_handle_t *)scrap, (uv_close_cb)free);
        return;
    }

    huv_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn)
        return;

    conn->server = server;
    uv_tcp_init(&server->loop, &conn->tcp);
    conn->tcp.data = conn;
    huv_conn_ref(conn); /* tcp handle holds a ref until its close_cb fires */
    uv_timer_init(&server->loop, &conn->timer);
    conn->timer.data = conn;
    huv_conn_ref(conn); /* timer handle holds a ref until its close_cb fires */

    /* Insert before accept so that on_handle_closed can safely
     * conn_list_remove if accept fails and we close the handles below. */
    conn_list_insert(server, conn);

    if (uv_accept(listener, (uv_stream_t *)&conn->tcp) != 0) {
        conn->closing = true;
        uv_close((uv_handle_t *)&conn->tcp, on_handle_closed);
        uv_close((uv_handle_t *)&conn->timer, on_handle_closed);
        return;
    }

    uv_tcp_nodelay(&conn->tcp, 1);
    init_parser_settings(conn);

    if (is_tls && huv_tls_conn_attach(conn) < 0) {
        huv_log(server, HUV_LOG_ERROR, "tls attach failed — closing");
        huv_conn_close(conn);
        return;
    }

    huv_conn_set_phase(conn, PHASE_IDLE);
    uv_read_start((uv_stream_t *)&conn->tcp, alloc_buffer, on_read);
}

void huv_conn_on_accept(uv_stream_t *listener, int status)
{
    accept_common(listener, status, false);
}

void huv_conn_on_accept_tls(uv_stream_t *listener, int status)
{
    accept_common(listener, status, true);
}
