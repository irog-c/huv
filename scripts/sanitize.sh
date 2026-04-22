#!/bin/bash
# Rebuild the project with the requested sanitizers into a sibling build
# directory (default: build-asan/) and run the full test suite against it.
# Sanitizers picked up from the HUV_SANITIZE env var, or the first
# positional argument; default is "address,undefined".
#
# Usage:
#   bash scripts/sanitize.sh                         # ASan + UBSan
#   bash scripts/sanitize.sh thread                  # TSan
#   HUV_SANITIZE=memory bash scripts/sanitize.sh    # MSan (see caveat)
#   bash scripts/sanitize.sh address,undefined tests/test_async.sh
#
# Exit status reflects the test suite. Sanitizer diagnostics print to
# stderr of each test; ASAN_OPTIONS=halt_on_error=1 below makes the
# process exit non-zero on the first violation so assertions surface
# the failure rather than masking it.
#
# MSan caveat: -fsanitize=memory reports *every* read of memory that
# any uninstrumented library wrote. We instrument the FetchContent
# deps (llhttp, mbedtls) — see CMakeLists.txt — but libuv comes from
# the system and is NOT instrumented, so MSan will fire on almost any
# uv_read_cb callback. Treat the `memory` mode as "works in principle"
# rather than "run this in CI." For routine runs, stick with
# address,undefined.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# --- pick sanitizer set --------------------------------------------------
SAN="${HUV_SANITIZE:-}"
EXTRA_ARGS=()
if [ -z "$SAN" ] && [ $# -gt 0 ] && [[ "$1" != /* && "$1" != tests/* ]]; then
    SAN="$1"; shift
fi
SAN="${SAN:-address,undefined}"

# Whatever is left on argv is a list of test scripts to run instead of
# the full suite — handy for targeted sanitizer runs.
EXTRA_ARGS=("$@")

# --- pick build dir ------------------------------------------------------
# One dir per sanitizer set avoids recompiling back and forth, and keeps
# the ordinary `build/` untouched so native test/benchmark runs keep
# working without a rebuild.
# printf avoids the trailing newline that echo adds (which the complement
# tr would otherwise map to a dash).
safe=$(printf '%s' "$SAN" | tr ',' '-' | tr -c 'A-Za-z0-9-' '-')
BUILD_DIR="$REPO_ROOT/build-$safe"

echo "=== sanitizer build ==="
echo "  sanitizers : $SAN"
echo "  build dir  : $BUILD_DIR"
echo

# --- configure + build ---------------------------------------------------
# RelWithDebInfo gives -O2 -g: optimized enough that perf still matters
# while keeping frame info for readable sanitizer traces.
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DHUV_SANITIZE="$SAN" \
    >/dev/null
if ! cmake --build "$BUILD_DIR" -j; then
    echo "!!! sanitizer build failed for SAN=$SAN" >&2
    exit 2
fi

# --- runtime options -----------------------------------------------------
# halt_on_error=1         — first violation aborts, no continue-on-warning
# detect_leaks=1          — ASan's built-in LSan integration
# abort_on_error=1        — produce a core-ish exit so wait picks it up
# print_stacktrace=1      — UBSan's useful trace
# suppressions            — none for now; add here if noisy deps surface
export ASAN_OPTIONS="halt_on_error=1:detect_leaks=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:abort_on_error=1"
export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1"
export MSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stats=1"

# Prefer llvm-symbolizer if present — gives function names + line numbers
# in sanitizer traces instead of raw addresses.
if command -v llvm-symbolizer >/dev/null 2>&1; then
    export ASAN_SYMBOLIZER_PATH="$(command -v llvm-symbolizer)"
    export UBSAN_SYMBOLIZER_PATH="$ASAN_SYMBOLIZER_PATH"
    export MSAN_SYMBOLIZER_PATH="$ASAN_SYMBOLIZER_PATH"
fi

# Tests locate binaries via this var — see tests/_common.sh.
export HUV_BUILD_DIR="$BUILD_DIR"

# Critical for multi-worker sanitizer coverage: the master's default
# behavior is to respawn workers that exit abnormally. That's correct for
# production (one handler bug shouldn't take down the whole service), but
# during a sanitizer run it masks failures — ASan aborts the worker with
# SIGABRT, the master respawns, the master exits 0 on SIGINT, the test
# sees rc=0. HUV_RESPAWN=0 (honored by run_master in src/server.c) turns
# respawn off so any worker abort propagates to a non-zero master exit.
export HUV_RESPAWN=0

# test_respawn.sh deliberately SIGKILLs a worker and asserts the master
# respawns it — that test is incompatible with the env var above. Skip it
# in sanitizer runs; its coverage is orthogonal to memory-safety anyway.
export SKIP_TESTS="test_respawn.sh"

echo
echo "=== running tests against sanitized build ==="
echo "  respawn    : disabled (HUV_RESPAWN=0 — violations must propagate)"
echo "  skipping   : $SKIP_TESTS"
if [ ${#EXTRA_ARGS[@]} -gt 0 ]; then
    # Run only the specified scripts. Each is run in its own shell so one
    # failure doesn't mask the rest.
    rc=0
    for t in "${EXTRA_ARGS[@]}"; do
        echo; echo "### $t ###"
        bash "$t" || rc=$?
    done
    exit "$rc"
else
    # Full suite.
    bash "$REPO_ROOT/tests/test_all.sh"
fi
