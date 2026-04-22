#include "internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool path_has_param(const char *path)
{
    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[1] == ':')
            return true;
    }
    return false;
}

/* Check whether `path` matches parameterized `pattern` without capturing.
 * Used to build the 405 Allow header: we need to know "does this path exist
 * under some other method" without mutating the conn's param state. */
static bool path_matches_pattern(const char *pattern, const char *path)
{
    const char *pp = pattern;
    const char *sp = path;
    while (*pp && *sp) {
        if (*pp == '/' && pp[1] == ':') {
            if (*sp != '/')
                return false;
            pp++; sp++;  /* skip '/' */
            pp++;        /* skip ':' */
            while (*pp && *pp != '/')
                pp++;    /* skip name */
            if (*sp == '\0' || *sp == '/')
                return false;  /* empty value segment */
            while (*sp && *sp != '/')
                sp++;    /* skip value */
        } else {
            if (*pp != *sp)
                return false;
            pp++; sp++;
        }
    }
    return *pp == '\0' && *sp == '\0';
}

/* Match `path` against parameterized `pattern`, filling conn->params on
 * success. Returns true on match. On no-match or param-overflow, returns
 * false and leaves conn->params / param_buf untouched for the next try. */
static bool route_match(huv_conn_t *conn, const char *pattern, const char *path)
{
    /* Take a checkpoint so that on failure we rewind cleanly — callers
     * walk routes in order and a failed match must not leak state. */
    size_t saved_count = conn->param_count;
    size_t saved_buf = conn->param_buf_len;

    const char *pp = pattern;
    const char *sp = path;
    while (*pp && *sp) {
        if (*pp == '/' && pp[1] == ':') {
            if (*sp != '/')
                goto fail;
            pp++; sp++; /* skip '/' */
            pp++;       /* skip ':' */

            const char *name = pp;
            while (*pp && *pp != '/')
                pp++;
            size_t name_len = (size_t)(pp - name);

            const char *val = sp;
            while (*sp && *sp != '/')
                sp++;
            size_t val_len = (size_t)(sp - val);

            if (name_len == 0 || val_len == 0)
                goto fail;
            if (name_len >= HUV_PARAM_NAME_MAX)
                goto fail;
            if (conn->param_count >= HUV_MAX_PARAMS)
                goto fail;

            if (huv_buf_append(&conn->param_buf, &conn->param_buf_len,
                                &conn->param_buf_cap, val, val_len,
                                64u * 1024u) < 0)
                goto fail;
            if (huv_buf_append_nul(&conn->param_buf, &conn->param_buf_len,
                                    &conn->param_buf_cap, 64u * 1024u) < 0)
                goto fail;

            param_slot_t *slot = &conn->params[conn->param_count++];
            memcpy(slot->name, name, name_len);
            slot->name[name_len] = '\0';
            slot->value_off = conn->param_buf_len - val_len - 1;
        } else {
            if (*pp != *sp)
                goto fail;
            pp++; sp++;
        }
    }

    if (*pp || *sp)
        goto fail;
    return true;

fail:
    conn->param_count = saved_count;
    conn->param_buf_len = saved_buf;
    if (conn->param_buf && conn->param_buf_cap > saved_buf)
        conn->param_buf[saved_buf] = '\0';
    return false;
}

void huv_dispatch_next(huv_request_t *req, huv_response_t *res)
{
    huv_conn_t *conn = req->conn;
    huv_server_t *server = conn->server;

    if (conn->middleware_idx < server->num_middlewares) {
        huv_handler_fn handler = server->middlewares[conn->middleware_idx++];
        handler(req, res, huv_dispatch_next);
        return;
    }

    /* Static routes always win — look them up first. */
    for (int i = 0; i < server->num_routes; i++) {
        huv_route_t *route = &server->routes[i];
        if (!route->has_param &&
            strcmp(route->method, req->method) == 0 &&
            strcmp(route->path, req->path) == 0) {
            route->handler(req, res, huv_dispatch_next);
            return;
        }
    }

    for (int i = 0; i < server->num_routes; i++) {
        huv_route_t *route = &server->routes[i];
        if (route->has_param &&
            strcmp(route->method, req->method) == 0 &&
            route_match(conn, route->path, req->path)) {
            route->handler(req, res, huv_dispatch_next);
            return;
        }
    }

    /* No handler matched. If the path exists under another method, emit 405
     * with an Allow header listing them; otherwise 404. */
    const char *allowed[HUV_MAX_ROUTES];
    int nallowed = 0;
    for (int i = 0; i < server->num_routes; i++) {
        huv_route_t *route = &server->routes[i];
        bool matches = route->has_param
                           ? path_matches_pattern(route->path, req->path)
                           : strcmp(route->path, req->path) == 0;
        if (!matches)
            continue;
        bool dup = false;
        for (int j = 0; j < nallowed; j++) {
            if (strcmp(allowed[j], route->method) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup)
            allowed[nallowed++] = route->method;
    }

    if (nallowed > 0) {
        char allow[256];
        int off = 0;
        for (int i = 0; i < nallowed; i++)
            off += snprintf(allow + off, sizeof(allow) - (size_t)off, "%s%s",
                            i ? ", " : "", allowed[i]);
        huv_response_header(res, "Allow", allow);
        huv_response_status(res, 405);
        huv_response_send(res, "Method Not Allowed", 18);
        return;
    }

    huv_response_status(res, 404);
    huv_response_send(res, "Not Found", 9);
}

void huv_router_add(huv_server_t *s, const char *method, const char *path,
                     huv_handler_fn handler)
{
    if (s->num_routes >= HUV_MAX_ROUTES)
        return;
    huv_route_t *r = &s->routes[s->num_routes++];
    snprintf(r->method, sizeof(r->method), "%s", method);
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->handler = handler;
    r->has_param = path_has_param(r->path);
}

void huv_server_get(huv_server_t *s, const char *path, huv_handler_fn h)
{
    huv_router_add(s, "GET", path, h);
}

void huv_server_post(huv_server_t *s, const char *path, huv_handler_fn h)
{
    huv_router_add(s, "POST", path, h);
}

void huv_server_put(huv_server_t *s, const char *path, huv_handler_fn h)
{
    huv_router_add(s, "PUT", path, h);
}

void huv_server_delete(huv_server_t *s, const char *path, huv_handler_fn h)
{
    huv_router_add(s, "DELETE", path, h);
}

void huv_server_patch(huv_server_t *s, const char *path, huv_handler_fn h)
{
    huv_router_add(s, "PATCH", path, h);
}

void huv_server_head(huv_server_t *s, const char *path, huv_handler_fn h)
{
    huv_router_add(s, "HEAD", path, h);
}
