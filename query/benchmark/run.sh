#!/usr/bin/env bash
# Orchestrates the FastCSV-C benchmark suite:
#   1. generate the synthetic CSV
#   2. build + run the three candidate containers (csvql, sqlite, league/csv)
#   3. aggregate the results into a comparison table
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROWS="${ROWS:-1000000}"
RUNS="${RUNS:-3}"

cd "${HERE}"

echo "==> [1/3] Generating synthetic dataset (${ROWS} rows)"
python3 generate_data.py --rows "${ROWS}" --out data/data.csv

echo "==> [2/3] Building and running benchmark containers (runs=${RUNS})"
docker compose build
for svc in csvql sqlite league-csv; do
  echo "    -- running ${svc}"
  docker compose run --rm "${svc}"
done

# Containers run as root; hand the results files back to the invoking user.
sudo chown -R "$(id -u):$(id -g)" results 2>/dev/null || true

echo "==> [3/3] Aggregating results"
python3 aggregate.py
