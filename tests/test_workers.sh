#!/bin/bash
# Multi-worker smoke: example_workers spawns 4 children sharing :8080 via
# SO_REUSEPORT. Hit /pid many times, expect multiple distinct pids. Then
# SIGINT the master and confirm all children exit cleanly.
set -uo pipefail
. "$(dirname "$0")/_common.sh"
build_once
start_server example_workers

echo "=== /health served by worker ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' http://localhost:8080/health)
[ "$CODE" = "200" ] && pass "GET /health → 200" || fail "expected 200, got $CODE"

echo
echo "=== /pid distribution across workers ==="
# Fire 200 requests; the kernel distributes new connections across the 4
# SO_REUSEPORT sockets, so we should see multiple distinct pids. Use
# --no-keepalive (new connection per request) to give the kernel a chance
# to load-balance. Keep-alive would pin us to a single worker.
PIDS_FILE=$(mktemp)
for _ in $(seq 1 200); do
    curl -s --http1.1 -H 'Connection: close' http://localhost:8080/pid
done > "$PIDS_FILE" 2>/dev/null
UNIQUE=$(sort -u "$PIDS_FILE" | wc -l)
echo "saw $UNIQUE distinct worker pids over 200 requests"
sort "$PIDS_FILE" | uniq -c | sort -rn | head -5
rm -f "$PIDS_FILE"
if [ "$UNIQUE" -gt 1 ]; then
    pass "requests distributed across $UNIQUE workers"
else
    fail "expected >1 distinct pid, got $UNIQUE"
fi

echo
echo "=== master pid is distinct from workers ==="
# SERVER_PID is the master process. Confirm it's not the same as any worker
# pid we observed serving traffic.
WORKER_PID=$(curl -s --http1.1 -H 'Connection: close' http://localhost:8080/pid \
             | sed -n 's/^pid=\([0-9]*\).*/\1/p')
if [ -n "$WORKER_PID" ] && [ "$WORKER_PID" != "$SERVER_PID" ]; then
    pass "master=$SERVER_PID worker=$WORKER_PID (distinct)"
else
    fail "master/worker pid relation unexpected: master=$SERVER_PID worker=$WORKER_PID"
fi

echo
echo "=== clean SIGINT shutdown propagates to workers ==="
# Collect current worker pids (children of master).
CHILDREN=$(pgrep -P "$SERVER_PID" || true)
echo "master $SERVER_PID has children: $CHILDREN"
t0=$(date +%s%3N)
kill -INT "$SERVER_PID"
for _ in $(seq 1 100); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then break; fi
    sleep 0.1
done
t1=$(date +%s%3N)
if kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "master did not exit within 10s of SIGINT"
else
    pass "master exited in $((t1 - t0))ms"
fi

# Verify no child is lingering.
STILL_ALIVE=""
for c in $CHILDREN; do
    if kill -0 "$c" 2>/dev/null; then
        STILL_ALIVE="$STILL_ALIVE $c"
    fi
done
if [ -z "$STILL_ALIVE" ]; then
    pass "all workers exited with master"
else
    fail "lingering workers after master exit:$STILL_ALIVE"
fi
# Master is gone — clear SERVER_PID so cleanup trap doesn't try to kill.
SERVER_PID=""

finish
