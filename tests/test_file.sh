#!/bin/bash
# File-read example: serves files from a temp dir using huv_work_submit so
# the blocking read happens on the libuv thread pool. Verifies content
# delivery, content-type mapping, and each of the error codes.
set -uo pipefail
. "$(dirname "$0")/_common.sh"
build_once

SERVE_DIR=$(mktemp -d)
trap 'rm -rf "$SERVE_DIR"; _common_cleanup' EXIT

echo "hello from disk" > "$SERVE_DIR/hello.txt"
echo '{"k":"v"}'       > "$SERVE_DIR/data.json"
# 5 MiB file exceeds the 4 MiB cap baked into the example.
dd if=/dev/zero of="$SERVE_DIR/big.bin" bs=1M count=5 >/dev/null 2>&1

start_server example_file "$SERVE_DIR"

echo
echo "=== GET hello.txt returns contents + text/plain ==="
BODY=$(curl -s 'http://localhost:8080/file?name=hello.txt')
[ "$BODY" = "hello from disk" ] && pass "body matches" || fail "body mismatch: '$BODY'"
CT=$(curl -s -D- 'http://localhost:8080/file?name=hello.txt' -o /dev/null \
     | awk 'tolower($1)=="content-type:" {sub(/\r$/,"",$0); sub(/^[^:]*: /,"",$0); print}')
case "$CT" in
    text/plain*) pass "Content-Type=$CT" ;;
    *) fail "expected text/plain, got '$CT'" ;;
esac

echo
echo "=== GET data.json maps to application/json ==="
CT=$(curl -s -D- 'http://localhost:8080/file?name=data.json' -o /dev/null \
     | awk 'tolower($1)=="content-type:" {sub(/\r$/,"",$0); sub(/^[^:]*: /,"",$0); print}')
[ "$CT" = "application/json" ] && pass "Content-Type=application/json" \
    || fail "expected application/json, got '$CT'"

echo
echo "=== missing file → 404 ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' 'http://localhost:8080/file?name=nope.txt')
[ "$CODE" = "404" ] && pass "404 Not Found" || fail "expected 404, got $CODE"

echo
echo "=== file over 4 MiB cap → 413 ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' 'http://localhost:8080/file?name=big.bin')
[ "$CODE" = "413" ] && pass "413 Payload Too Large" || fail "expected 413, got $CODE"

echo
echo "=== path traversal rejected → 400 ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' 'http://localhost:8080/file?name=../etc/passwd')
[ "$CODE" = "400" ] && pass "400 Bad Request (traversal blocked)" \
    || fail "expected 400, got $CODE"

echo
echo "=== missing name param → 400 ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' 'http://localhost:8080/file')
[ "$CODE" = "400" ] && pass "400 Bad Request (no name)" \
    || fail "expected 400, got $CODE"

echo
echo "=== loop stays responsive while reads are in flight ==="
# Fire 20 reads in parallel; meanwhile a /health hit should return within
# a few ms. Proves the reads are really off-loop. Track curl pids so `wait`
# doesn't also block on the backgrounded server from start_server.
CURL_PIDS=()
for _ in $(seq 1 20); do
    curl -s 'http://localhost:8080/file?name=hello.txt' > /dev/null &
    CURL_PIDS+=($!)
done
t0=$(date +%s%3N)
curl -s http://localhost:8080/health > /dev/null
t1=$(date +%s%3N)
for p in "${CURL_PIDS[@]}"; do wait "$p"; done
DELTA=$((t1 - t0))
if [ "$DELTA" -lt 200 ]; then
    pass "/health responded in ${DELTA}ms while 20 file reads in flight"
else
    fail "/health took ${DELTA}ms — loop may be blocked"
fi

finish
