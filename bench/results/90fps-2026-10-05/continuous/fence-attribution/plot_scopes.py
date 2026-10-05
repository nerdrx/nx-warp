#!/usr/bin/env python3
"""Plot distinct retained timing intervals; never infer queue or photon delay."""
import csv
from math import ceil
from pathlib import Path
from statistics import mean
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

base = Path(__file__).resolve().parent
with (base / "retained-serial.csv").open() as f:
    rows = list(csv.DictReader(f))
assert len(rows) == 20 and all(row["mode"] == "serial_l3" for row in rows)
fig, axes = plt.subplots(1, 2, figsize=(11, 5.5), sharey=True)
fields = ["eye{eye}_wait_ms", "eye{eye}_gpu_dispatch_readback_ms", "submit_to_fence{eye}_ms"]
labels = ["CPU fence\nwait call", "GPU dispatch\nthrough copy", "Host submit to\nfence observation"]
colors = ["#287C8E", "#6B58A8", "#B66432"]
for eye, ax in enumerate(axes):
    for column, (field, color) in enumerate(zip(fields, colors)):
        values = [float(row[field.format(eye=eye)]) for row in rows]
        assert all(value > 0 for value in values)
        xs = [column + (i - (len(values)-1)/2)*0.012 for i in range(len(values))]
        ax.scatter(xs, values, s=22, color=color, alpha=0.6)
        average = mean(values)
        p95 = sorted(values)[ceil(0.95*len(values))-1]
        ax.plot([column-0.17, column+0.17], [average, average], color="black", lw=2)
        ax.scatter([column], [p95], color="black", marker="D", s=26)
        ax.annotate(f"mean {average:.4f}\np95 {p95:.4f}", (column, p95),
                    xytext=(0, 12), textcoords="offset points", ha="center", fontsize=9)
    ax.set_title(f"Eye {eye} · 20 retained samples")
    ax.set_xticks(range(3), labels)
    ax.set_yscale("log")
    ax.set_ylim(0.0006, 9)
    ax.set_yticks([0.001, 0.01, 0.1, 1], ["0.001", "0.01", "0.1", "1"])
    ax.grid(axis="y", alpha=0.25)
    ax.spines[["top", "right"]].set_visible(False)
axes[0].set_ylabel("Interval duration (ms, logarithmic scale)")
fig.suptitle("Different boundaries explain different timings", fontsize=16, y=0.97)
fig.text(0.5, 0.055, "Dots: individual samples. Black line: mean. Diamond: p95. Retained offscreen serial L3 run.", ha="center", fontsize=9)
fig.text(0.5, 0.02, "Do not subtract CPU and GPU intervals to infer queue delay. No live FPS or photon claim.", ha="center", fontsize=9)
fig.subplots_adjust(top=0.84, bottom=0.19, left=0.09, right=0.98, wspace=0.12)
fig.savefig(base / "scope-comparison.png", dpi=180)
fig.savefig(base / "scope-comparison.svg")
print("PASS: 20 retained rows plotted; no new timing run")
