"""Plot the matched-source, full-image NXVC control against PyroWave."""

import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


root = Path(__file__).parent
with (root / "results.csv").open(newline="") as file:
    pyro = list(csv.DictReader(file))
with (root / "full-image-nxvc.csv").open(newline="") as file:
    full = list(csv.DictReader(file))

fig, axes = plt.subplots(1, 2, figsize=(11, 4.5))
for ax, scene in zip(axes, ("forest", "dark")):
    chosen = sorted((r for r in full if r["scene"] == scene), key=lambda r: int(r["bytes_per_stereo_frame"]))
    ax.plot([int(r["bytes_per_stereo_frame"]) * 720 / 1e6 for r in chosen],
            [float(r["rgb_psnr_db"]) for r in chosen],
            "o-", color="#f07828", lw=2, label="NXVC full native 4:4:4")
    chosen = sorted((r for r in pyro if r["scene"] == scene and r["chroma"] == "444"),
                    key=lambda r: int(r["bytes_per_stereo_frame"]))
    ax.plot([int(r["bytes_per_stereo_frame"]) * 720 / 1e6 for r in chosen],
            [float(r["rgb_psnr_db"]) for r in chosen],
            "s-", color="#9747ff", lw=2, label="PyroWave full native 4:4:4")
    current = next(r for r in pyro if r["scene"] == scene and r["codec"] == "NXVC")
    ax.scatter([int(current["bytes_per_stereo_frame"]) * 720 / 1e6],
               [float(current["rgb_psnr_db"])], marker="X", s=120,
               color="#222", label="Previous foveated NXVC", zorder=5)
    ax.axvline(500, color="#bbb", lw=1, ls="--")
    ax.set_xscale("log")
    ax.set_xlim(38, 1100)
    ax.set_xticks((50, 100, 500, 1000), ("50", "100", "500", "1000"))
    ax.set_title(scene.title(), fontsize=14, fontweight="bold")
    ax.set_xlabel("Stereo payload rate at 90 fps (Mbit/s)")
    ax.grid(alpha=0.18)
axes[0].set_ylabel("RGB PSNR against original (dB)")
axes[0].legend(frameon=False, loc="lower right", fontsize=8)
fig.suptitle("Same full image: NXVC versus PyroWave", fontsize=15)
fig.text(0.5, 0.01, "Static duplicated-eye 2176² frames • 4:4:4 • NXVC CPU reference encoder • no headset performance claim", ha="center", fontsize=9, color="#555")
fig.tight_layout(rect=(0, 0.05, 1, 0.94))
fig.savefig(root / "full-image-control.png", dpi=180)
