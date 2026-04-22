# Sanitizers

AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan),
ThreadSanitizer (TSan), and MemorySanitizer (MSan) are compiler
instrumentation passes that catch memory-safety bugs at runtime. They
need a rebuild with the right flags, but then exercise the server via
the normal test suite and the violation shows up with a full backtrace.

## Quick start

```bash
bash scripts/sanitize.sh              # ASan + UBSan (recommended)
bash scripts/sanitize.sh thread       # TSan
HUV_SANITIZE=memory bash scripts/sanitize.sh   # MSan — see caveat
```

The script configures a sibling build tree (`build-address-undefined/`,
`build-thread/`, …) with `RelWithDebInfo`, rebuilds everything
(including the FetchContent deps `llhttp` and `mbedtls`), then runs the
full test suite against it. A violation aborts the offending process;
the test runner sees a non-zero exit and fails loudly.

To target one test instead of the whole suite:

```bash
bash scripts/sanitize.sh address,undefined tests/test_async.sh
```

## What each mode catches

| Mode                 | Flag                            | Catches                                                                                   |
| -------------------- | ------------------------------- | ----------------------------------------------------------------------------------------- |
| `address`            | `-fsanitize=address`            | heap buffer overflow, UAF, double-free, stack-use-after-return; LSan catches leaks at exit |
| `undefined`          | `-fsanitize=undefined`          | signed overflow, null deref, misaligned pointer, shift out of range, VLA bound violation  |
| `thread`             | `-fsanitize=thread`             | data races and some deadlock patterns                                                      |
| `memory`             | `-fsanitize=memory`             | reads of uninitialised memory                                                              |

ASan and MSan are mutually exclusive (both replace the allocator).
`address,undefined` is the common combo and what `sanitize.sh` uses by
default.

## How it's wired through CMake

`CMakeLists.txt` exposes one option:

```cmake
set(HUV_SANITIZE "" CACHE STRING
    "Sanitizers to enable (comma list): address, undefined, thread, memory")
```

When set, the flags are injected via `add_compile_options` +
`add_link_options` **before** `FetchContent_MakeAvailable`. That
placement matters: FetchContent-fetched projects inherit compile flags
from the calling directory, so `llhttp` and `mbedtls` get rebuilt with
the same instrumentation as our own code. Without that, ASan would miss
violations inside a dep — or, worse for MSan, fire on reads of memory
that an uninstrumented dep legitimately wrote.

The system `libuv` (from `libuv1-dev`) is **not** rebuilt and is not
instrumented. This is fine for ASan/UBSan (they work on
compiler-inserted checks at callers) but a dealbreaker for MSan. See
the MSan caveat below.

## Multi-worker propagation: the `HUV_RESPAWN` override

The library's default behavior in `src/server.c` is to respawn a worker
that exits abnormally — correct for production, wrong for sanitizer
runs:

```
worker A has heap overflow
  → ASan calls abort(), A dies with SIGABRT
  → master sees abnormal exit, forks replacement worker B
  → master's any_fail stays clear ("recovered")
  → test sends SIGINT → master exits 0
  → test passes ✗
```

To fix this, `run_master` honors `HUV_RESPAWN=0` (accepts `0`, `false`,
`off`, `no`, case-insensitive) and disables respawn regardless of
`cfg.respawn_workers`. `sanitize.sh` sets it unconditionally:

```bash
export HUV_RESPAWN=0
```

Now:

```
worker A has heap overflow
  → ASan calls abort(), A dies with SIGABRT
  → master sees abnormal exit, respawn disabled
  → any_fail = 1
  → master returns -1 (exit 255)
  → test fails ✓
```

One consequence: `tests/test_respawn.sh` explicitly asserts that the
master respawns a SIGKILLed worker, which contradicts the override.
`sanitize.sh` exports `SKIP_TESTS=test_respawn.sh`; `tests/test_all.sh`
honors the variable and prints `[SKIPPED]` for listed scripts.

## Runtime options

`sanitize.sh` sets opinionated defaults so violations abort promptly and
symbols resolve:

```bash
ASAN_OPTIONS="halt_on_error=1:detect_leaks=1:abort_on_error=1:
              strict_string_checks=1:detect_stack_use_after_return=1"
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:abort_on_error=1"
TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1"
MSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stats=1"
```

If `llvm-symbolizer` is on `$PATH`, it's used to resolve addresses to
`function + file:line`. Install `llvm` if your traces show raw hex
offsets.

## MSan caveat

`memory` sanitizer reports a violation on any read of memory that wasn't
written by instrumented code. The path from a client request to our
handler looks roughly like:

```
epoll_wait (kernel)
  → libuv uv__io_poll        ← uninstrumented (system libuv)
  → libuv uv_read_cb buffer  ← uninstrumented write
  → on_read in src/conn.c    ← instrumented READ  ← MSan fires here
```

Every single request will trip MSan because libuv is writing into a
buffer that MSan considers "never initialised." To use MSan seriously,
rebuild libuv from source with `-fsanitize=memory` and point
pkg-config at it:

```bash
git clone https://github.com/libuv/libuv
cd libuv && sh autogen.sh
CFLAGS="-fsanitize=memory -fno-omit-frame-pointer -g -O1" \
    LDFLAGS="-fsanitize=memory" \
    ./configure --prefix=$HOME/local/libuv-msan
make && make install
PKG_CONFIG_PATH=$HOME/local/libuv-msan/lib/pkgconfig \
    bash scripts/sanitize.sh memory
```

Routine coverage is `address,undefined` — stick to that unless you
specifically suspect an uninitialized-read bug.

## Relationship to valgrind

Valgrind (`bash tests/test_valgrind.sh`) and ASan overlap on the leak /
UAF / overflow categories but differ in trade-offs:

| Property           | ASan                              | valgrind                               |
| ------------------ | --------------------------------- | -------------------------------------- |
| Slowdown           | ~2×                               | ~20–30×                                |
| Requires rebuild   | yes                               | no                                     |
| Instruments deps   | only if rebuilt with `-fsanitize` | all deps including system libs         |
| Stack overflows    | yes                               | partial                                |
| Data races         | no (need TSan)                    | yes (via `--tool=helgrind`)            |

Use ASan for the fast feedback loop during development. Use valgrind as
a second check, especially for deps we don't rebuild (libuv).
