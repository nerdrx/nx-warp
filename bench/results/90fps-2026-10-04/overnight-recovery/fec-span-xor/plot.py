#!/usr/bin/env python3
"""Plot medians from the matched synthetic FEC recovery probe."""
import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

root = Path(__file__).parent
rows = list(csv.DictReader((root / "matched.csv").open()))
labels = ["Baseline", "Direct-span XOR\n+ checked bool"]
treatments = ["baseline-final", "final"]
metrics = [
    ("ns_per_reconstruct", 1e-3, "Median recovery time (µs)"),
    ("allocs_per_reconstruct", 1, "Median operator new calls / recovery"),
    ("allocated_bytes_per_reconstruct", 1, "Median requested bytes / recovery"),
]
colors = ["#6b7280", "#2878b5"]
fig, axes = plt.subplots(1, 3, figsize=(12, 4.1), constrained_layout=True)
for ax, (field, scale, title) in zip(axes, metrics):
    vals = [np.median([int(r[field]) for r in rows if r["treatment"] == t]) * scale for t in treatments]
    bars = ax.bar(labels, vals, color=colors, width=.62)
    ax.set_title(title, fontsize=11)
    ax.set_ylabel("lower is better")
    ax.grid(axis="y", alpha=.22)
    ax.set_axisbelow(True)
    for bar, val in zip(bars, vals):
        text = f"{val:.3f}" if scale == 1e-3 else f"{val:,.0f}"
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height(), text,
                ha="center", va="bottom", fontsize=10)
fig.suptitle("FEC recovery: direct-span XOR host microbenchmark", fontsize=14, weight="bold")
fig.text(.5, -.02,
         "Median of 5 ABBA blocks · GCC 16.2.1 -O2 · Ryzen 9 9950X3D, CPU 3 · synthetic data; not headset/network/FPS",
         ha="center", fontsize=9, color="#444444")
fig.savefig(root / "fec-span-xor.png", dpi=160, bbox_inches="tight")
