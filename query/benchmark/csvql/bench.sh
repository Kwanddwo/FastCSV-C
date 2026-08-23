#!/usr/bin/env bash
set -euo pipefail

CSVQL=${CSVQL:-/repo/query/build/csvql}
DATA=${DATA:-/data/data.csv}
ROWS=${ROWS:-20000}
RUNS=${RUNS:-3}
OUT=${OUT:-/results/csvql.json}

declare -A QUERIES=(
  [q1_filter]="SELECT id, name, age FROM '${DATA}' WHERE age >= 70;"
  [q2_group]="SELECT city, COUNT(*) FROM '${DATA}' GROUP BY city;"
  [q3_sort]="SELECT id, name FROM '${DATA}' ORDER BY salary DESC LIMIT 100;"
  [q4_count]="SELECT COUNT(*) FROM '${DATA}';"
)

if [ ! -f "${CSVQL}" ]; then
  echo "csvql binary not found at ${CSVQL}" >&2
  exit 1
fi

ns() { date +%s%N; }

# Run a query and print "<elapsed_ms> <rows>".
run_one() {
  local sql="$1"
  local before after output rows=0
  before=$(ns)
  output=$("${CSVQL}" "${sql}" 2>&1)
  after=$(ns)
  local elapsed_ms=$(( (after - before) / 1000000 ))
  rows=$(printf '%s\n' "${output}" | sed -n 's/^\([0-9][0-9]*\) row(s) in set\./\1/p' | head -n1)
  rows=${rows:-0}
  if [ -z "${rows}" ] || ! [[ "${rows}" =~ ^[0-9]+$ ]]; then rows=0; fi
  echo "${elapsed_ms} ${rows}"
}

# For the COUNT(*) query the meaningful result is the scalar value, which
# csvql renders inside its table; extract it from the data row.
run_count_value() {
  local output value
  output=$("${CSVQL}" "${QUERIES[q4_count]}" 2>&1)
  value=$(printf '%s\n' "${output}" | sed -n 's/^| *\([0-9][0-9]*\) *|$/\1/p' | head -n1)
  value=${value:-0}
  echo "${value}"
}

echo "csvql bench: data=${DATA} rows=${ROWS} runs=${RUNS}"

# startup_ms: csvql has no file-free query, so measure the fixed
# per-invocation cost on a 1-row CSV (spawn + arena alloc + minimal parse).
TINY=$(mktemp)
printf 'id,name\n1,foo\n' > "${TINY}"
startup_total=0
for ((i = 0; i < RUNS; i++)); do
  read -r ms _ < <(run_one "SELECT COUNT(*) FROM '${TINY}';")
  startup_total=$((startup_total + ms))
done
startup_ms=$((startup_total / RUNS))
rm -f "${TINY}"

# csvql fuses parsing into each query: there is no separate load step, so
# load_ms is reported as 0 and query_ms is the full one-shot cost a user
# pays on every run (parse + execute).
count_value=$(run_count_value)

json="{"
json+="\"candidate\":\"csvql\","
json+="\"rows\":${ROWS},"
json+="\"runs\":${RUNS},"
json+="\"startup_ms\":${startup_ms},"
json+="\"load_ms\":0,"
json+="\"queries\":{"
first=1
for key in q1_filter q2_group q3_sort q4_count; do
  total=0
  rows=0
  for ((i = 0; i < RUNS; i++)); do
    read -r ms r < <(run_one "${QUERIES[$key]}")
    total=$((total + ms))
    rows=${r}
  done
  avg_ms=$((total / RUNS))
  # q4's meaningful result is the scalar count value
  if [ "${key}" = "q4_count" ]; then rows=${count_value}; fi
  if [ "${first}" -ne 1 ]; then json+=","; fi
  first=0
  json+="\"${key}\":{\"total_ms\":${avg_ms},\"query_ms\":${avg_ms},\"rows\":${rows}}"
done
json+="}}"
printf '%s\n' "${json}" > "${OUT}"
echo "wrote ${OUT}"
cat "${OUT}"
