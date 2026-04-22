# Async work

Handlers run on the libuv loop thread. **Blocking there stalls every other
connection in the same worker** — a 100 ms synchronous DB query means no
other request on that worker advances for 100 ms. The library exposes two
primitives for deferring work off-loop.

## When to use each

| If your work is…                                | Use                        |
| ----------------------------------------------- | -------------------------- |
| A known delay (timeout, rate limit, retry)      | `huv_timer_defer`         |
| I/O the loop can't drive (DB client, file read) | `huv_work_submit`         |
| CPU-bound (hashing, compression, big math)      | `huv_work_submit`         |
| Non-blocking I/O already on libuv               | just do it on the loop     |

## Pattern: deferred response

A handler may return **without** sending a response — provided it eventually
calls `huv_response_send` on the same `res`, even if the client disconnects
in the meantime (`send` is a safe no-op in that case).

```c
static void on_deadline(void *ud)
{
    huv_response_t *res = ud;
    huv_response_status(res, 200);
    huv_response_send(res, "done\n", 5);
}

static void slow(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)next;
    if (huv_timer_defer(req, 1000, on_deadline, res) != 0) {
        huv_response_status(res, 500);
        huv_response_send(res, "defer failed", 12);
    }
}
```

`huv_timer_defer` fires on the loop thread, so `on_deadline` can touch
`res` freely.

## Pattern: blocking work on a pool thread

```c
typedef struct { huv_response_t *res; unsigned long result; } ctx_t;

static void do_work(void *ud) {
    ctx_t *c = ud;
    /* Runs on a libuv worker thread. MUST NOT touch req/res here. */
    c->result = compute_heavy_thing();
}

static void work_done(void *ud) {
    ctx_t *c = ud;
    /* Back on the loop thread — safe to touch res. */
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "result=%lu\n", c->result);
    huv_response_status(c->res, 200);
    huv_response_send(c->res, buf, (size_t)n);
    free(c);
}

static void work_handler(huv_request_t *req, huv_response_t *res,
                         huv_next_fn next)
{
    (void)next;
    ctx_t *c = malloc(sizeof(*c));
    c->res = res;
    if (huv_work_submit(req, do_work, work_done, c) != 0) {
        free(c);
        huv_response_status(res, 500);
        huv_response_send(res, "submit failed", 13);
    }
}
```

### Work-pool rules

- `work_cb` runs on a libuv thread-pool thread. It **must not** touch
  `huv_request_t` / `huv_response_t` — only its own `userdata` and any
  thread-safe external state.
- `done_cb` runs on the loop thread. It may call `huv_response_send` or do
  any normal request work.
- Thread-pool size is controlled by the standard libuv env var
  `UV_THREADPOOL_SIZE` (default 4).

## Lifetime caveats

- `req` and `res` stay valid as long as the response hasn't finished. They
  survive the original handler returning — that's the whole point.
- The client may disconnect while your timer / work is in flight. When you
  finally call `huv_response_send`, the library detects the closed conn
  and silently drops the write. You do **not** need to check beforehand.
- If you call `huv_timer_defer` or `huv_work_submit` and it returns `-1`
  (allocation failure), the callback will not fire — you still own the
  response and must send something yourself.

See `examples/async.c` and `tests/test_async.sh` for a full working
demo that also proves the loop stays responsive while work is offloaded.
`examples/file.c` uses the same pattern for a practical case: reading a
file from disk on a pool thread and sending its bytes back once the read
completes.
