#!/bin/bash
# Master test runner. Builds once, then invokes each test script as a
# subprocess and aggregates exit codes (each script exits with its own
# FAIL_COUNT). Exits non-zero if any script reported a failure.
set -uo pipefail
. "$(dirname "$0")/_common.sh"
build_once

SCRIPTS=(test_basic.sh test_routing.sh test_async.sh test_streaming.sh test_hardening.sh test_tls.sh test_workers.sh test_respawn.sh test_file.sh)
TOTAL_FAILS=0
FAILED_SCRIPTS=()

# SKIP_TESTS is a space-separated list of script basenames to skip.
# sanitize.sh uses this to drop test_respawn.sh (which is incompatible
# with HUV_RESPAWN=0).
for s in "${SCRIPTS[@]}"; do
    if [ -n "${SKIP_TESTS:-}" ] && [[ " $SKIP_TESTS " == *" $s "* ]]; then
        echo
        echo "################################################################"
        echo "# $s  [SKIPPED]"
        echo "################################################################"
        continue
    fi
    echo
    echo "################################################################"
    echo "# $s"
    echo "################################################################"
    bash "$REPO_ROOT/tests/$s"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        TOTAL_FAILS=$((TOTAL_FAILS + rc))
        FAILED_SCRIPTS+=("$s ($rc)")
    fi
done

echo
echo "################################################################"
if [ "$TOTAL_FAILS" -eq 0 ]; then
    echo "# ALL SUITES PASSED"
    exit 0
else
    echo "# $TOTAL_FAILS assertion(s) failed across:"
    for f in "${FAILED_SCRIPTS[@]}"; do echo "#   - $f"; done
    exit 1
fi
