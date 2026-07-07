#!/bin/bash
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"

# Pick a single CPU to pin to (avoid core 0 which handles interrupts)
CPU=1
if [ -f /sys/devices/system/cpu/cpu1/topology/core_id ]; then
    :  # CPU 1 exists
else
    CPU=0  # fallback
fi
TASKSET="taskset -c $CPU"

# Try to lock CPU frequency (best-effort, may not work without root)
if [ -w /sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_governor ]; then
    echo performance | tee /sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_governor > /dev/null 2>&1 || true
fi

# Disable turbo boost if possible
if [ -w /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true
fi

RUNS_PER_FILE=3

echo "=== FastCSV-C SIMD Benchmark ==="
echo "  Pinned to CPU $CPU"
echo "  $RUNS_PER_FILE runs per file, reporting best (min) time"
echo ""

# ---- build (only if binaries missing) ----
if [ ! -x "$DIR/benchmark_simd_on" ] || [ ! -x "$DIR/benchmark_simd_off" ]; then
    echo "Building benchmarks..."
    make -C "$DIR" > /dev/null 2>&1
    echo "  benchmark_simd_on  (SIMD ON,  -O3 -mavx2)"
    echo "  benchmark_simd_off (SIMD OFF, -O3 -mno-sse2 -mno-avx2)"
    echo ""
fi

# ---- generate data (only if missing) ----
echo -n "Checking CSV data files..."

gen_csv_3col() {
    local rows="$1" file="$2"
    > "$file"
    for ((i = 0; i < rows; i++)); do
        printf "user_%d,id_%d," "$i" "$i" >> "$file"
        printf "This_is_a_sample_description_that_is_very_long_and_contains_no_special_characters_so_the_SIMD_parser_can_fast_forward_through_multiple_chunks_without_checking_each_byte_this_shows_the_benefit_of_SIMD_optimization_in_csv_parsing_%d\n" "$i" >> "$file"
    done
}

gen_csv_30col() {
    local rows="$1" file="$2"
    > "$file"
    for ((i = 0; i < rows; i++)); do
        for ((j = 0; j < 29; j++)); do
            printf "col%d_val%05d," "$j" "$i" >> "$file"
        done
        printf "col29_val%05d\n" "$i" >> "$file"
    done
}

gen_if_missing() {
    local file="$1" rows="$2" gen_func="$3"
    if [ ! -f "$file" ]; then
        $gen_func "$rows" "$file"
        echo -n " $rows"
    fi
}

echo -n " 3col:"
gen_if_missing "$DIR/data_3col_100.csv"   100   gen_csv_3col
gen_if_missing "$DIR/data_3col_1000.csv"  1000  gen_csv_3col
gen_if_missing "$DIR/data_3col_10000.csv" 10000 gen_csv_3col
echo -n " 30col:"
gen_if_missing "$DIR/data_30col_100.csv"   100   gen_csv_30col
gen_if_missing "$DIR/data_30col_1000.csv"  1000  gen_csv_30col
gen_if_missing "$DIR/data_30col_10000.csv" 10000 gen_csv_30col
echo ""

# ---- run benchmarks ----
run_one() {
    local exe="$1" file="$2" out="$3"
    $TASKSET "$DIR/$exe" "$file" >> "$out"
}

rm -f /tmp/bench_off.txt /tmp/bench_on.txt
ALL_FILES="$DIR/data_3col_100.csv $DIR/data_3col_1000.csv $DIR/data_3col_10000.csv $DIR/data_30col_100.csv $DIR/data_30col_1000.csv $DIR/data_30col_10000.csv"

echo -n "Running SIMD OFF..."
for ((r = 0; r < RUNS_PER_FILE; r++)); do
    echo -n " $((r+1))"; for f in $ALL_FILES; do run_one benchmark_simd_off "$f" /tmp/bench_off.txt; done
done
echo ""

echo -n "Running SIMD ON..."
for ((r = 0; r < RUNS_PER_FILE; r++)); do
    echo -n " $((r+1))"; for f in $ALL_FILES; do run_one benchmark_simd_on "$f" /tmp/bench_on.txt; done
done
echo ""

# ---- parse results (best of RUNS_PER_FILE) ----
parse_mins() {
    local out="$1" file="$2"
    grep "^$(basename "$file"):" "$out" | sed 's/.* min=\([0-9.]*\).*/\1/'
}

best_of() {
    local out="$1" file="$2"
    parse_mins "$out" "$file" | sort -n | head -1
}

print_set() {
    local title="$1" prefix="$2" label="$3" label2="$4" label3="$5" len1="$6" len2="$7" len3="$8"
    local f1="${prefix}100.csv" f2="${prefix}1000.csv" f3="${prefix}10000.csv"

    echo "================================================================"
    echo " $title"
    echo "================================================================"
    echo ""
    echo "File              Lines  SIMD OFF (ms)  SIMD ON (ms)   Speedup"
    echo "----------------- -----  -------------  -------------  -------"

    run_and_print() {
        local file="$1" label="$2" lines="$3"
        local off=$(best_of "/tmp/bench_off.txt" "$file")
        local on=$(best_of "/tmp/bench_on.txt" "$file")
        if [ -n "$off" ] && [ -n "$on" ] && [ "$(echo "$off > 0" | bc -l)" -eq 1 ]; then
            local speedup=$(echo "scale=2; $off / $on" | bc -l)
            printf "%-17s %5d  %13.3f  %13.3f  %5.2fx\n" \
                   "$label" "$lines" "$off" "$on" "$speedup"
        else
            printf "%-17s %5d  %13s  %13s  %s\n" \
                   "$label" "$lines" "N/A" "N/A" "N/A"
        fi
    }

    run_and_print "$DIR/$f1" "$label"  "$len1"
    run_and_print "$DIR/$f2" "$label2" "$len2"
    run_and_print "$DIR/$f3" "$label3" "$len3"
    echo ""
}

print_set \
    "Data Set 1: 3 columns, long 3rd field (SIMD-friendly)" \
    "data_3col_" \
    "100 rows   3-col" "1000 rows  3-col" "10000 rows 3-col" \
    100 1000 10000

print_set \
    "Data Set 2: 30 columns, short fields (SIMD-unfriendly)" \
    "data_30col_" \
    "100 rows  30-col" "1000 rows 30-col" "10000 rows 30-col" \
    100 1000 10000

# Restore turbo if we disabled it (polite)
if [ -w /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true
fi
