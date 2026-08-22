#!/usr/bin/env python3
"""Deterministic synthetic CSV generator for the FastCSV-C benchmark suite.

Produces an RFC-4180-clean CSV with typed columns. The generator is seeded so
every run of the suite uses byte-identical input across the three candidates.
"""
import argparse
import csv
import random
import sys
from pathlib import Path

CITIES = ["NYC", "LA", "Chicago", "Houston", "Phoenix", "London", "Berlin", "Tokyo"]
FIRST = ["Alice", "Bob", "Charlie", "Diana", "Eve", "Frank", "Grace", "Heidi",
         "Ivan", "Judy", "Karl", "Lena", "Mona", "Nina", "Oscar", "Paul"]
LAST = ["Smith", "Jones", "Brown", "Davis", "Miller", "Wilson", "Moore",
        "Taylor", "Anderson", "Thomas", "Jackson", "White", "Harris", "Martin"]
PREFIXES = ["", "", "", "", "Dr.", "Prof."]
NICKNAMES = ["", "", "The Great", "Junior", "III", "The Bold"]


def generate(rows: int, seed: int = 42) -> list:
    rng = random.Random(seed)
    out = []
    for i in range(rows):
        name = "{} {} {} {}".format(
            rng.choice(PREFIXES).strip(),
            rng.choice(FIRST),
            rng.choice(LAST),
            rng.choice(NICKNAMES).strip(),
        ).strip()
        name = " ".join(name.split())
        out.append({
            "id": i + 1,
            "name": name,
            "city": rng.choice(CITIES),
            "age": rng.randint(18, 90),
            "salary": round(rng.uniform(20_000, 250_000), 2),
            "score": round(rng.uniform(0, 100), 4),
            "active": rng.randint(0, 1),
        })
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rows", type=int, default=20_000,
                    help="number of data rows (default: 20000)")
    ap.add_argument("--seed", type=int, default=42,
                    help="deterministic RNG seed (default: 42)")
    ap.add_argument("--out", type=Path, default=Path("data/data.csv"),
                    help="output CSV path (default: data/data.csv)")
    args = ap.parse_args()

    if args.rows <= 0:
        print("rows must be > 0", file=sys.stderr)
        return 2

    args.out.parent.mkdir(parents=True, exist_ok=True)
    header = ["id", "name", "city", "age", "salary", "score", "active"]

    with args.out.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=header)
        writer.writeheader()
        for row in generate(args.rows, args.seed):
            writer.writerow(row)

    size = args.out.stat().st_size
    print(f"wrote {args.rows} rows -> {args.out} ({size / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
