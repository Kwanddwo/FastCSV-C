#!/usr/bin/env python3
"""Aggregate per-candidate benchmark JSON results into a comparison table.

Reads benchmark/results/*.json (csvql, sqlite, league-csv) and prints
markdown tables plus a machine-readable CSV. Cross-checks that every
candidate returned the same row count per query.

The amortization table models repeated use: total cost for N queries in a
single process = load_ms + N * query_ms. csvql reports load_ms = 0 because
parsing is fused into every query; sqlite pays its import once; league/csv
pays its parse once. The crossover shows where a load-once engine overtakes
a parse-every-time one.
"""
import csv
import json
import sys
from pathlib import Path

RESULTS_DIR = Path(__file__).parent / "results"
QUERY_ORDER = ["q1_filter", "q2_group", "q3_sort", "q4_count"]
QUERY_LABELS = {
    "q1_filter": "Q1 filter (age>=70)",
    "q2_group": "Q2 group by city",
    "q3_sort": "Q3 sort+limit 100",
    "q4_count": "Q4 count all",
}
AMORT_N = [1, 10, 100, 1000]


def load_results(directory: Path):
    out = {}
    for f in sorted(directory.glob("*.json")):
        try:
            data = json.loads(f.read_text())
        except json.JSONDecodeError as exc:
            print(f"warning: skipping {f.name}: {exc}", file=sys.stderr)
            continue
        out[data["candidate"]] = data
    return out


def crosscheck(results):
    """Every candidate must agree on rows returned per query."""
    problems = []
    for q in QUERY_ORDER:
        counts = {name: res["queries"][q]["rows"] for name, res in results.items()
                  if q in res.get("queries", {})}
        if len(set(counts.values())) > 1:
            problems.append((q, counts))
    return problems


def main():
    results = load_results(RESULTS_DIR)
    if not results:
        print(f"no results found in {RESULTS_DIR}", file=sys.stderr)
        return 1

    candidates = sorted(results)
    rows = results[candidates[0]].get("rows", "?")

    problems = crosscheck(results)
    if problems:
        print("CROSS-CHECK FAILED (row counts disagree per query):", file=sys.stderr)
        for q, counts in problems:
            print(f"  {q}: {counts}", file=sys.stderr)
        print(file=sys.stderr)

    print(f"# FastCSV-C Benchmark Results (rows={rows:,})")
    print()
    print("Timings are wall-clock milliseconds averaged over the reported "
          "number of runs. `total_ms` is the end-to-end cost for a fresh run "
          "in a single process (load once, then one query).")
    print()

    hdr = ["query", "candidate", "startup_ms", "total_ms", "rows"]
    print("| " + " | ".join(hdr) + " |")
    print("|" + "|".join(["---"] * len(hdr)) + "|")

    csv_rows = []
    for q in QUERY_ORDER:
        for name in candidates:
            qres = results[name]["queries"].get(q)
            if not qres:
                continue
            startup = results[name].get("startup_ms", 0.0)
            row = [
                QUERY_LABELS[q],
                name,
                f"{startup:.2f}",
                f"{qres['total_ms']:.1f}",
                qres["rows"],
            ]
            print("| " + " | ".join(map(str, row)) + " |")
            csv_rows.append([q, name, startup, qres["total_ms"], qres["rows"]])

    # Amortization table: cost of running each query N times in one process.
    print()
    print("## Amortization (cost of running a query N times in one process)")
    print()
    print("Formula: `load_ms + N * query_ms`. csvql re-parses on every run "
          "(load_ms=0); sqlite and league/csv pay load once and benefit from "
          "repeated queries. Bold marks the cheapest engine per N.")
    print()
    hdr = ["query", "N=1", "N=10", "N=100", "N=1000"]
    print("| " + " | ".join(hdr) + " |")
    print("|" + "|".join(["---"] * len(hdr)) + "|")
    for q in QUERY_ORDER:
        best = {}
        for n in AMORT_N:
            vals = {}
            for name in candidates:
                qres = results[name]["queries"].get(q)
                if qres:
                    vals[name] = results[name]["load_ms"] + n * qres["query_ms"]
            if vals:
                best[n] = min(vals, key=vals.get)
        cells = []
        for name in candidates:
            qres = results[name]["queries"].get(q)
            if not qres:
                continue
            parts = []
            for n in AMORT_N:
                v = results[name]["load_ms"] + n * qres["query_ms"]
                if best.get(n) == name:
                    parts.append(f"**{v:.1f}**")
                else:
                    parts.append(f"{v:.1f}")
            cells.append(f"{name}: " + " / ".join(parts))
        print(f"| {QUERY_LABELS[q]} | " + " | ".join(cells) + " |")

    out_csv = RESULTS_DIR / "comparison.csv"
    with out_csv.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["query", "candidate", "startup_ms", "total_ms", "rows"])
        writer.writerows(csv_rows)
    print()
    print(f"machine-readable results written to {out_csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
