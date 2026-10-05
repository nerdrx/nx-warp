#!/usr/bin/env python3
"""Plot retained outputs from the actual controller replay, never measured FPS."""
from pathlib import Path
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
with (root / "baseline-20ms-spacing.csv").open() as file:
    before = list(csv.DictReader(file))
with (root / "patched-20ms-spacing.csv").open() as file:
    after = list(csv.DictReader(file))
assert [row["case"] for row in before] == [row["case"] for row in after]
labels = ["Single 400 KB burst", "Two overlapping\n200 KB eye bursts", "Two sequential\n200 KB eye bursts"]
fig, axes = plt.subplots(1, 2, figsize=(11, 4.8), sharex=True)
for ax, field, title, unit in zip(axes,
    ["controller_estimate_mbps", "bitrate_bps"],
    ["Delivery-rate estimate", "Controller target after replay"],
    [1, 1e-6]):
    for offset, rows, colour, label in [(-0.18, before, "#8b73cd", "Before"), (0.18, after, "#2b998c", "Matched intervals")]:
        values = [float(row[field]) * unit for row in rows]
        bars = ax.bar([i + offset for i in range(3)], values, width=0.36, color=colour, label=label)
        ax.bar_label(bars, labels=[f"{value:.1f}" for value in values], fontsize=9, padding=3)
    ax.set_xticks(range(3), labels, fontsize=9)
    ax.set_ylabel("Mbit/s")
    ax.set_title(title, fontsize=12)
    ax.set_ylim(0, 780)
    ax.set_axisbelow(True)
    ax.grid(axis="y", alpha=0.2)
    ax.spines[["top", "right"]].set_visible(False)
axes[0].legend(loc="upper left", fontsize=9)
fig.suptitle("Same bytes; matching the delivery-time scope", fontsize=15)
fig.text(0.5, 0.025, "Synthetic production-controller replay • 60 fixtures • 20 ms client/host spacing • desired refresh 90 Hz\n6 ms per burst • 1 Gbit/s ceiling • not a headset, link-capacity or fresh-frame measurement", ha="center", fontsize=9)
fig.tight_layout(rect=(0, 0.11, 1, 0.95))
fig.savefig(root / "scope-comparison.png", dpi=160)
fig.savefig(root / "scope-comparison.svg")
svg = root / "scope-comparison.svg"
svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
