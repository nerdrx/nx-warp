#!/usr/bin/env python3
"""Summarize retained nxvc-vkdec frame logs, excluding frame 0."""
import math, re
from pathlib import Path

rx = re.compile(r"frame (\d+): .*? gpu ([0-9.]+) .*? total ([0-9.]+) ms")

def pct(xs, q):
    xs = sorted(xs)
    if not xs: return None
    i = (len(xs) - 1) * q
    lo, hi = math.floor(i), math.ceil(i)
    return xs[lo] + (xs[hi] - xs[lo]) * (i - lo)

for mode in ("new", "old"):
    runs = []
    for path in sorted(Path(__file__).resolve().parent.glob(f"still-{mode}-*.log")):
        rows = [(float(g), float(t)) for n, g, t in rx.findall(open(path).read())
                if int(n) > 0 and float(g) > 0]
        runs.append((path, rows))
    for path, rows in runs:
        print(mode, path, "n", len(rows),
              "gpu_p50/p95", round(pct([x[0] for x in rows], .5), 3),
              round(pct([x[0] for x in rows], .95), 3),
              "wall_p50/p95", round(pct([x[1] for x in rows], .5), 3),
              round(pct([x[1] for x in rows], .95), 3))
    for label, col in (("gpu", 0), ("wall", 1)):
        vals = [rows[i][col] for _, rows in runs for i in range(len(rows))]
        print(mode, label, "runs", [len(r) for _, r in runs],
              "p50", round(pct(vals, .5), 3), "p95", round(pct(vals, .95), 3))
