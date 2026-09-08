"""Render the measured window aggregates from summary.json."""
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

p = Path(__file__).resolve().parent
runs = list(json.loads((p / "summary.json").read_text())["runs"].values())
fig, ax = plt.subplots(figsize=(8, 4.6), layout="constrained")
x = range(3)
bars = ax.bar(x, [r["gpu_ms_median"] for r in runs], width=0.55,
              color=["#087e70", "#526377", "#087e70"], label="Median GPU window mean")
ax.scatter(x, [r["estimated_noncache_gpu_ms_median"] for r in runs],
           color="#202c38", marker="D", s=45, zorder=3, label="Estimated noncached window mean")
for b, r in zip(bars, runs):
    ax.text(b.get_x() + b.get_width()/2, b.get_height()/2, f'{r["gpu_ms_median"]:.2f} ms',
            ha="center", color="white", weight="bold", fontsize=13)
ax.axhline(1000/240, linestyle="--", color="#b45626", linewidth=1)
ax.text(-0.38, 4.35, "4.17 ms stage budget", color="#b45626", fontsize=10)
ax.set(xticks=x, xticklabels=["Vertex warp", "Original control", "Vertex warp repeated"],
       ylabel="Render GPU time (ms)", ylim=(0, 8),
       title="Move tile homographies out of millions of fragments")
ax.legend(frameon=False, loc="upper left", fontsize=9)
ax.spines[["top", "right"]].set_visible(False)
ax.grid(axis="y", alpha=0.15)
ax.set_axisbelow(True)
fig.supxlabel("Pico 4 · 2160×2160 per eye · ordered live runs · same prototype APK\n"
              "Window aggregates include cache hits. No per-frame p99 or physical 240 FPS claim.", fontsize=9)
fig.savefig(p / "render-comparison.png", dpi=180)
