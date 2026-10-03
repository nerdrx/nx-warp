#!/usr/bin/env python3
"""Plot matched 4:4:4 source error values supplied in source-quality.csv."""
from pathlib import Path
import csv
import numpy as np
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
with (ROOT / "source-quality.csv").open(newline="") as f:
    rows = list(csv.DictReader(f))
labels = ["Y", "Cb", "Cr"]
paths = ["CDF_9_7", "Paired_Haar"]
colors = ["#315A9B", "#D17A32"]
values = {p: [float(next(r["source_mae_code_values"] for r in rows if r["transform"] == p and r["component"] == c)) for c in labels] for p in paths}
payload = {p: int(next(r["payload_bytes_excluding_44_byte_header"] for r in rows if r["transform"] == p)) for p in paths}
fixture = {p: int(next(r["fixture_bytes_including_44_byte_header"] for r in rows if r["transform"] == p)) for p in paths}

plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "svg.fonttype": "none"})
fig, ax = plt.subplots(figsize=(8.4, 4.8), constrained_layout=True)
x = np.arange(len(labels)); width = .32
for i, (p, color, title) in enumerate(zip(paths, colors, ("CDF 9/7", "Paired Haar"))):
    offset = (i - .5) * width
    bars = ax.bar(x + offset, values[p], width, color=color, edgecolor="white", linewidth=0.8, label=title, zorder=3)
    ax.bar_label(bars, fmt="%.4f", padding=4, fontsize=9, color="#263241")
ax.set_title("Source reconstruction error by component", loc="left", fontsize=15, weight="bold", pad=18)
ax.text(0, 1.01, "4352 × 2176 • 4:4:4 • matched ≈694 kB payloads (common 44 B header excluded)", transform=ax.transAxes, color="#566273", fontsize=10)
ax.set_ylabel("Mean absolute error (8-bit code values)")
ax.set_xticks(x, labels)
ax.set_ylim(0, 1.95)
ax.set_yticks(np.arange(0, 2.01, .25))
ax.grid(axis="y", color="#DDE3EA", linewidth=.8, zorder=0)
ax.spines[["top", "right", "left"]].set_visible(False)
ax.spines["bottom"].set_color("#AEB8C4")
ax.tick_params(axis="y", length=0, colors="#566273")
ax.tick_params(axis="x", length=0, pad=8, colors="#263241")
ax.legend(frameon=False, ncols=2, loc="upper right", bbox_to_anchor=(1, 1.12))
ax.text(.01, -.18,
        f"Payload (44 B header excluded): CDF {payload['CDF_9_7']:,} B; Haar {payload['Paired_Haar']:,} B.",
        transform=ax.transAxes, fontsize=9, color="#566273")
ax.text(.01, -.23,
        f"Fixture sizes including header: {fixture['CDF_9_7']:,} / {fixture['Paired_Haar']:,} B. Haar MAE is higher in every plane.",
        transform=ax.transAxes, fontsize=9, color="#566273")
fig.savefig(ROOT / "source-quality.svg")
fig.savefig(ROOT / "source-quality.png", dpi=180)
