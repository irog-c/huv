#!/bin/bash
# TLS smoke: example_tls listens on both 8080 (plain) and 8443 (HTTPS),
# sharing one router. We exercise the TLS listener end-to-end, confirm the
# cert presented, and verify that plain HTTP on the same process is
# undisturbed by the TLS path.
set -uo pipefail
. "$(dirname "$0")/_common.sh"
build_once
CERT="$BUILD_DIR/tls/server.crt"
KEY="$BUILD_DIR/tls/server.key"
[ -s "$CERT" ] && [ -s "$KEY" ] || { echo "missing cert/key at $CERT / $KEY" >&2; exit 2; }

start_server example_tls "$CERT" "$KEY"

echo "=== HTTPS /health on :8443 ==="
CODE=$(curl -s --cacert "$CERT" -o /dev/null -w '%{http_code}' \
            https://localhost:8443/health)
[ "$CODE" = "200" ] && pass "GET https://localhost:8443/health → 200" \
    || fail "expected 200, got $CODE"

echo
echo "=== cert presented matches our self-signed CN=localhost ==="
SUBJECT=$(openssl s_client -connect localhost:8443 -servername localhost \
              -showcerts </dev/null 2>/dev/null \
          | openssl x509 -noout -subject 2>/dev/null | tr -d '\n')
echo "peer subject: $SUBJECT"
echo "$SUBJECT" | grep -q "CN *= *localhost" \
    && pass "peer presented CN=localhost cert" \
    || fail "peer subject missing CN=localhost"

echo
echo "=== HTTPS body intact (GET /hello?who=mbedtls) ==="
BODY=$(curl -s --cacert "$CERT" "https://localhost:8443/hello?who=mbedtls")
EXPECTED="hello mbedtls over tls-or-plain"
if echo "$BODY" | grep -q "^$EXPECTED$"; then
    pass "body matched: '$EXPECTED'"
else
    fail "unexpected body: '$BODY'"
fi

echo
echo "=== TLS keep-alive: two requests on one connection ==="
# curl reuses the connection across URLs on one command line. Discard both
# bodies and collect the two huv_code values into a single string.
OUT=$(curl -s --cacert "$CERT" -o /dev/null -o /dev/null -w '%{http_code}\n' \
           https://localhost:8443/health https://localhost:8443/health \
      | tr '\n' ' ' | sed 's/ *$//')
[ "$OUT" = "200 200" ] && pass "two sequential HTTPS requests on one conn → 200 200" \
    || fail "expected '200 200', got '$OUT'"

echo
echo "=== plain HTTP on :8080 still works alongside TLS ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' http://localhost:8080/health)
[ "$CODE" = "200" ] && pass "GET http://localhost:8080/health → 200" \
    || fail "expected 200, got $CODE"

echo
echo "=== plain HTTP to the TLS port is rejected (not decoded as HTTP) ==="
# Sending plain HTTP bytes to 8443 should NOT produce a 200 response. The
# server sees them as a garbage TLS record and closes the conn. curl will
# hit a hard error or receive nothing; either way no 200.
CODE=$(curl -s --max-time 2 -o /dev/null -w '%{http_code}' \
            http://localhost:8443/health || true)
if [ "$CODE" != "200" ]; then
    pass "plain HTTP on TLS port did not get a 200 (got '$CODE')"
else
    fail "plain HTTP on TLS port unexpectedly returned 200"
fi

finish
