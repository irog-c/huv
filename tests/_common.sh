# Shared helpers for test scripts.
#
# Usage:
#   . scripts/_common.sh
#   start_server example_basic
#   assert '[ "$(curl -s .../foo)" = "bar" ]' "foo returns bar"
#   finish   # exits with the failure count

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# HUV_BUILD_DIR lets sanitizer / valgrind wrappers point tests at an
# alternate build tree (e.g. build-asan). Path is relative to REPO_ROOT
# unless it's already absolute.
BUILD_DIR="${HUV_BUILD_DIR:-build}"
case "$BUILD_DIR" in
    /*) ;;
    *)  BUILD_DIR="$REPO_ROOT/$BUILD_DIR" ;;
esac

FAIL_COUNT=0
SERVER_PID=""
SERVER_LOG=""

# Build once per shell. Idempotent thanks to cmake's own caching.
build_once() {
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" >/dev/null
    cmake --build "$BUILD_DIR" -j >/dev/null
}

start_server() {
    local example="$1"; shift
    local bin="$BUILD_DIR/examples/$example"
    [ -x "$bin" ] || { echo "missing binary: $bin (did build_once run?)" >&2; exit 2; }

    fuser -k 8080/tcp >/dev/null 2>&1 || true
    fuser -k 8443/tcp >/dev/null 2>&1 || true
    SERVER_LOG=$(mktemp)
    "$bin" "$@" >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    echo "server=$example pid=$SERVER_PID log=$SERVER_LOG"
    trap _common_cleanup EXIT

    # Wait up to ~2s for /health to respond on the plain-HTTP port.
    local i
    for i in $(seq 1 20); do
        curl -fs -o /dev/null http://localhost:8080/health && return 0
        sleep 0.1
    done
    echo "!!! server $example did not become ready within 2s" >&2
    return 1
}

_common_cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -INT "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [ "${PRINT_LOG_ON_EXIT:-0}" = "1" ] && [ -n "$SERVER_LOG" ]; then
        echo
        echo "=== server log ==="
        cat "$SERVER_LOG"
    fi
    [ -n "$SERVER_LOG" ] && rm -f "$SERVER_LOG"
}

pass() { echo "PASS: $1"; }
fail() {
    echo "FAIL: $1"
    FAIL_COUNT=$((FAIL_COUNT + 1))
}

# finish: call at end of script. Exits with $FAIL_COUNT so test_all.sh can aggregate.
finish() {
    if [ "$FAIL_COUNT" -eq 0 ]; then
        echo "-- all assertions passed --"
    else
        echo "-- $FAIL_COUNT assertion(s) failed --"
    fi
    exit "$FAIL_COUNT"
}
