#!/bin/bash
# Benchmark runs against example_tls (plain HTTP on 8080, HTTPS on 8443 sharing
# the same router). That lets us compare plain vs. TLS overhead on identical
# routes in a single process. Results are parsed from `ab` output and printed
# as a final summary table.
set -uo pipefail
. "$(dirname "$0")/../tests/_common.sh"
build_once

CERT="$REPO_ROOT/build/tls/server.crt"
KEY="$REPO_ROOT/build/tls/server.key"
[ -s "$CERT" ] && [ -s "$KEY" ] || { echo "missing cert/key" >&2; exit 2; }

start_server example_tls "$CERT" "$KEY"

# One line per run, tab-separated: endpoint, scheme, ka, conc, reqs, rps, ms, failed, transfer
RESULTS_TSV=$(mktemp)
trap 'rm -f "$RESULTS_TSV"; _common_cleanup' EXIT

run_bench() {
    local label="$1" scheme="$2" ka="$3" conc="$4" total="$5" path="$6"
    local url ka_flag=""
    if [ "$scheme" = "https" ]; then
        url="https://localhost:8443${path}"
    else
        url="http://localhost:8080${path}"
    fi
    [ "$ka" = "yes" ] && ka_flag="-k"

    echo
    echo "=== $label: $scheme c=$conc n=$total ka=$ka $path ==="
    # -l: do not flag variable-length responses as failed. ab's length check
    # is unreliable against HTTPS (it occasionally reports all responses as
    # "failed" even when every body is identical).
    local out
    out=$(ab -l $ka_flag -n "$total" -c "$conc" "$url" 2>&1) || true

    local rps ms failed transfer
    rps=$(echo "$out" | awk '/Requests per second:/ {print $4; exit}')
    ms=$(echo "$out"  | awk '/Time per request:/ && /mean\)/ && !/across/ {print $4; exit}')
    failed=$(echo "$out" | awk '/Failed requests:/ {print $3; exit}')
    transfer=$(echo "$out" | awk '/Transfer rate:/ {print $3; exit}')

    echo "  rps=$rps  ms_per_req=$ms  failed=$failed  transfer_KBps=$transfer"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$path" "$scheme" "$ka" "$conc" "$total" \
        "${rps:-?}" "${ms:-?}" "${failed:-?}" "${transfer:-?}" \
        >> "$RESULTS_TSV"
}

# Fast-path overhead (tiny body) — plain vs. HTTPS, with/without keep-alive.
run_bench "fast path, keep-alive"         http  yes 100 20000 /health
run_bench "fast path, keep-alive"         https yes 100 20000 /health
run_bench "fast path, no keep-alive"      http  no  50  5000  /health
run_bench "fast path, no keep-alive"      https no  50  5000  /health

# Slightly warmer path (query-param parsing + larger response).
run_bench "query handler, keep-alive"     http  yes 100 20000 "/hello?who=bench"
run_bench "query handler, keep-alive"     https yes 100 20000 "/hello?who=bench"

echo
echo "========================================================================"
echo "SUMMARY"
echo "========================================================================"
printf '%-20s | %-5s | %-8s | %-5s | %-7s | %-12s | %-10s | %-7s | %-10s\n' \
    "Endpoint" "TLS" "Keep-A" "Conc" "Reqs" "Req/sec" "Time (ms)" "Failed" "KB/sec"
printf -- '---------------------+-------+----------+-------+---------+--------------+------------+---------+-----------\n'
while IFS=$'\t' read -r path scheme ka conc total rps ms failed transfer; do
    tls_label="no"; [ "$scheme" = "https" ] && tls_label="yes"
    printf '%-20s | %-5s | %-8s | %-5s | %-7s | %-12s | %-10s | %-7s | %-10s\n' \
        "$path" "$tls_label" "$ka" "$conc" "$total" "$rps" "$ms" "$failed" "$transfer"
done < "$RESULTS_TSV"
echo

# Optional: tail server log for post-mortem if anything went sideways.
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "!!! server exited during benchmark !!!"
    echo "--- server log ---"
    cat "$SERVER_LOG"
fi
