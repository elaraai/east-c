#!/bin/bash
# Run profile IR files through east-c CLI with ASAN leak detection.
# Usage: ./scripts/run_profiler.sh [profile-ir-dir]
#   Defaults to /tmp/east-profile-ir
#
# Builds in build-asan/ with AddressSanitizer + LeakSanitizer,
# then runs each profile beast2/json IR file and reports results.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${PROJECT_DIR}/build-asan"
IR_DIR="${1:-/tmp/east-profile-ir}"
CLI_BIN="${BUILD_DIR}/packages/east-c-cli/east-c"

# Build with ASAN if needed
if [ ! -x "$CLI_BIN" ] || [ "${REBUILD:-}" = "1" ]; then
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

if [ ! -x "$CLI_BIN" ]; then
    echo "Error: east-c CLI not found at $CLI_BIN"
    exit 1
fi

if [ ! -d "$IR_DIR" ]; then
    echo "Error: Profile IR directory not found at $IR_DIR"
    echo "Generate with: node profile_generator.js --size xlarge (writes to /tmp/east-profile-ir)"
    exit 1
fi

export ASAN_OPTIONS="detect_leaks=1:exitcode=42"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Collect all beast2 and json files
shopt -s nullglob
IR_FILES=("$IR_DIR"/*.beast2 "$IR_DIR"/*.json)
shopt -u nullglob

if [ ${#IR_FILES[@]} -eq 0 ]; then
    echo "Error: No .beast2 or .json files found in $IR_DIR"
    exit 1
fi

# Run all in parallel
PIDS=()
NAMES=()
for f in "${IR_FILES[@]}"; do
    base=$(basename "$f")
    name="${base%.*}"
    ext="${base##*.}"
    tag="${name}_${ext}"
    outfile="$TMPDIR/$tag.out"
    errfile="$TMPDIR/$tag.err"
    (timeout 120 "$CLI_BIN" run "$f" -p std > "$outfile" 2> "$errfile"; echo "EXIT:$?" >> "$outfile") &
    PIDS+=($!)
    NAMES+=("$tag")
done

# Wait for all
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done

# Collect results
LEAK_COUNT=0
CLEAN_COUNT=0
ERROR_COUNT=0
FAIL_COUNT=0
LEAK_SUMMARY=""
CLEAN_SUMMARY=""
ERROR_SUMMARY=""
FAIL_SUMMARY=""

for i in "${!NAMES[@]}"; do
    tag="${NAMES[$i]}"
    outfile="$TMPDIR/$tag.out"
    errfile="$TMPDIR/$tag.err"

    if [ ! -f "$outfile" ]; then
        ERROR_SUMMARY+="  MISSING  $tag\n"
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
        leak_bytes=$(grep -oP 'SUMMARY:.*?(\d+ byte)' "$errfile" 2>/dev/null | head -1)
    fi

    # Check for other ASAN errors
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
        ERROR_SUMMARY+="  ASAN   $tag ($asan_type)\n"
        ERROR_COUNT=$((ERROR_COUNT + 1))
    elif $has_leak; then
        LEAK_SUMMARY+="  LEAK   $tag ($leak_bytes)\n"
        LEAK_COUNT=$((LEAK_COUNT + 1))
    elif [ "$exit_code" != "0" ]; then
        err_msg=$(grep -m1 "^Error:" "$errfile" 2>/dev/null || echo "exit=$exit_code")
        FAIL_SUMMARY+="  FAIL   $tag ($err_msg)\n"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    else
        CLEAN_SUMMARY+="  CLEAN  $tag\n"
        CLEAN_COUNT=$((CLEAN_COUNT + 1))
    fi
done

echo "========================================="
echo "  East-C Profile Leak Check ($IR_DIR)"
echo "========================================="
echo ""
if [ -n "$ERROR_SUMMARY" ]; then
    echo -e "$ERROR_SUMMARY" | sort
fi
if [ -n "$LEAK_SUMMARY" ]; then
    echo -e "$LEAK_SUMMARY" | sort
fi
if [ -n "$FAIL_SUMMARY" ]; then
    echo -e "$FAIL_SUMMARY" | sort
fi
if [ -n "$CLEAN_SUMMARY" ]; then
    echo -e "$CLEAN_SUMMARY" | sort
fi
echo ""
echo "========================================="
echo "  Total: $CLEAN_COUNT clean, $LEAK_COUNT leaking, $FAIL_COUNT failed, $ERROR_COUNT errors"
echo "========================================="
