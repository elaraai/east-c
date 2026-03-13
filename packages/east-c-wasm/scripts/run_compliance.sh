#!/bin/bash
# Run all WASM compliance tests in parallel.
# Each IR file runs in its own Node process with its own WASM instance.
#
# Usage: ./scripts/run_compliance.sh [test-ir-dir]
#        FILTER=Integer ./scripts/run_compliance.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_DIR="${SCRIPT_DIR}/.."
IR_DIR="${1:-/tmp/east-test-ir}"
TEST_JS="${PKG_DIR}/dist/test/compliance.spec.js"

if [ ! -f "$TEST_JS" ]; then
    echo "Error: compliance.spec.js not found. Run: npm run build"
    exit 1
fi

if [ ! -d "$IR_DIR" ]; then
    echo "Error: IR directory not found at $IR_DIR"
    exit 1
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

PIDS=()
NAMES=()

for f in "$IR_DIR"/*.json; do
    name=$(basename "$f" .json)

    if [ -n "${FILTER:-}" ] && [ "$name" != "$FILTER" ]; then
        continue
    fi

    outfile="$TMPDIR/$name.out"
    (
        FILTER="$name" IR_DIR="$IR_DIR" \
        timeout 60 node --enable-source-maps --test "$TEST_JS" > "$outfile" 2>&1
        echo "EXIT:$?" >> "$outfile"
    ) &

    PIDS+=($!)
    NAMES+=("$name")
done

# Wait for all
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done

# Collect results
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_ERROR=0

for name in "${NAMES[@]}"; do
    outfile="$TMPDIR/$name.out"

    if [ ! -f "$outfile" ]; then
        TOTAL_ERROR=$((TOTAL_ERROR + 1))
        echo "  ERROR $name (missing output)"
        continue
    fi

    # Print test output lines (node:test prefixes console.log with "# ")
    grep -oP '(?<=# )[ ]*[▶✔✘ℹ].*' "$outfile" 2>/dev/null || true

    # Count passes and failures from ℹ lines
    pass=$(grep -oP '(?<=ℹ pass )\d+' "$outfile" 2>/dev/null | tail -1)
    fail=$(grep -oP '(?<=ℹ fail )\d+' "$outfile" 2>/dev/null | tail -1)

    if [ -n "${pass:-}" ]; then
        TOTAL_PASS=$((TOTAL_PASS + pass))
    fi
    if [ -n "${fail:-}" ]; then
        TOTAL_FAIL=$((TOTAL_FAIL + fail))
    fi

    # Check exit code
    exit_line=$(grep "^EXIT:" "$outfile" 2>/dev/null | tail -1)
    exit_code="${exit_line#EXIT:}"
    if [ "${exit_code:-1}" != "0" ] && [ -z "${fail:-}" ]; then
        TOTAL_ERROR=$((TOTAL_ERROR + 1))
        echo "  ERROR $name (exit $exit_code)"
    fi
done

echo ""
echo "========================================="
printf "  Total: %d passed, %d failed, %d errors\n" "$TOTAL_PASS" "$TOTAL_FAIL" "$TOTAL_ERROR"
echo "========================================="
