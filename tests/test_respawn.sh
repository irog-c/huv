#!/bin/bash
# Respawn smoke: example_workers forks one worker per core. SIGKILL one of
# them and verify the master forks a replacement and traffic keeps flowing.
# Then verify a clean SIGINT still returns 0 (respawn recovered, no failure).
set -uo pipefail
. "$(dirname "$0")/_common.sh"
build_once
start_server example_workers

echo
echo "=== master has workers ==="
CHILDREN_BEFORE=$(pgrep -P "$SERVER_PID" | sort)
N_BEFORE=$(echo "$CHILDREN_BEFORE" | wc -l)
echo "master $SERVER_PID has $N_BEFORE workers: $(echo $CHILDREN_BEFORE | tr '\n' ' ')"
if [ "$N_BEFORE" -gt 1 ]; then
    pass "multi-worker server ($N_BEFORE workers)"
else
    fail "expected >1 worker, got $N_BEFORE"
fi

echo
echo "=== SIGKILL one worker → master respawns it ==="
VICTIM=$(echo "$CHILDREN_BEFORE" | head -1)
echo "killing worker pid=$VICTIM with SIGKILL"
kill -KILL "$VICTIM"

# Give the master up to ~2s to reap + respawn.
for _ in $(seq 1 20); do
    CHILDREN_AFTER=$(pgrep -P "$SERVER_PID" | sort)
    N_AFTER=$(echo "$CHILDREN_AFTER" | wc -l)
    if [ "$N_AFTER" -eq "$N_BEFORE" ] && ! echo "$CHILDREN_AFTER" | grep -qx "$VICTIM"; then
        break
    fi
    sleep 0.1
done

echo "after respawn: $N_AFTER workers: $(echo $CHILDREN_AFTER | tr '\n' ' ')"
if [ "$N_AFTER" -eq "$N_BEFORE" ]; then
    pass "worker count recovered ($N_AFTER)"
else
    fail "worker count did not recover: before=$N_BEFORE after=$N_AFTER"
fi
if echo "$CHILDREN_AFTER" | grep -qx "$VICTIM"; then
    fail "killed pid $VICTIM still in child list"
else
    pass "killed pid $VICTIM is gone, replacement present"
fi

echo
echo "=== traffic still flows ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' http://localhost:8080/health)
[ "$CODE" = "200" ] && pass "GET /health → 200" || fail "expected 200, got $CODE"

# Hit /pid a few times via new connections — we should see the new worker's
# pid (and, statistically, the others).
PIDS=$(for _ in $(seq 1 40); do
    curl -s -H 'Connection: close' http://localhost:8080/pid
done | sed -n 's/^pid=\([0-9]*\).*/\1/p' | sort -u)
echo "pids observed: $(echo $PIDS | tr '\n' ' ')"
N_UNIQUE=$(echo "$PIDS" | wc -l)
if [ "$N_UNIQUE" -gt 1 ]; then
    pass "requests distributed across $N_UNIQUE workers post-respawn"
else
    fail "expected >1 pid post-respawn, got $N_UNIQUE"
fi

echo
echo "=== master log shows respawn line ==="
if grep -q "respawned pid=" "$SERVER_LOG"; then
    pass "master logged respawn event"
    grep "respawned pid=" "$SERVER_LOG" | sed 's/^/  /'
else
    fail "no respawn log line in master output"
fi

echo
echo "=== clean SIGINT → master exits 0 (respawn recovered) ==="
kill -INT "$SERVER_PID"
for _ in $(seq 1 100); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then break; fi
    sleep 0.1
done
wait "$SERVER_PID" 2>/dev/null
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "master exited with rc=0"
else
    fail "master exited with rc=$RC (respawn should clear any_fail)"
fi
SERVER_PID=""

finish
