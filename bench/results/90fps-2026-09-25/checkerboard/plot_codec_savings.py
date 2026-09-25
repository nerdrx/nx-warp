#!/usr/bin/env python3
"""Plot phase-averaged compressed-byte savings from codec-results.csv."""
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent
values = defaultdict(lambda: [0, 0])
with (ROOT / "codec-results.csv").open(newline="") as stream:
    for row in csv.DictReader(stream):
        key = (Path(row["source"]).name, row["codec"])
        values[key][0] += int(row["baseline_bytes"])
        values[key][1] += int(row["checker_bytes"])

sources = [
    ("dark-gpu-shift0.nxdf", "Dark · shift 0"),
    ("dark-gpu-shift8.nxdf", "Dark · shift 8"),
    ("forest-gpu-shift0.nxdf", "Forest · shift 0"),
    ("forest-gpu-shift8.nxdf", "Forest · shift 8"),
]
codecs = [("lz4", "LZ4", "#39b6a3"), ("zstd", "Zstd", "#6478d7")]
x = np.arange(len(sources))
width = 0.34
top = max(100 * (values[(filename, codec)][0] - values[(filename, codec)][1]) /
          values[(filename, codec)][0]
          for filename, _ in sources for codec, _, _ in codecs)
fig, ax = plt.subplots(figsize=(9.2, 4.8), layout="constrained")
for i, (codec, label, color) in enumerate(codecs):
    reductions = []
    for filename, _ in sources:
        baseline, checker = values[(filename, codec)]
        reductions.append(100 * (baseline - checker) / baseline)
    bars = ax.bar(x + (i - 0.5) * width, reductions, width, label=label, color=color)
    ax.bar_label(bars, fmt="%.1f%%", padding=3, fontsize=9)

ax.set_title("Checkerboard compressed-byte savings")
ax.set_ylabel("Reduction from full-frame encoding (%)")
ax.set_xticks(x, [label for _, label in sources])
ax.set_ylim(0, max(50, top + 9))
ax.grid(axis="y", alpha=0.22)
ax.set_axisbelow(True)
ax.legend(frameon=False, ncols=2, loc="upper left")
fig.text(0.5, -0.015,
         "Mean of two checker phases per 2176×2176 stereo fixture; host compression only.",
         ha="center", fontsize=9, color="#555555")
fig.savefig(ROOT / "codec-savings.png", dpi=180, bbox_inches="tight")
