#include "internal.h"

#include <strings.h>
#include <string.h>

const char *huv_request_method(const huv_request_t *req) { return req->method; }
const char *huv_request_path(const huv_request_t *req) { return req->path; }
const char *huv_request_query(const huv_request_t *req) { return req->query; }
const char *huv_request_body(const huv_request_t *req, size_t *len)
{
    if (len)
        *len = req->body_len;
    return req->body;
}

const char *huv_request_header(const huv_request_t *req, const char *name)
{
    huv_conn_t *conn = req->conn;
    for (size_t i = 0; i < conn->hdr_slot_count; i++) {
        const header_slot_t *s = &conn->hdr_slots[i];
        const char *hname = conn->hdr_buf + s->name_off;
        if (strcasecmp(hname, name) == 0)
            return conn->hdr_buf + s->value_off;
    }
    return NULL;
}

size_t huv_request_header_count(const huv_request_t *req)
{
    return ((huv_conn_t *)req->conn)->hdr_slot_count;
}

void huv_request_header_at(const huv_request_t *req, size_t index,
                            const char **name, const char **value)
{
    huv_conn_t *conn = req->conn;
    if (index >= conn->hdr_slot_count) {
        if (name)
            *name = NULL;
        if (value)
            *value = NULL;
        return;
    }
    const header_slot_t *s = &conn->hdr_slots[index];
    if (name)
        *name = conn->hdr_buf + s->name_off;
    if (value)
        *value = conn->hdr_buf + s->value_off;
}

const char *huv_request_param(const huv_request_t *req, const char *name)
{
    huv_conn_t *conn = req->conn;
    for (size_t i = 0; i < conn->param_count; i++) {
        if (strcmp(conn->params[i].name, name) == 0)
            return conn->param_buf + conn->params[i].value_off;
    }
    return NULL;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL-decode `len` bytes from `in` and append the result + trailing NUL to
 * conn->query_buf. Returns the offset where the decoded string starts, or
 * (size_t)-1 on OOM. */
static size_t query_decode_append(huv_conn_t *conn, const char *in, size_t len)
{
    size_t start = conn->query_buf_len;
    for (size_t i = 0; i < len; i++) {
        char out;
        if (in[i] == '+') {
            out = ' ';
        } else if (in[i] == '%' && i + 2 < len) {
            int hi = hex_nibble(in[i + 1]);
            int lo = hex_nibble(in[i + 2]);
            if (hi < 0 || lo < 0) {
                out = in[i];
            } else {
                out = (char)((hi << 4) | lo);
                i += 2;
            }
        } else {
            out = in[i];
        }
        if (huv_buf_append(&conn->query_buf, &conn->query_buf_len,
                            &conn->query_buf_cap, &out, 1,
                            HUV_URL_HARD_CAP) < 0)
            return (size_t)-1;
    }
    if (huv_buf_append_nul(&conn->query_buf, &conn->query_buf_len,
                            &conn->query_buf_cap, HUV_URL_HARD_CAP) < 0)
        return (size_t)-1;
    return start;
}

static void ensure_query_parsed(huv_conn_t *conn)
{
    if (conn->query_parsed)
        return;
    conn->query_parsed = true;
    const char *q = conn->query_ptr;
    if (!q)
        return;

    while (*q && conn->query_slot_count < HUV_MAX_QUERY_PARAMS) {
        const char *name_start = q;
        while (*q && *q != '=' && *q != '&')
            q++;
        size_t name_len = (size_t)(q - name_start);

        const char *value_start = NULL;
        size_t value_len = 0;
        if (*q == '=') {
            q++;
            value_start = q;
            while (*q && *q != '&')
                q++;
            value_len = (size_t)(q - value_start);
        }

        if (name_len > 0) {
            size_t noff = query_decode_append(conn, name_start, name_len);
            if (noff == (size_t)-1)
                return;
            size_t voff = value_start ? query_decode_append(conn, value_start,
                                                            value_len)
                                      : query_decode_append(conn, "", 0);
            if (voff == (size_t)-1)
                return;
            query_slot_t *s = &conn->query_slots[conn->query_slot_count++];
            s->name_off = noff;
            s->value_off = voff;
        }

        if (*q == '&')
            q++;
    }
}

const char *huv_request_query_param(const huv_request_t *req,
                                     const char *name)
{
    huv_conn_t *conn = req->conn;
    ensure_query_parsed(conn);
    for (size_t i = 0; i < conn->query_slot_count; i++) {
        const char *key = conn->query_buf + conn->query_slots[i].name_off;
        if (strcmp(key, name) == 0)
            return conn->query_buf + conn->query_slots[i].value_off;
    }
    return NULL;
}
