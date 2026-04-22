#!/bin/bash
set -uo pipefail
. "$(dirname "$0")/_common.sh"
build_once
start_server example_streaming

echo "=== /count uses chunked encoding ==="
RESP=$(curl -s -i "http://localhost:8080/count?to=5")
if echo "$RESP" | tr -d '\r' | grep -qi "^Transfer-Encoding: chunked$"; then
    pass "Transfer-Encoding: chunked header present"
else
    fail "missing chunked header"
fi

echo
echo "=== /count body is reassembled correctly ==="
curl -s "http://localhost:8080/count?to=5" > /tmp/count5.txt
if [ "$(xxd /tmp/count5.txt)" = "$(printf '1\n2\n3\n4\n5\n' | xxd)" ]; then
    pass "body matches byte-for-byte (1..5)"
else
    fail "unexpected body bytes:"
    xxd /tmp/count5.txt
fi
rm -f /tmp/count5.txt

echo
echo "=== /count?to=1000 — larger stream ==="
# 1-9 = 9*2 bytes, 10-99 = 90*3, 100-999 = 900*4, 1000 = 5 → 3893
BYTES=$(curl -s "http://localhost:8080/count?to=1000" | wc -c)
[ "$BYTES" = "3893" ] && pass "returned expected 3893 bytes" \
    || fail "expected 3893 bytes, got $BYTES"

echo
echo "=== /numbers — atomic bulk send ==="
BYTES=$(curl -s "http://localhost:8080/numbers?n=100" | wc -c)
# 1-9 = 9*2, 10-99 = 90*3, 100 = 4 → 18 + 270 + 4 = 292
[ "$BYTES" = "292" ] && pass "/numbers?n=100 returned expected 292 bytes" \
    || fail "expected 292 bytes, got $BYTES"

finish
