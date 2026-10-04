"""Plot measured fixture payload and explicitly separate raw budget estimates."""
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
rows = json.loads((ROOT / "thumbnail-payload.json").read_text())
names = list(dict.fromkeys(r["input"] for r in rows))
fig, axes = plt.subplots(1, 2, figsize=(12, 4.8), layout="constrained")
colors = ["#9c78ff", "#53c9c1", "#f6ad65"]
for j, size in enumerate((128, 256, 512)):
    vals = [next(r["stereo_90hz_mbps"] for r in rows
                 if r["input"] == name and r["size_per_eye"] == size) for name in names]
    axes[0].bar([i + (j - 1) * .24 for i in range(len(names))], vals,
                width=.23, color=colors[j], label=f"{size}² per eye")
axes[0].axhline(20, color="#b34056", linestyle="--", label="20 Mbit/s target")
axes[0].set_xticks(range(len(names)), ["Room fast", "Room mid", "Room rest", "Object motion", "Photo"])
axes[0].tick_params(axis="x", rotation=25)
axes[0].set_ylabel("Duplicated-eye payload at 90 Hz (Mbit/s)")
axes[0].set_title("Measured ASTC + Zstd3 fixture bytes")
axes[0].legend(fontsize=8)
sizes = (128, 192, 256)
raw = [2 * ((s + 7) // 8) ** 2 * 16 * 90 * 8 / 1e6 for s in sizes]
axes[1].bar(range(3), raw, color=colors, label="Raw ASTC blocks")
axes[1].bar(range(3), [r * .35 for r in raw], bottom=raw, color="#d7c9ed",
            label="Illustrative 35% overhead allowance")
for i, r in enumerate(raw):
    axes[1].text(i, r + .3, f"{r:.2f}", ha="center", fontsize=10)
axes[1].axhline(20, color="#b34056", linestyle="--")
axes[1].set_xticks(range(3), [f"{s}²" for s in sizes])
axes[1].set_ylabel("Stereo 90 Hz rate (Mbit/s)")
axes[1].set_title("Uncompressed backup budget estimate")
axes[1].legend(fontsize=8)
for ax in axes:
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", alpha=.15)
    ax.set_axisbelow(True)
fig.suptitle("Coarse safety stream feasibility — not a live fallback result", fontsize=15)
fig.savefig(ROOT / "thumbnail-budget.png", dpi=160)
