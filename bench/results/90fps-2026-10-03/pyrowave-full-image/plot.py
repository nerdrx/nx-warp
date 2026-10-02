"""Regenerate the static-image PyroWave comparison graph from results.csv."""

import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


ROOT = Path(__file__).parent
with (ROOT / "results.csv").open(newline="") as file:
    rows = list(csv.DictReader(file))

colors = {"444": "#9747ff", "420": "#18bfc6", "current": "#ff8a40"}
fig, axes = plt.subplots(1, 2, figsize=(11, 4.5), sharex=True)
for axis, scene in zip(axes, ("forest", "dark")):
    scene_rows = [row for row in rows if row["scene"] == scene]
    for chroma in ("current", "444", "420"):
        series = [row for row in scene_rows if row["chroma"] == chroma]
        series.sort(key=lambda row: int(row["bytes_per_stereo_frame"]))
        x = [int(row["bytes_per_stereo_frame"]) * 8 * 90 / 1e6 for row in series]
        y = [float(row["rgb_psnr_db"]) for row in series]
        if chroma == "current":
            axis.scatter(x, y, color=colors[chroma], s=110, marker="X", label="Current NXVC", zorder=4)
        else:
            axis.plot(x, y, color=colors[chroma], marker="o", lw=2, label=f"PyroWave 4:{chroma[0]}:{chroma[1]}")
    axis.set_title(scene.title(), fontsize=14, fontweight="bold")
    axis.set_xscale("log")
    axis.set_xticks((50, 100, 500, 1000), ("50", "100", "500", "1000"))
    axis.set_xlim(38, 1200)
    axis.grid(alpha=0.18)
    axis.set_xlabel("Stereo payload rate at 90 fps (Mbit/s)")
axes[0].set_ylabel("RGB PSNR against original (dB)")
axes[0].legend(frameon=False, loc="lower right")
fig.suptitle("Full-image PyroWave vs current NXVC representation", fontsize=15)
fig.text(0.5, 0.01, "Two static, identical-eye VRChat frames • Desktop GPU stage times only • NXVC reference is foveated", ha="center", fontsize=9, color="#555")
fig.tight_layout(rect=(0, 0.05, 1, 0.94))
fig.savefig(ROOT / "rate-distortion.png", dpi=180)
