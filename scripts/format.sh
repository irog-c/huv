#!/bin/bash
# Run clang-format in place across every C source and header file the
# project owns — that is, src/*.{c,h}, include/huv/*.h, and examples/*.c.
# Third-party trees (build/_deps/llhttp, build/_deps/mbedtls) are not
# touched: they have their own style and we have no reason to churn them.
#
# Configuration lives in .clang-format at the repo root.
#
# Usage:
#     bash scripts/format.sh            # format everything in place
#     bash scripts/format.sh --check    # diff-only: exit 1 if anything
#                                       # would change, do not modify files
#     bash scripts/format.sh src/conn.c # format one or more specific files
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format not installed — skipping"
    echo "  install with: sudo apt install clang-format   (Debian/Ubuntu)"
    echo "               sudo dnf install clang-tools-extra   (Fedora/RHEL)"
    exit 0
fi

# Parse args: --check switches to dry-run mode, otherwise list-of-paths
# (empty ⇒ all own-source files).
CHECK_ONLY=0
FILES=()
for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        -*) echo "unknown flag: $arg" >&2; exit 2 ;;
        *)  FILES+=("$arg") ;;
    esac
done

if [ "${#FILES[@]}" -eq 0 ]; then
    shopt -s nullglob
    FILES=(src/*.c src/*.h include/huv/*.h examples/*.c)
fi

echo "clang-format $(clang-format --version | awk '{print $NF; exit}')"
if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "checking ${#FILES[@]} file(s) (no changes written)"
else
    echo "formatting ${#FILES[@]} file(s) in place"
fi
echo

DRIFT_FILES=()
for f in "${FILES[@]}"; do
    if [ "$CHECK_ONLY" -eq 1 ]; then
        # clang-format -n prints a diagnostic per violation; --Werror makes
        # the presence of any diagnostic an exit-1. We don't need the diag
        # text, just the yes/no answer.
        if ! clang-format -n --Werror "$f" >/dev/null 2>&1; then
            DRIFT_FILES+=("$f")
        fi
    else
        clang-format -i "$f"
    fi
done

echo "=== summary ==="
if [ "$CHECK_ONLY" -eq 1 ]; then
    if [ "${#DRIFT_FILES[@]}" -eq 0 ]; then
        echo "clang-format: all ${#FILES[@]} file(s) already formatted"
        exit 0
    fi
    echo "clang-format: ${#DRIFT_FILES[@]} file(s) need formatting"
    printf '  %s\n' "${DRIFT_FILES[@]}"
    echo
    echo "run without --check to apply:  bash scripts/format.sh"
    exit 1
fi
echo "clang-format: formatted ${#FILES[@]} file(s)"
