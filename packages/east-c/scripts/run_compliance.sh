#!/bin/bash
# Run all compliance tests in parallel.
# Usage: ./packages/east-c/scripts/run_compliance.sh [test-ir-dir] [test-binary]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/../../.."
BUILD_DIR="${PROJECT_DIR}/build"
IR_DIR="${1:-/tmp/east-test-ir}"
TEST_BIN="${2:-${BUILD_DIR}/packages/east-c/test_compliance}"

if [ ! -x "$TEST_BIN" ]; then
    echo "Error: $(basename "$TEST_BIN") not found at $TEST_BIN"
    echo "Run: make build"
    exit 1
fi

if [ ! -d "$IR_DIR" ]; then
    echo "Error: IR directory not found at $IR_DIR"
    exit 1
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Run all tests in parallel, each writing output to a temp file
PIDS=()
FILES=()
for f in "$IR_DIR"/*.json; do
    name=$(basename "$f" .json)
    outfile="$TMPDIR/$name.out"
    (timeout 30 "$TEST_BIN" "$f" > "$outfile" 2>&1; echo "EXIT:$?" >> "$outfile") &
    PIDS+=($!)
    FILES+=("$outfile")
done

# Wait for all
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done

# Collect results
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_CRASH=0
SUMMARY=""

for f in "$IR_DIR"/*.json; do
    name=$(basename "$f" .json)
    outfile="$TMPDIR/$name.out"

    if [ ! -f "$outfile" ]; then
        SUMMARY+="$name: MISSING\n"
        continue
    fi

    exit_line=$(grep "^EXIT:" "$outfile" 2>/dev/null | tail -1)
    exit_code="${exit_line#EXIT:}"

    result_line=$(grep "^Results:" "$outfile" 2>/dev/null | tail -1)

    if [ -n "$result_line" ]; then
        passed=$(echo "$result_line" | grep -oP '\d+(?=/)')
        total=$(echo "$result_line" | grep -oP '(?<=/)\d+')
        failed=$((total - passed))
        TOTAL_PASS=$((TOTAL_PASS + passed))
        TOTAL_FAIL=$((TOTAL_FAIL + failed))

        if [ "$failed" -eq 0 ]; then
            SUMMARY+="  PASS  $name ($passed/$total)\n"
        else
            SUMMARY+="  FAIL  $name ($passed/$total, $failed failed)\n"
        fi
    elif [ "$exit_code" = "137" ] || [ "$exit_code" = "139" ] || [ "$exit_code" = "134" ]; then
        TOTAL_CRASH=$((TOTAL_CRASH + 1))
        SUMMARY+="  CRASH $name (signal)\n"
    else
        TOTAL_CRASH=$((TOTAL_CRASH + 1))
        SUMMARY+="  ERROR $name (exit $exit_code)\n"
    fi
done

# Print detailed test output (▶, ✔, ✖, ℹ lines) from each file, sorted by name
for f in "$IR_DIR"/*.json; do
    name=$(basename "$f" .json)
    outfile="$TMPDIR/$name.out"
    [ -f "$outfile" ] && grep -E '^[▶✔✖ℹ]|^  [✔✖]' "$outfile"
done

echo ""
echo "========================================="
printf "  Total: %d passed, %d failed, %d crashed\n" "$TOTAL_PASS" "$TOTAL_FAIL" "$TOTAL_CRASH"
echo "========================================="
