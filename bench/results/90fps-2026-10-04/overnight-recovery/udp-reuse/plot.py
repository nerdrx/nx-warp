#!/usr/bin/env python3
"""Plot the matched production UDP buffer reuse probe from runs.csv."""
import argparse
import csv
import pathlib
import random
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

ROOT = pathlib.Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument("csv", nargs="?", type=pathlib.Path, default=ROOT / "runs.csv")
parser.add_argument("output", nargs="?", type=pathlib.Path, default=ROOT / "udp-reuse.png")
args = parser.parse_args()
with args.csv.open(newline="") as f:
    rows = list(csv.DictReader(f))
if len(rows) != 40:
    raise SystemExit(f"expected 40 runs, got {len(rows)}")

colors = {"baseline": "#3569a8", "pool": "#d67b24"}
metrics = {
    "baseline": [r for r in rows if r["treatment"] == "baseline"],
    "pool": [r for r in rows if r["treatment"] == "pool"],
}
for name, values in metrics.items():
    if len(values) != 20:
        raise SystemExit(f"expected 20 {name} runs, got {len(values)}")

allocation_medians = {
    name: statistics.median(int(r["matching_allocs"]) for r in values)
    for name, values in metrics.items()
}
reduction = 100 * (1 - allocation_medians["pool"] / allocation_medians["baseline"])

fig, (ax_alloc, ax_latency) = plt.subplots(
    1, 2, figsize=(11.2, 5.0), gridspec_kw={"width_ratios": [0.8, 1.5]}
)
fig.subplots_adjust(left=0.08, right=0.98, bottom=0.24, top=0.79, wspace=0.35)
fig.suptitle("UDP receive buffer reuse", fontsize=17, fontweight="bold", y=0.96)
fig.text(
    0.5,
    0.875,
    "20 ABBA runs per treatment · 400 batches/run · 20 × 1400 B datagrams/batch · CPU 20 pinned",
    ha="center",
    fontsize=9.5,
)

# Every run observed the same treatment-specific allocation count.
labels = ["Baseline", "Pool"]
values = [allocation_medians["baseline"], allocation_medians["pool"]]
bars = ax_alloc.bar(labels, values, color=[colors["baseline"], colors["pool"]], width=0.62)
ax_alloc.set_title("~40 KiB allocations", fontsize=12, pad=10)
ax_alloc.set_ylabel("Matching operator-new requests per 400 batches")
ax_alloc.set_ylim(0, max(values) * 1.28)
ax_alloc.grid(axis="y", alpha=0.25)
ax_alloc.set_axisbelow(True)
for bar, value in zip(bars, values):
    ax_alloc.text(bar.get_x() + bar.get_width() / 2, value + 6, f"{int(value)}", ha="center", fontweight="bold")
ax_alloc.text(
    0.5,
    max(values) * 1.13,
    f"{reduction:.1f}% fewer",
    ha="center",
    fontsize=11,
    fontweight="bold",
    color="#333333",
)

# Each dot is one run's percentile, computed across that run's 400 timed batch calls.
positions = [1, 2, 4, 5]
series = [
    ("baseline", "batch_p50_ns", positions[0], "Baseline P50"),
    ("pool", "batch_p50_ns", positions[1], "Pool P50"),
    ("baseline", "batch_p95_ns", positions[2], "Baseline P95"),
    ("pool", "batch_p95_ns", positions[3], "Pool P95"),
]
for treatment, field, x, label in series:
    vals = [int(r[field]) / 1000 for r in metrics[treatment]]
    ax_latency.boxplot(
        vals,
        positions=[x],
        widths=0.52,
        showfliers=False,
        patch_artist=True,
        boxprops={"facecolor": colors[treatment], "alpha": 0.22, "edgecolor": colors[treatment]},
        medianprops={"color": colors[treatment], "linewidth": 2},
        whiskerprops={"color": colors[treatment]},
        capprops={"color": colors[treatment]},
    )
    rng = random.Random(x)
    jitter = [rng.uniform(-0.10, 0.10) for _ in vals]
    ax_latency.scatter(
        [x + j for j in jitter], vals, s=17, alpha=0.68, color=colors[treatment], edgecolors="none", zorder=3
    )
    ax_latency.text(x, min(vals) - 0.22, f"{statistics.median(vals):.2f}", ha="center", fontsize=8.5)
ax_latency.set_xticks(positions, ["Baseline\nP50", "Pool\nP50", "Baseline\nP95", "Pool\nP95"])
ax_latency.set_ylabel("Receive + pending-drain time per batch (µs)")
ax_latency.set_title("Per-batch receive latency", fontsize=12, pad=10)
ax_latency.set_ylim(1.5, 4.8)
ax_latency.grid(axis="y", alpha=0.25)
ax_latency.set_axisbelow(True)
ax_latency.legend(
    handles=[Patch(facecolor=colors["baseline"], label="Baseline"), Patch(facecolor=colors["pool"], label="Pool")],
    frameon=False,
    loc="upper left",
    ncol=2,
)

fig.text(
    0.5,
    0.06,
    "Dots = run-level percentiles (20 runs/treatment); labels = medians. Loopback API probe only; no FPS or Pico evidence.",
    ha="center",
    fontsize=8.5,
    color="#444444",
)
fig.savefig(args.output, dpi=180, facecolor="white")
