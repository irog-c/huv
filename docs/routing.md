# Routing & middleware

## Handler signature

Every handler — middleware or route — takes the same arguments:

```c
typedef void (*huv_handler_fn)(huv_request_t *req,
                                huv_response_t *res,
                                huv_next_fn next);
```

- `req`: parsed request. Accessors are described below.
- `res`: response you write into.
- `next`: only meaningful in middleware — call it to let the chain continue.

## Middleware

`huv_server_use(app, fn)` adds a handler that runs on every request before
the route handler. Up to `HUV_MAX_MIDDLEWARES` are registered in the order
they're added. Typical middleware calls `next(req, res)` to let the chain
continue:

```c
static void logger(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    printf("%s %s\n", huv_request_method(req), huv_request_path(req));
    next(req, res);   /* let the next middleware / the route handler run */
}

huv_server_use(app, logger);
```

A middleware that sends a response and does **not** call `next` short-circuits
the chain — useful for auth gates, rate limiters, etc.

## Route registration

```c
huv_server_get   (app, path, handler);
huv_server_post  (app, path, handler);
huv_server_put   (app, path, handler);
huv_server_delete(app, path, handler);
huv_server_patch (app, path, handler);
huv_server_head  (app, path, handler);
```

All six helpers behave identically except for the method they register under.

### Path syntax

- **Static**: `/users/new`, `/health`
- **Parameterized**: `:name` segments capture one path component.
  `/users/:id` matches `/users/42`; capture `id = "42"`.
  `/users/:id/posts/:pid` matches `/users/alice/posts/99`.

Segments are separated by `/`. A `:param` captures exactly one segment —
`/users/:id` does **not** match `/users/42/extra`, and empty captures like
`/users/` are rejected with 404.

### Precedence

Static routes always beat parameterized ones for the same method + path:

```c
huv_server_get(app, "/users/:id",  user_detail);
huv_server_get(app, "/users/new",  user_new);
/* GET /users/new → user_new, GET /users/42 → user_detail */
```

### 405 Method Not Allowed

If a path is registered for some methods but not the requested one, the
server returns `405` with an `Allow:` header listing the registered methods:

```
HTTP/1.1 405 Method Not Allowed
Allow: GET, POST
```

If the path is not registered at all, the server returns `404`.

## Request accessors

```c
const char *huv_request_method(const huv_request_t *req);  /* "GET", "POST", ... */
const char *huv_request_path(const huv_request_t *req);    /* "/users/42" */
const char *huv_request_query(const huv_request_t *req);   /* raw "a=1&b=2" or "" */
const char *huv_request_body(const huv_request_t *req, size_t *len);

const char *huv_request_param      (const huv_request_t *req, const char *name);
const char *huv_request_query_param(const huv_request_t *req, const char *name);
const char *huv_request_header     (const huv_request_t *req, const char *name);

size_t huv_request_header_count(const huv_request_t *req);
void   huv_request_header_at   (const huv_request_t *req, size_t i,
                                 const char **name, const char **value);
```

Key points:

- Header lookup is **case-insensitive**. Query and param lookup are
  case-sensitive.
- `query_param` URL-decodes `%XX` and `+` → space. Bare keys (no `=`) decode
  to `""`. Missing keys return `NULL`.
- Returned pointers are valid for the lifetime of the request. Copy if you
  need them longer.
- `body` may be `NULL` with `*len = 0` — e.g., GET requests.

## Response API

```c
void huv_response_status(huv_response_t *res, int status);
void huv_response_header(huv_response_t *res, const char *name, const char *value);

/* One-shot: write status + headers + body atomically. */
void huv_response_send(huv_response_t *res, const char *body, size_t len);

/* Or stream: */
void huv_response_write_head(huv_response_t *res);
int  huv_response_write     (huv_response_t *res, const char *data, size_t len);
void huv_response_end       (huv_response_t *res);
```

- `status` defaults to `200` if you don't set it.
- `Content-Type` defaults to `text/plain` unless you set it yourself.
- `Connection` is filled in automatically based on `Connection: keep-alive`
  and the server's `idle_timeout_ms`.
- For **streaming**, if you don't set `Content-Length` before `write_head`,
  the server auto-applies `Transfer-Encoding: chunked`.

See [async.md](async.md) for deferring responses off the loop thread.
