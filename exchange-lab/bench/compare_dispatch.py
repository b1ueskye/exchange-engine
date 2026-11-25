#!/usr/bin/env python3
"""Capture or compare Release engine_bench runs using the same workload matrix."""
import argparse
import json
from pathlib import Path
from statistics import median
import subprocess

# Isolate order-book depth, account count and unrelated holds. Keep the expensive
# reference workloads short and print their actual sample counts.
CASES = [
    *(("mix", book, depth, 2, cycles, 1, 0)
      for book in ("list", "pooled")
      for depth, cycles in ((0, 50_000), (100, 5000), (1000, 1000), (10_000, 300))),
    ("mix", "list", 100, 1000, 1000, 1, 0),
    ("mix", "list", 100, 10_000, 300, 1, 0),
    ("mix", "list", 100, 2, 1000, 1, 1000),
    *((mode, "list", 1000, 2, 1000, 1, 0)
      for mode in ("place", "cancel", "reduce", "move", "reject")),
    *(("sweep", "list", 100, 2, cycles, fills, 0)
      for fills, cycles in ((1, 4000), (10, 1000), (100, 100), (1000, 10))),
    ("settlement-failure", "list", 0, 2, 10_000, 2, 0),
]

def capture(binary, output):
    with output.open("w") as file:
        for mode, book, depth, users, cycles, sweep, other_depth in CASES:
            args = (mode, book, depth, users, cycles, sweep, 5, other_depth)
            print(" ".join(map(str, args)), flush=True)
            rows = subprocess.check_output(
                [str(binary.resolve()), *map(str, args)], text=True
            ).splitlines()
            if len(rows) != 5:
                raise ValueError("benchmark did not return all five trials")
            for row in rows:
                record = dict(field.split("=", 1) for field in row.split())
                file.write(json.dumps(record) + "\n")
            file.flush()

def compare(before, after):
    def load(path):
        cases = {}
        for line in path.read_text().splitlines():
            row = json.loads(line)
            key = tuple(row[k] for k in ("mode", "book", "depth", "users", "other_depth", "sweep"))
            cases.setdefault(key, []).append(row)
        return cases

    base, optimized = load(before), load(after)
    if base.keys() != optimized.keys():
        raise ValueError("workload matrices differ")
    print("mode book depth users other sweep samples base_ops/s new_ops/s speedup base_p99_ns new_p99_ns")
    for key, old in base.items():
        new = optimized[key]
        witness = lambda rows: [tuple(r[k] for k in ("round", "ops", "trades", "fingerprint")) for r in rows]
        if len(old) != 5 or witness(old) != witness(new):
            raise ValueError(f"workload results differ: {key}")
        metric = lambda rows, name: median(float(row[name]) for row in rows)
        old_rate, new_rate = metric(old, "ops_per_s"), metric(new, "ops_per_s")
        print(*key, sum(int(r["ops"]) for r in old),
              f"{old_rate:.0f}", f"{new_rate:.0f}", f"{new_rate / old_rate:.2f}x",
              f"{metric(old, 'p99_ns'):.0f}", f"{metric(new, 'p99_ns'):.0f}")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    for name, first, second in (("capture", "binary", "output"), ("compare", "before", "after")):
        command = sub.add_parser(name)
        command.add_argument(first, type=Path)
        command.add_argument(second, type=Path)
    args = parser.parse_args()
    if args.mode == "capture":
        capture(args.binary, args.output)
    else:
        compare(args.before, args.after)

if __name__ == "__main__":
    main()
