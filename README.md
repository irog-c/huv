# huv

A small C HTTP/1.1 server library built on [libuv](https://libuv.org/) and
[llhttp](https://github.com/nodejs/llhttp), with optional TLS via
[mbedTLS](https://www.trustedfirmware.org/projects/mbed-tls/) and optional
multi-worker scaling via `SO_REUSEPORT` + `fork()`.

- **Non-blocking**: single-threaded event loop per worker. Handlers run on
  that loop, so they must not block — use [docs/async.md](docs/async.md) for
  I/O and CPU work.
- **Middleware + routing**: Express-style `use()` + `get/post/...`, with
  `:param` captures and method helpers that produce `405 + Allow` for
  unregistered methods on known paths.
- **HTTP/HTTPS on one process**: plain HTTP, TLS, or both — in the same
  worker, sharing one router.
- **Multi-worker**: set `workers=N` and the library forks N processes that
  share the listen port. The kernel load-balances incoming connections.

## Minimal example

```c
#include "huv/server.h"

static void hello(huv_request_t *req, huv_response_t *res, huv_next_fn next)
{
    (void)req; (void)next;
    huv_response_status(res, 200);
    huv_response_send(res, "hello\n", 6);
}

int main(void)
{
    huv_server_config_t cfg = HUV_SERVER_CONFIG_DEFAULT;
    cfg.port = 8080;

    huv_server_t *app = huv_server_new(&cfg);
    huv_server_get(app, "/", hello);

    int rc = huv_server_run(app);   /* blocks until SIGINT/SIGTERM */
    huv_server_free(app);
    return rc;
}
```

Consume from your own CMake project:

```cmake
add_subdirectory(path/to/huv)            # or FetchContent_Declare/MakeAvailable
add_executable(my_server main.c)
target_link_libraries(my_server PRIVATE huv)
```

Then:

```bash
cmake -S . -B build && cmake --build build -j
./build/my_server &
curl http://localhost:8080/   # → hello
```

## Where to start

| If you want to…                    | Read                                               |
| ---------------------------------- | -------------------------------------------------- |
| Build it and run hello-world       | [docs/getting-started.md](docs/getting-started.md) |
| Understand the request lifecycle   | [docs/architecture.md](docs/architecture.md)       |
| See every knob on the config       | [docs/configuration.md](docs/configuration.md)     |
| Wire up routes + middleware        | [docs/routing.md](docs/routing.md)                 |
| Defer work without blocking        | [docs/async.md](docs/async.md)                     |
| Enable HTTPS                       | [docs/tls.md](docs/tls.md)                         |
| Scale beyond one CPU core          | [docs/workers.md](docs/workers.md)                 |
| Run under ASan/UBSan/TSan/MSan     | [docs/sanitizers.md](docs/sanitizers.md)           |
| Benchmark / tune it                | [docs/performance.md](docs/performance.md)         |

## Status

Library is a learning project — not a hardened production server. The design
goals are: small surface, understandable code, end-to-end shell tests for
everything that ships.

## License

Copyright 2026 Igor Mihajlov. Licensed under the Apache License, Version 2.0 —
see [LICENSE](LICENSE) for the full text.
