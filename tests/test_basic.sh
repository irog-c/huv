#!/bin/bash
set -uo pipefail
. "$(dirname "$0")/_common.sh"
build_once
start_server example_basic

echo "=== baseline ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' http://localhost:8080/health)
if [ "$CODE" = "200" ]; then pass "GET /health → 200"; else fail "expected 200, got $CODE"; fi

echo
echo "=== request headers: lookup ==="
UA=$(curl -s -H "User-Agent: hardening-test/1.0" http://localhost:8080/whoami)
if [ "$UA" = "ua=hardening-test/1.0" ]; then
    pass "header lookup (case-insensitive match)"
else
    fail "expected 'ua=hardening-test/1.0', got '$UA'"
fi

UA2=$(curl -s -H "user-agent: lc-test" http://localhost:8080/whoami)
if [ "$UA2" = "ua=lc-test" ]; then
    pass "lookup is case-insensitive on header name"
else
    fail "expected 'ua=lc-test', got '$UA2'"
fi

echo
echo "=== request headers: iteration ==="
BODY=$(curl -s -H "X-Custom-A: one" -H "X-Custom-B: two two" http://localhost:8080/headers)
if echo "$BODY" | grep -q "^X-Custom-A: one$" && \
   echo "$BODY" | grep -q "^X-Custom-B: two two$"; then
    pass "iteration returned both custom headers (spaces preserved)"
else
    fail "/headers missing expected lines: $BODY"
fi

echo
echo "=== query string parsing ==="
R=$(curl -s "http://localhost:8080/echo?msg=hello")
[ "$R" = "msg=hello" ] && pass "basic query lookup" || fail "expected 'msg=hello', got '$R'"

R=$(curl -s "http://localhost:8080/echo")
[ "$R" = "msg=(none)" ] && pass "missing key returns NULL" || fail "expected 'msg=(none)', got '$R'"

R=$(curl -s "http://localhost:8080/echo?a=1&msg=two&b=3")
[ "$R" = "msg=two" ] && pass "key found among multiple params" || fail "expected 'msg=two', got '$R'"

R=$(curl -s --get --data-urlencode "msg=hello world/foo!" http://localhost:8080/echo)
[ "$R" = "msg=hello world/foo!" ] && pass "%XX decoding (space, slash, bang)" \
    || fail "expected 'msg=hello world/foo!', got '$R'"

R=$(curl -s "http://localhost:8080/echo?msg=a+b+c")
[ "$R" = "msg=a b c" ] && pass "'+' decoded as space" || fail "expected 'msg=a b c', got '$R'"

R=$(curl -s "http://localhost:8080/echo?msg")
[ "$R" = "msg=" ] && pass "bare key decodes to empty value" || fail "expected 'msg=', got '$R'"

finish
