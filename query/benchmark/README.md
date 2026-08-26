# FastCSV-C Benchmark Suite

Dockerized benchmarks comparing how fast three different stacks can run the
same queries over the same CSV file:

| Candidate | Approach |
|-----------|----------|
| **csvql** | FastCSV-C query shell. Single C process, arena-backed, parses the CSV lazily *while* executing the query (no separate load step). |
| **sqlite** | Load the CSV into a fresh SQLite database (`.import`), then run the query. |
| **league/csv** | Popular PHP CSV library. Parses via PHP's C-backed `fgetcsv`/`SplFileObject` (load phase), queries the in-memory records in PHP. |

## Layout

```
benchmark/
├── docker-compose.yml      # services: csvql, sqlite, league-csv
├── run.sh                  # generate data -> build -> run -> aggregate
├── generate_data.py        # deterministic synthetic CSV (host python3)
├── aggregate.py            # merge results/*.json into a comparison table
├── data/                   # generated dataset (gitignored)
├── results/                # per-candidate JSON + comparison.csv (gitignored)
├── csvql/                  # Dockerfile + bench.sh
├── sqlite/                 # Dockerfile + bench.py
└── league-csv/             # Dockerfile + composer.json + bench.php
```

## Requirements

- Docker with Compose v2 (`docker compose`)
- Python 3 on the host (for data generation and aggregation)

## Usage

```bash
cd benchmark
./run.sh                          # default: 1,000,000 rows, 3 runs
ROWS=100000 RUNS=5 ./run.sh       # override dataset size / iterations
```

The script:

1. Generates `data/data.csv` (deterministic, seeded).
2. Builds the three containers.
3. Runs each container sequentially, writing `results/<candidate>.json`.
4. Prints a markdown comparison table (including an amortization table) and
   writes `results/comparison.csv`.

## Dataset

`generate_data.py` produces typed, RFC-4180-clean rows:

```
id,name,city,age,salary,score,active
```

The same file is mounted read-only into every container, so all candidates
read byte-identical input.

## Queries

Every candidate runs the same four queries (SQL shown; the PHP candidate
expresses the equivalent via `Statement`/PHP):

| id | Query | Notes |
|----|-------|-------|
| Q1 | `SELECT id,name,age FROM data WHERE age >= 70` | filter, ~28% of rows |
| Q2 | `SELECT city, COUNT(*) FROM data GROUP BY city` | aggregation |
| Q3 | `SELECT id,name FROM data ORDER BY salary DESC LIMIT 100` | sort + limit |
| Q4 | `SELECT COUNT(*) FROM data` | full-scan baseline |

The reported `rows` column lets `aggregate.py` cross-check that all candidates
returned identical results (it prints `CROSS-CHECK FAILED` otherwise).

## Metrics

Per candidate × query, wall-clock milliseconds averaged over `RUNS`:

- **startup_ms** — fixed per-process baseline: csvql spawn + minimal query on a
  1-row file; sqlite `sqlite3` CLI + connection (`SELECT 1`); league/csv
  autoload + library init.
- **load_ms** — one-time cost to get the CSV into queryable form:
  - sqlite: `CREATE TABLE` + `.import` into a fresh DB on a tmpfs mount
    (`/shm`), with `journal_mode=OFF`/`synchronous=OFF` bulk-load pragmas
  - league/csv: `Reader` -> materialize all records
  - csvql: `0` — parsing is fused into every query (no separate load step)
- **query_ms** — the query itself on the loaded form: sqlite `SELECT`;
  league/csv `Statement`/PHP; csvql the full one-shot parse+query cost.
- **total_ms** — `load_ms + query_ms`, the honest end-to-end cost of a fresh
  run in a single process (load once, then one query).

The **amortization table** models repeated use: `load_ms + N * query_ms` for
N = 1, 10, 100, 1000. csvql re-parses the CSV on *every* query (its fixed
startup is not even counted here); sqlite and league/csv pay their load once
and amortize it over N queries — so the table shows the crossover where a
load-once engine becomes cheaper.

## csvql arena sizing

csvql's arenas (parse, per-row scratch, result set) are growable: each starts
small and chains additional chunks on demand, so memory stays proportional to
what a query actually holds. A `SELECT COUNT(*)` over the 1M-row dataset
barely allocates, while `ORDER BY` materializes every projected row before
applying `LIMIT` and grows as it goes (~150 bytes/row). No sizing, no tuning,
no environment variables.

## Caveats

- **csvql re-parses on every invocation.** Each `csvql` run pays process spawn
  (`startup_ms`) plus a full parse even for a trivial query; that is its real
  usage model and it is captured as `query_ms` (its `load_ms` is 0). A
  long-running process holding parsed data would beat this, but that is not
  what the tool provides.
- **sqlite's import is measured on tmpfs** (`/shm`) with bulk-load pragmas so
  the container's overlayfs / bind-mount read penalty does not inflate it;
  importing from a cold disk or without pragmas is slower. Import also
  excludes `startup_ms` (the `sqlite3` CLI spawn per command).
- **league/csv** parses the file once then queries in-process; its load phase
  mirrors a real "parse then query" workflow in a long-lived process.
- Timings are wall-clock and depend on host hardware/load; run multiple times
  (`RUNS=5`) for stable numbers.

## Adding a query

Edit the query in all three places (they must stay semantically identical):

- `csvql/bench.sh` — `QUERIES` map (SQL)
- `sqlite/bench.py` — `QUERIES` dict (SQL)
- `league-csv/bench.php` — the q-block at the bottom (PHP equivalent)

Then add a label to `QUERY_LABELS` in `aggregate.py`.
