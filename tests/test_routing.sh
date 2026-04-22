#!/bin/bash
set -uo pipefail
. "$(dirname "$0")/_common.sh"
build_once
start_server example_routing

echo "=== path params ==="
R=$(curl -s http://localhost:8080/users/42)
[ "$R" = "user=42" ] && pass "/users/42 → 'user=42'" || fail "got '$R'"

R=$(curl -s http://localhost:8080/users/alice/posts/99)
[ "$R" = "user=alice post=99" ] && pass "/users/alice/posts/99 → 'user=alice post=99'" \
    || fail "got '$R'"

CODE=$(curl -s -o /dev/null -w '%{http_code}' http://localhost:8080/users/42/extra)
[ "$CODE" = "404" ] && pass "/users/42/extra correctly 404s (no over-consuming match)" \
    || fail "expected 404, got $CODE"

CODE=$(curl -s -o /dev/null -w '%{http_code}' http://localhost:8080/users/)
[ "$CODE" = "404" ] && pass "/users/ correctly 404s (empty param rejected)" \
    || fail "expected 404, got $CODE"

echo
echo "=== method helpers (GET/POST/PUT/DELETE/PATCH/HEAD on /item) ==="
for m in GET POST PUT DELETE PATCH; do
    BODY=$(curl -s -X "$m" http://localhost:8080/item)
    if [ "$BODY" = "method=$m" ]; then
        pass "$m /item dispatched → 'method=$m'"
    else
        fail "$m /item expected 'method=$m', got '$BODY'"
    fi
done
# HEAD must return no body but correct status + headers.
HDR=$(curl -s -I http://localhost:8080/item | tr -d '\r')
CODE=$(echo "$HDR" | head -1 | awk '{print $2}')
[ "$CODE" = "200" ] && pass "HEAD /item → 200 (no body)" || fail "HEAD /item expected 200, got $CODE"

echo
echo "=== 405 Method Not Allowed + Allow header ==="
HDR=$(curl -s -i -X POST http://localhost:8080/users/42 | tr -d '\r')
CODE=$(echo "$HDR" | head -1 | awk '{print $2}')
ALLOW=$(echo "$HDR" | grep -i '^Allow:' | head -1 | sed 's/^[Aa]llow: *//')
if [ "$CODE" = "405" ] && [ "$ALLOW" = "GET" ]; then
    pass "POST /users/42 → 405 with Allow: GET (param route)"
else
    fail "expected 405 / Allow: GET, got code=$CODE allow=$ALLOW"
fi

# /item is registered on every method → Allow must list all of them.
# An unregistered method like OPTIONS should return 405.
HDR=$(curl -s -i -X OPTIONS http://localhost:8080/item | tr -d '\r')
CODE=$(echo "$HDR" | head -1 | awk '{print $2}')
ALLOW=$(echo "$HDR" | grep -i '^Allow:' | head -1 | sed 's/^[Aa]llow: *//')
if [ "$CODE" = "405" ] && [ "$ALLOW" = "GET, POST, PUT, DELETE, PATCH, HEAD" ]; then
    pass "OPTIONS /item → 405 with Allow listing all registered methods"
else
    fail "expected 405 / Allow: GET, POST, PUT, DELETE, PATCH, HEAD — got code=$CODE allow='$ALLOW'"
fi

CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://localhost:8080/nope)
[ "$CODE" = "404" ] && pass "unknown path still 404 (not 405)" || fail "expected 404, got $CODE"

finish
