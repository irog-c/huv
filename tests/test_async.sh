#!/bin/bash
set -uo pipefail
. "$(dirname "$0")/_common.sh"
build_once
start_server example_async

echo "=== /slow: completes after ~1s ==="
t0=$(date +%s%3N)
BODY=$(curl -s http://localhost:8080/slow)
t1=$(date +%s%3N)
ELAPSED=$((t1 - t0))
echo "elapsed=${ELAPSED}ms body=\"$BODY\""
if [ "$ELAPSED" -ge 800 ] && [ "$ELAPSED" -le 2000 ]; then
    pass "/slow delay in expected range (800-2000ms)"
else
    fail "/slow delay out of range: ${ELAPSED}ms"
fi

echo
echo "=== async handler orphan: client disconnects before response ==="
# --max-time 0.3 < 1s timer → server's async send fires into closed conn.
# /health must still work afterwards (no corruption).
curl -sS --max-time 0.3 -o /dev/null http://localhost:8080/slow || true
sleep 1.3  # let the orphaned timer fire
CODE=$(curl -s -o /dev/null -w '%{http_code}' http://localhost:8080/health)
[ "$CODE" = "200" ] && pass "/health still 200 after orphaned async send" \
    || fail "/health status after orphan: $CODE"

echo
echo "=== work pool: /sum correct + loop stays responsive ==="
EXPECTED="sum=1250000025000000"
curl -s -o /tmp/sum_response.txt http://localhost:8080/sum &
SUM_PID=$!
sleep 0.05  # make sure /sum is on the worker
hstart=$(date +%s%3N)
curl -s -o /dev/null http://localhost:8080/health
hend=$(date +%s%3N)
HDUR=$((hend - hstart))
echo "/health served in ${HDUR}ms while /sum was on worker"
[ "$HDUR" -lt 100 ] && pass "loop responsive (<100ms) with work offloaded" \
    || fail "/health blocked for ${HDUR}ms"
wait "$SUM_PID"
BODY=$(cat /tmp/sum_response.txt)
[ "$BODY" = "$EXPECTED" ] && pass "/sum returned $EXPECTED" \
    || fail "expected $EXPECTED, got '$BODY'"
rm -f /tmp/sum_response.txt

finish
