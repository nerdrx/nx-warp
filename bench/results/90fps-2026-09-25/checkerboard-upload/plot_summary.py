#!/usr/bin/env python3
"""Plot scalar benchmark summaries from summary-metrics.csv."""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).resolve().parent
with (HERE / "summary-metrics.csv").open(newline="") as source:
    rows = list(csv.DictReader(source))
labels = [f"{row['run']}\n" + {"J": "Old", "K": "Merged", "L": "Old", "N": "Old geometry", "O": "Merged debug", "V": "Old control", "W": "Full-rate", "Y": "Final"}[row["run"]] for row in rows]
colors = ["#697586", "#2474a6", "#697586", "#a68a3a", "#4e9b75", "#697586", "#7457a6", "#1d9d8f"]
x = np.arange(len(rows))
width = 0.34
fig, (gpu, fresh) = plt.subplots(
    2, 1, figsize=(9.2, 6.2), sharex=True,
    gridspec_kw={"height_ratios": [1.45, 1]},
)
for key, offset, color, name in (
    ("gpu_ms", -width / 2, "#2474a6", "GPU render"),
    ("decode_ms", width / 2, "#e08b3e", "Decode"),
):
    means = np.array([float(row[f"{key}_mean"]) for row in rows])
    p95 = np.array([float(row[f"{key}_p95"]) for row in rows])
    gpu.bar(x + offset, means, width, color=color, label=f"{name} mean", zorder=3)
    gpu.errorbar(x + offset, means, yerr=[np.zeros(len(means)), np.maximum(0, p95 - means)],
                 fmt="none", ecolor="#27313a", capsize=2, lw=1, zorder=4)
    for xx, value in zip(x + offset, means):
        gpu.text(xx, value + 0.07, f"{value:.2f}", ha="center", va="bottom", fontsize=8)
gpu.set_ylabel("Time (ms)")
gpu.set_ylim(0, 6.7)
gpu.set_title("Checkerboard upload benchmark — Pico client (11 samples/run)")
gpu.legend(frameon=False, ncol=2, loc="upper right")
gpu.grid(axis="y", alpha=0.22, zorder=0)
fps = np.array([float(row["fresh_fps_mean"]) for row in rows])
fresh.bar(x, fps, color=colors, width=0.58, zorder=3)
fresh.axhline(90, color="#697586", lw=1, ls="--", label="90 Hz target")
for xx, value in zip(x, fps):
    fresh.text(xx, value + 0.002, f"{value:.3f}", ha="center", va="bottom", fontsize=8)
fresh.set_ylabel("Fresh selected image rate (fps)")
fresh.set_ylim(0, 100)
fresh.set_xticks(x, labels)
fresh.grid(axis="y", alpha=0.22, zorder=0)
fresh.legend(frameon=False, loc="lower right")
fig.text(0.01, 0.01,
         "Bars: JSON summary means. Whiskers: p95 only; not confidence intervals. J/K/L are primary paired set.",
         fontsize=8, color="#46515b")
fig.tight_layout(rect=(0, 0.045, 1, 1))
fig.savefig(HERE / "checkerboard-upload-summary.png", dpi=180, bbox_inches="tight")
