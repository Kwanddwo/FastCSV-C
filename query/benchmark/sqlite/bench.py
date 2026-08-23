#!/usr/bin/env python3
"""SQLite benchmark: load a CSV into a fresh SQLite DB, then run SQL queries.

Methodology (kept honest vs csvql / league-csv):
- The DB lives on a tmpfs mount (/shm) so the container's overlayfs /
  bind-mount read penalty does not poison the import measurement.
- Bulk-import pragmas (journal_mode=OFF, synchronous=OFF) are applied; these
  are the standard settings for a one-shot bulk load and mirror what a
  real benchmark harness would use.
- startup_ms = cost of opening a connection and running `SELECT 1` (sqlite's
  fixed per-invocation overhead, the analog of csvql's process startup).
- load_ms  = CREATE TABLE + import, averaged over RUNS (fresh DB each run).
- query_ms = one SELECT on the loaded DB, averaged over RUNS.
- total_ms = load_ms + query_ms (the honest end-to-end cost per run).
"""
import json
import os
import subprocess
import sys
import time

DATA = os.environ.get("DATA", "/data/data.csv")
ROWS = int(os.environ.get("ROWS", "20000"))
RUNS = int(os.environ.get("RUNS", "3"))
OUT = os.environ.get("OUT", "/results/sqlite.json")
DB = os.environ.get("DB", "/shm/bench.db")

CREATE_SQL = (
    "CREATE TABLE data ("
    "id INTEGER, name TEXT, city TEXT, age INTEGER, "
    "salary REAL, score REAL, active INTEGER);"
)

PRAGMAS = "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;"

IMPORT_SCRIPT = (
    PRAGMAS
    + "\n"
    + CREATE_SQL
    + "\n.mode csv\n.import --skip 1 {data} data\n"
)

QUERIES = {
    "q1_filter": "SELECT id, name, age FROM data WHERE age >= 70;",
    "q2_group": "SELECT city, COUNT(*) FROM data GROUP BY city;",
    "q3_sort": "SELECT id, name FROM data ORDER BY salary DESC LIMIT 100;",
    "q4_count": "SELECT COUNT(*) FROM data;",
}

STARTUP_QUERY = "SELECT 1;"


def run_sqlite_script(db, script):
    subprocess.run(["sqlite3", db], input=script, check=True,
                   text=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_sqlite(db, sql):
    run_sqlite_script(db, PRAGMAS + "\n" + sql + "\n")


def import_csv(db):
    if os.path.exists(db):
        os.remove(db)
    run_sqlite_script(db, IMPORT_SCRIPT.format(data=DATA))


def row_count(db, sql):
    """Number of rows the query would return, via a COUNT(*) wrapper."""
    inner = sql.rstrip().rstrip(";")
    proc = subprocess.run(["sqlite3", db, f"SELECT COUNT(*) FROM ({inner})"],
                          check=True, capture_output=True, text=True)
    return int(proc.stdout.strip())


def total_rows(db):
    proc = subprocess.run(["sqlite3", db, "SELECT COUNT(*) FROM data"],
                          check=True, capture_output=True, text=True)
    return int(proc.stdout.strip())


def bench_script(db, script):
    t0 = time.monotonic()
    run_sqlite_script(db, script)
    return (time.monotonic() - t0) * 1000


def bench_load(db):
    t0 = time.monotonic()
    import_csv(db)
    return (time.monotonic() - t0) * 1000


def main():
    if not os.path.exists(DATA):
        print(f"data file not found: {DATA}", file=sys.stderr)
        return 1

    print(f"sqlite bench: data={DATA} rows={ROWS} runs={RUNS}")

    # startup_ms: open a connection and run the trivial q0 baseline.
    startup_ms = bench_script(os.path.join(os.path.dirname(DB) or ".", "s.db"),
                              PRAGMAS + "\n" + STARTUP_QUERY + "\n")
    if os.path.exists(os.path.join(os.path.dirname(DB) or ".", "s.db")):
        os.remove(os.path.join(os.path.dirname(DB) or ".", "s.db"))

    # load_ms: fresh DB import, averaged over RUNS.
    load_ms = 0.0
    for _ in range(RUNS):
        load_ms += bench_load(DB)
    load_ms /= RUNS

    # query timings on a single loaded DB.
    import_csv(DB)
    results = {}
    for key, sql in QUERIES.items():
        total = 0.0
        for _ in range(RUNS):
            total += bench_script(DB, PRAGMAS + "\n" + sql + "\n")
        query_ms = total / RUNS
        # q4 (SELECT COUNT(*)) returns a scalar; surface its value as rows.
        if key == "q4_count":
            rows = total_rows(DB)
        else:
            rows = row_count(DB, sql)
        results[key] = {
            "query_ms": round(query_ms, 3),
            "total_ms": round(load_ms + query_ms, 3),
            "rows": rows,
        }
    if os.path.exists(DB):
        os.remove(DB)

    payload = {
        "candidate": "sqlite",
        "rows": ROWS,
        "runs": RUNS,
        "startup_ms": round(startup_ms, 3),
        "load_ms": round(load_ms, 3),
        "queries": results,
    }
    with open(OUT, "w") as fh:
        json.dump(payload, fh)
    print(f"wrote {OUT}")
    print(json.dumps(payload, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
