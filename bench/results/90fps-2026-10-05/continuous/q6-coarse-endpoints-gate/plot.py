#!/usr/bin/env python3
"""Static aggregate tradeoff; no live performance inference."""
from pathlib import Path
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p = Path(__file__).resolve().parent
rows = list(csv.DictReader((p / "raw.csv").open()))
fig, axes = plt.subplots(1, 2, figsize=(10, 4.8), layout="constrained")
x = range(len(rows))
for ax, base, candidate, label in [
    (axes[0], "baseline_packet24", "candidate_packet24", "Independent packet size (kB, decimal)"),
    (axes[1], "baseline_mse", "candidate_mse", "RGB mean squared error (8-bit values)")]:
    scale = 1000 if base.endswith("packet24") else 1
    for offset, key, legend, color in [(-.18, base, "Production q6", "#777"), (.18, candidate, "Coarser endpoints", "#7946d2")]:
        bars = ax.bar([i + offset for i in x], [float(r[key]) / scale for r in rows], .36, label=legend, color=color)
        ax.bar_label(bars, fmt="%.1f", padding=3, fontsize=9)
    ax.set_xticks(list(x), [r["eye"].capitalize() for r in rows])
    ax.set_ylabel(label)
    ax.set_ylim(0, ax.get_ylim()[1] * 1.15)
    ax.grid(axis="y", alpha=.2)
axes[0].legend(fontsize=9)
fig.suptitle("Native 2176² static fixtures: 7.7–9.0% fewer bytes, higher colour error")
fig.supxlabel("Same ASTC modes and weights; ordinary CEM8 endpoints only. Other blocks unchanged.\nCPU reference gate: no motion, GPU, Pico, FPS or photon latency result.", fontsize=9)
fig.savefig(p / "tradeoff.png", dpi=170)
