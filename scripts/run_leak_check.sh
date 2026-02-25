#!/bin/bash
# Run all compliance tests with ASAN leak detection.
# Usage: ./scripts/run_leak_check.sh [test-ir-dir] [test-binary-relative-path]
#   Defaults to /tmp/east-test-ir and packages/east-c/test_compliance
#
# Builds in build-asan/ with AddressSanitizer + LeakSanitizer,
# then runs each compliance test and reports which ones leak.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${PROJECT_DIR}/build-asan"
IR_DIR="${1:-/tmp/east-test-ir}"
TEST_BIN="${BUILD_DIR}/${2:-packages/east-c/test_compliance}"

# Build with ASAN if needed
if [ ! -x "$TEST_BIN" ] || [ "${REBUILD:-}" = "1" ]; then
    echo "Building with AddressSanitizer..."
    mkdir -p "$BUILD_DIR"
    cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
        -DCMAKE_C_FLAGS="-fsanitize=address -g -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
        -DCMAKE_BUILD_TYPE=Debug \
        > /dev/null 2>&1
    cmake --build "$BUILD_DIR" -j"$(nproc)" > /dev/null 2>&1
    echo "Build complete."
fi

if [ ! -x "$TEST_BIN" ]; then
    echo "Error: $(basename "$TEST_BIN") not found at $TEST_BIN"
    exit 1
fi

if [ ! -d "$IR_DIR" ]; then
    echo "Error: IR directory not found at $IR_DIR"
    exit 1
fi

export ASAN_OPTIONS="detect_leaks=1:exitcode=42"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Run all tests in parallel, capturing both stdout and stderr
PIDS=()
FILES=()
for f in "$IR_DIR"/*.json; do
    name=$(basename "$f" .json)
    outfile="$TMPDIR/$name.out"
    errfile="$TMPDIR/$name.err"
    (timeout 60 "$TEST_BIN" "$f" > "$outfile" 2> "$errfile"; echo "EXIT:$?" >> "$outfile") &
    PIDS+=($!)
    FILES+=("$outfile")
done

# Wait for all
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done

# Collect results
LEAK_COUNT=0
CLEAN_COUNT=0
ERROR_COUNT=0
LEAK_SUMMARY=""
CLEAN_SUMMARY=""
ERROR_SUMMARY=""

for f in "$IR_DIR"/*.json; do
    name=$(basename "$f" .json)
    outfile="$TMPDIR/$name.out"
    errfile="$TMPDIR/$name.err"

    if [ ! -f "$outfile" ]; then
        ERROR_SUMMARY+="  MISSING  $name\n"
        ERROR_COUNT=$((ERROR_COUNT + 1))
        continue
    fi

    exit_line=$(grep "^EXIT:" "$outfile" 2>/dev/null | tail -1)
    exit_code="${exit_line#EXIT:}"

    # Check for ASAN leak report in stderr
    has_leak=false
    leak_bytes=""
    if [ -f "$errfile" ] && grep -q "LeakSanitizer" "$errfile" 2>/dev/null; then
        has_leak=true
        # Extract total leaked bytes
        leak_bytes=$(grep -oP 'SUMMARY:.*?(\d+ byte)' "$errfile" 2>/dev/null | head -1)
    fi

    # Check for other ASAN errors (heap-use-after-free, etc.)
    has_asan_error=false
    if [ -f "$errfile" ] && grep -q "ERROR: AddressSanitizer" "$errfile" 2>/dev/null; then
        has_asan_error=true
    fi

    # Exit code 42 = ASAN leak detected
    if [ "$exit_code" = "42" ]; then
        has_leak=true
    fi

    if $has_asan_error && ! $has_leak; then
        asan_type=$(grep -oP 'ERROR: AddressSanitizer: \K\S+' "$errfile" 2>/dev/null | head -1)
        ERROR_SUMMARY+="  ASAN   $name ($asan_type)\n"
        ERROR_COUNT=$((ERROR_COUNT + 1))
    elif $has_leak; then
        LEAK_SUMMARY+="  LEAK   $name ($leak_bytes)\n"
        LEAK_COUNT=$((LEAK_COUNT + 1))
    else
        CLEAN_SUMMARY+="  CLEAN  $name\n"
        CLEAN_COUNT=$((CLEAN_COUNT + 1))
    fi
done

echo "========================================="
echo "  East-C Memory Leak Check"
echo "========================================="
echo ""
if [ -n "$ERROR_SUMMARY" ]; then
    echo -e "$ERROR_SUMMARY" | sort
fi
if [ -n "$LEAK_SUMMARY" ]; then
    echo -e "$LEAK_SUMMARY" | sort
fi
if [ -n "$CLEAN_SUMMARY" ]; then
    echo -e "$CLEAN_SUMMARY" | sort
fi
echo ""
echo "========================================="
echo "  Total: $CLEAN_COUNT clean, $LEAK_COUNT leaking, $ERROR_COUNT errors"
echo "========================================="
