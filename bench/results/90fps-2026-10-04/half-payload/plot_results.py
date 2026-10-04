#!/usr/bin/env python3
"""Plot same-input ASTC payload sizes and decoded quality; no live FPS claims."""
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
data = json.loads((root / "zstd-quality-results.json").read_text())
plt.rcParams.update({"font.size": 11, "figure.facecolor": "#13101b",
                    "axes.facecolor": "#13101b", "text.color": "#ede7ff",
                    "axes.labelcolor": "#ede7ff", "xtick.color": "#ede7ff",
                    "ytick.color": "#ede7ff", "axes.edgecolor": "#756887"})
fig, axes = plt.subplots(1, 2, figsize=(12, 4.8))
for ax, (name, scene) in zip(axes, data["scenes"].items()):
    cases = scene["cases"]
    baseline = cases["6"]["lz4"]["bytes"]
    for method, color in [("lz4", "#b0a0ff"), ("zstd", "#4ce5c4")]:
        rows = [(int(q), c) for q, c in cases.items() if method in c]
        rows.sort(reverse=True)
        x = [c[method]["bytes"] if method == "lz4" else c[method]["3"]["bytes"] for q, c in rows]
        y = [c["rgb_psnr_db"] for q, c in rows]
        ax.plot([n / 1000 for n in x], y, "o-", color=color,
                label="LZ4" if method == "lz4" else "Zstd level 3")
        for (q, c), n, quality in zip(rows, x, y):
            ax.annotate(f"q{q}", (n / 1000, quality), xytext=(4, 5),
                        textcoords="offset points", fontsize=9, color=color)
    ax.axvline(baseline / 2000, color="#ffae67", linestyle="--", label="Half baseline bytes")
    ax.set_title(name.capitalize() + " — native 2176 × 2176 eye")
    ax.set_xlabel("Compressed ASTC payload (decimal kB / image)")
    ax.set_ylabel("Decoded RGB PSNR to source (dB)")
    ax.grid(alpha=.18)
    ax.legend(fontsize=9)
fig.suptitle("Fewer bytes has a quality cost — independent native ASTC frames", fontsize=14)
fig.text(.5, .01, "Offline RGB screenshot fixtures · 24-byte packet header and network overhead excluded · not Pico timings",
         ha="center", fontsize=9)
fig.tight_layout(rect=(0, .035, 1, .93))
fig.savefig(root / "payload-quality.png", dpi=160)
