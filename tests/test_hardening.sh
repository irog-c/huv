#!/bin/bash
# Server-wide hardening: oversize body, slowloris timeout, clean SIGINT drain.
# Runs against example_basic — these are infrastructure checks, not feature checks.
set -uo pipefail
. "$(dirname "$0")/_common.sh"
build_once
start_server example_basic

echo "=== 413 on oversize body (2 MiB > max_body_bytes=1 MiB) ==="
CODE=$(dd if=/dev/zero bs=1M count=2 2>/dev/null | \
    curl -sS -X POST --data-binary @- \
         -o /dev/null -w '%{http_code}' \
         http://localhost:8080/echo || true)
[ "$CODE" = "413" ] && pass "oversize POST → 413" || fail "expected 413, got $CODE"

echo
echo "=== slowloris: drip-feed headers over ~15s, request_timeout_ms=10000 ==="
BEFORE=$(grep -c 'timed out in phase' "$SERVER_LOG" || true)
(
    printf 'GET /health HTTP/1.1\r\n'; sleep 3
    printf 'Host: localhost\r\n';       sleep 3
    printf 'X-A: one\r\n';              sleep 3
    printf 'X-B: two\r\n';              sleep 3
    printf 'X-C: three\r\n';            sleep 3
    printf '\r\n'
) | nc -q 0 localhost 8080 >/tmp/slowloris_response.txt 2>&1 || true
AFTER=$(grep -c 'timed out in phase' "$SERVER_LOG" || true)
if [ "$AFTER" -gt "$BEFORE" ]; then
    pass "server logged request-timeout while request was incomplete"
else
    fail "no request-timeout log entry"
fi
BYTES=$(wc -c </tmp/slowloris_response.txt)
[ "$BYTES" -eq 0 ] && pass "server closed before body sent ($BYTES bytes)" \
    || fail "expected 0 response bytes, got $BYTES"
rm -f /tmp/slowloris_response.txt

echo
echo "=== clean SIGINT drain ==="
# Hold a keep-alive connection open, then SIGINT and measure exit time.
(
    exec 3<>/dev/tcp/127.0.0.1/8080
    printf 'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n' >&3
    sleep 5
    exec 3<&-
) &
HOLDER=$!
sleep 0.3

t0=$(date +%s%3N)
kill -INT "$SERVER_PID"
for _ in $(seq 1 100); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then break; fi
    sleep 0.1
done
t1=$(date +%s%3N)
wait "$HOLDER" 2>/dev/null || true

if kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "server did not exit within 10s after SIGINT"
else
    pass "server exited in $((t1 - t0))ms after SIGINT"
fi
# Server already exited — clear SERVER_PID so cleanup trap doesn't try to kill.
SERVER_PID=""

finish
