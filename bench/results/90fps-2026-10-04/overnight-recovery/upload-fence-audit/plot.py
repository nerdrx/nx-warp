#!/usr/bin/env python3
"""Plot sanitized prior-upload-fence 180-frame window means."""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
samples = {"Same-queue async": [], "Sync": []}
names = {"same-queue async": "Same-queue async", "sync": "Sync"}
with (root / "window_means.csv").open(newline="") as handle:
    for row in csv.DictReader(handle):
        samples[names[row["mode"]]].append(
            float(row["prior_upload_fence_180_frame_mean_us"])
        )

fig, ax = plt.subplots(figsize=(6.4, 3.8), constrained_layout=True)
ax.boxplot([samples[name] for name in samples], tick_labels=list(samples), showfliers=True)
ax.set_ylabel("Prior upload-fence wait (µs)")
ax.set_title("180-frame window means")
ax.grid(axis="y", alpha=0.25)
fig.savefig(root / "plot.png", dpi=160)
