# Getting started

## Prerequisites

- CMake ≥ 3.14, a C11 compiler, `pkg-config`
- `libuv` dev headers (Ubuntu: `libuv1-dev`)
- `openssl` CLI (used at build time to generate a self-signed cert for the
  TLS example and TLS smoke tests)

`llhttp` and `mbedTLS` are fetched automatically via CMake's `FetchContent`.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

Products land under `build/`:

- `build/libhuv.a` — the library
- `build/examples/example_*` — one binary per example (`basic`, `routing`,
  `async`, `streaming`, `tls`, `workers`)
- `build/tls/server.{crt,key}` — self-signed cert regenerated only if missing

## Hello world

```c
#include "huv/server.h"
#include <stdio.h>

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
    cfg.log_cb = huv_log_stderr;

    huv_server_t *app = huv_server_new(&cfg);
    if (!app) { fprintf(stderr, "new failed\n"); return 1; }

    huv_server_get(app, "/", hello);

    int rc = huv_server_run(app);      /* blocks until SIGINT/SIGTERM */
    huv_server_free(app);
    return rc;
}
```

Link it against `libhuv.a` + `libuv`:

```
cc main.c -o my_server -Iinclude -Lbuild -lhuv -luv
```

or consume it from CMake:

```cmake
add_subdirectory(path/to/huv)
target_link_libraries(my_target PRIVATE huv)
```

## Running the examples

```bash
./build/examples/example_basic         # :8080 — middleware + headers
./build/examples/example_routing       # :8080 — params + method helpers
./build/examples/example_async         # :8080 — timer + work pool
./build/examples/example_streaming     # :8080 — chunked encoding
./build/examples/example_workers       # :8080 — one worker per core
./build/examples/example_file ./public # :8080 — async file reads via work pool
./build/examples/example_tls \
    build/tls/server.crt build/tls/server.key   # :8080 + :8443
```

Each example is ~40–90 lines and mirrors one concept — good starting points
when wiring the library into your own project.

## Tests & benchmarks

```bash
bash tests/test_all.sh       # all end-to-end assertions
bash tests/test_valgrind.sh  # leak audit (not in test_all — slow)
bash scripts/tidy.sh         # clang-tidy across src/ + examples/
bash scripts/sanitize.sh     # rebuild with ASan+UBSan, run full suite
bash scripts/benchmark.sh    # plain vs HTTPS throughput table
```

`test_valgrind.sh` runs `example_basic` with `HUV_WORKERS=1` under
valgrind (about 20× slower than native), exercises a mix of routes, and
fails if any bytes are definitely or indirectly lost. Requires `valgrind`
on PATH — skips cleanly if missing.

`tidy.sh` runs `clang-tidy` against every C source file the project owns
(`src/*.c`, `examples/*.c`) using `build/compile_commands.json`. Third-
party trees under `build/_deps/` are excluded. The check list lives in
`.clang-tidy` at the repo root — C++-only families are omitted, and
`WarningsAsErrors: '*'` is set so any enabled-check finding fails the
run. Known false positives from the static analyzer are silenced at the
site with `/* NOLINT(check-name) */` plus a `/* FP: … */` comment
explaining why — not via blanket disables. Run
`bash scripts/tidy.sh src/conn.c` to lint one file, or
`bash scripts/tidy.sh --fix` to apply fixits in place.

`sanitize.sh` rebuilds the project (including FetchContent deps) with
the requested sanitizer into a sibling `build-<sanitizer>/` and runs
the full test suite against it. Default is `address,undefined`. Covers
the multi-worker path: `HUV_RESPAWN=0` is exported so a worker aborted
by ASan propagates through the master's exit code instead of being
silently respawned. See [sanitizers.md](sanitizers.md) for details —
each mode, the MSan libuv caveat, and the `HUV_RESPAWN` mechanism.
