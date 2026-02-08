#!/usr/bin/env bash
# Compile-time benchmark: hsm vs Boost-ext.SML
# Measures wall-clock compilation time for equivalent state machines.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SML_INCLUDE="$ROOT_DIR/build_bench/_deps/boost_ext_sml-src/include"
HSM_INCLUDE="$ROOT_DIR/include"

CXX="${CXX:-clang++}"
STD="${STD:-c++20}"
OPT="${OPT:--O2}"
RUNS="${RUNS:-5}"

# Verify dependencies
if [[ ! -d "$SML_INCLUDE" ]]; then
    echo "ERROR: SML headers not found at $SML_INCLUDE"
    echo "Run: cmake -B build_bench -DBUILD_BENCH=ON && cmake --build build_bench"
    exit 1
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

compile_and_time() {
    local label="$1"
    local src="$2"
    shift 2
    local includes=("$@")

    local total=0
    local times=()

    for ((i = 1; i <= RUNS; i++)); do
        local out="$TMPDIR/out_$$_$i"
        local t0 t1 elapsed

        # Drop disk caches between runs (best-effort, usually needs root)
        sync 2>/dev/null || true

        t0=$(python3 -c 'import time; print(time.monotonic())')
        "$CXX" -std="$STD" $OPT -fno-rtti -fno-exceptions \
            "${includes[@]}" \
            -o "$out" "$src" 2>&1
        t1=$(python3 -c 'import time; print(time.monotonic())')

        elapsed=$(python3 -c "print(f'{$t1 - $t0:.3f}')")
        times+=("$elapsed")
        total=$(python3 -c "print($total + $t1 - $t0)")
        rm -f "$out"
    done

    local avg=$(python3 -c "print(f'{$total / $RUNS:.3f}')")
    local min=$(python3 -c "print(f'{min([$( IFS=,; echo "${times[*]}" )]):.3f}')")
    local max=$(python3 -c "print(f'{max([$( IFS=,; echo "${times[*]}" )]):.3f}')")

    printf "  %-20s  avg: %6ss  min: %6ss  max: %6ss  (n=%d)\n" \
        "$label" "$avg" "$min" "$max" "$RUNS"
}

echo "============================================================"
echo "  Compile-Time Benchmark: hsm vs SML"
echo "============================================================"
echo "  Compiler: $($CXX --version 2>&1 | head -1)"
echo "  Std:      $STD"
echo "  Opt:      $OPT"
echo "  Runs:     $RUNS"
echo "============================================================"
echo ""

for size in small medium large; do
    echo "--- $size ---"
    compile_and_time "hsm" "$SCRIPT_DIR/ct_hsm_${size}.cpp" \
        "-I$HSM_INCLUDE"
    compile_and_time "sml" "$SCRIPT_DIR/ct_sml_${size}.cpp" \
        "-I$SML_INCLUDE"
    echo ""
done

echo "============================================================"
echo "  Done."
echo "============================================================"
