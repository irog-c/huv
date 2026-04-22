#!/bin/bash
# Valgrind leak-check smoke. Runs example_basic (forced to one worker via
# HUV_WORKERS=1) under valgrind, hits a variety of routes to exercise the
# request/response paths, sends SIGINT for a clean drain+teardown, then
# fails if valgrind reports any definitely- or indirectly-lost bytes.
#
# "Still reachable" and "possibly lost" are not checked — libuv, mbedtls,
# and glibc each hold onto some one-time-init state that is reclaimed by
# the kernel at exit. We only care about leaks from our own allocations,
# which show up as definite or indirect.
#
# NOT run by default from test_all.sh because valgrind makes the server
# 20–30× slower. Invoke it directly when you want a leak audit:
#     bash tests/test_valgrind.sh
set -uo pipefail
. "$(dirname "$0")/_common.sh"

if ! command -v valgrind >/dev/null 2>&1; then
    echo "valgrind not installed — skipping leak check"
    echo "  install with: sudo apt install valgrind   (Debian/Ubuntu)"
    echo "               sudo dnf install valgrind   (Fedora/RHEL)"
    exit 0
fi

build_once

VGLOG_DIR=$(mktemp -d)
SERVER_LOG=$(mktemp)
# Override the default cleanup (which kills SERVER_PID via SIGINT) so we
# don't race valgrind's exit. We handle shutdown ourselves below.
cleanup() {
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -INT "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$VGLOG_DIR" "$SERVER_LOG"
}
trap cleanup EXIT

BIN="$BUILD_DIR/examples/example_basic"
[ -x "$BIN" ] || { echo "missing $BIN (did build_once run?)" >&2; exit 2; }

# Pin to one worker so the valgrind log corresponds 1:1 to the loop thread.
export HUV_WORKERS=1

fuser -k 8080/tcp >/dev/null 2>&1 || true

echo "starting example_basic under valgrind (this is slow — expect ~10s startup)"
valgrind \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect \
    --error-exitcode=42 \
    --log-file="$VGLOG_DIR/vg-%p.log" \
    --trace-children=yes \
    --child-silent-after-fork=yes \
    "$BIN" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
echo "valgrind wrapper pid=$SERVER_PID  vglog=$VGLOG_DIR"

# Valgrind startup is slow — give it up to 30s to become ready.
ready=0
for _ in $(seq 1 300); do
    if curl -fs -o /dev/null http://localhost:8080/health 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "!!! server died during startup" >&2
        break
    fi
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    echo "!!! server didn't become ready under valgrind"
    cat "$SERVER_LOG" | tail -30
    exit 2
fi

echo
echo "=== exercising routes ==="
# Mix of paths to cover: middleware, GET, POST with body, header iteration,
# query decoding, 404 miss.
curl -s http://localhost:8080/health > /dev/null
curl -s http://localhost:8080/whoami > /dev/null
curl -s 'http://localhost:8080/echo?msg=hello%20world' > /dev/null
curl -s -X POST http://localhost:8080/echo -d 'message body' > /dev/null
curl -s -H 'X-Custom: yes' -H 'X-Another: no' http://localhost:8080/headers > /dev/null
curl -s http://localhost:8080/missing > /dev/null
# Keep-alive reuse exercises the recycle path.
curl -s http://localhost:8080/health http://localhost:8080/whoami > /dev/null

echo "=== SIGINT + graceful drain ==="
kill -INT "$SERVER_PID"
wait "$SERVER_PID"
RC=$?
SERVER_PID=""
echo "valgrind exit code: $RC (42 = leak/error detected, 0 = clean)"

echo
echo "=== valgrind summary ==="
# One log per process. With HUV_WORKERS=1 there is no fork — the calling
# process runs the loop directly — so we get exactly one log. With workers
# > 1 we'd get one master + one per worker.
shopt -s nullglob
logs=("$VGLOG_DIR"/vg-*.log)
if [ "${#logs[@]}" -eq 0 ]; then
    fail "no valgrind logs produced"
    finish
fi

for f in "${logs[@]}"; do
    pid=$(basename "$f" .log | sed 's/vg-//')
    # When there are no leaks at all, valgrind prints
    #   "All heap blocks were freed -- no leaks are possible"
    # instead of the per-kind breakdown. Treat that as def=ind=reach=0.
    if grep -q "no leaks are possible" "$f"; then
        def=0; ind=0; reach=0
    else
        def=$(awk -F'[:,]' '/definitely lost:/ {gsub(/[^0-9]/,"",$2); print $2; exit}' "$f")
        ind=$(awk -F'[:,]' '/indirectly lost:/ {gsub(/[^0-9]/,"",$2); print $2; exit}' "$f")
        reach=$(awk -F'[:,]' '/still reachable:/ {gsub(/[^0-9]/,"",$2); print $2; exit}' "$f")
    fi
    errs=$(awk '/ERROR SUMMARY:/ {print $4; exit}' "$f")
    printf 'pid=%-7s  definitely=%-6s  indirectly=%-6s  still_reachable=%-6s  errors=%s\n' \
        "$pid" "${def:-?}" "${ind:-?}" "${reach:-?}" "${errs:-?}"

    if [ -n "${def:-}" ] && [ "$def" != "0" ]; then
        fail "pid $pid: $def bytes definitely lost"
        echo "--- leak sites (pid $pid) ---"
        grep -A 15 "definitely lost in" "$f" | head -60 | sed 's/^/  /'
    fi
    if [ -n "${ind:-}" ] && [ "$ind" != "0" ]; then
        fail "pid $pid: $ind bytes indirectly lost"
    fi
    if [ -n "${errs:-}" ] && [ "$errs" != "0" ]; then
        fail "pid $pid: $errs valgrind errors"
        echo "--- error sites (pid $pid) ---"
        grep -B 1 -A 10 "Invalid\|uninitialised\|mismatched" "$f" | head -60 | sed 's/^/  /'
    fi
done

if [ "$FAIL_COUNT" -eq 0 ]; then
    pass "no definite or indirect leaks; no valgrind errors"
fi

finish
