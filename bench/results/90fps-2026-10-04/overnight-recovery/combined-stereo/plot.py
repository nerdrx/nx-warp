import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
root = Path(__file__).resolve().parent
rows = list(csv.DictReader((root / "results/three-mode.csv").open()))
modes = ["serial_l3", "parallel_l1", "parallel_compact_l1"]
labels = ["Serial\nZstd 3", "Parallel\nZstd 1", "Parallel\ncompact + Zstd 1"]
p50, p95, sizes = [], [], []
for mode in modes:
    group = [r for r in rows if r["mode"] == mode]
    assert len(group) == 50
    times = sorted(float(r["wall_ms"]) for r in group)
    p50.append(times[int((len(times)-1)*.5)])
    p95.append(times[int((len(times)-1)*.95)])
    sizes.append((int(group[0]["eye0_packet_bytes"])+int(group[0]["eye1_packet_bytes"]))/1000)
fig, axes = plt.subplots(1, 2, figsize=(11, 4.6))
colors = ["#788296", "#9270dd", "#5623a6"]
for ax, values, title, unit in zip(axes, [p50, sizes], ["Stereo wall time", "Selected stereo packets"], ["ms", "decimal kB"]):
    ax.bar(range(3), values, color=colors, width=.55)
    ax.set_xticks(range(3), labels)
    ax.set_title(title)
    ax.set_ylabel(unit)
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", alpha=.2)
    for x, value in enumerate(values):
        ax.text(x, value + max(values)*.025, f"{value:.3f}" if unit == "ms" else f"{value:.1f}", ha="center")
    ax.set_ylim(0, max(values)*1.3)
axes[0].errorbar(range(3), p50, yerr=[[0]*3, [b-a for a,b in zip(p50,p95)]], fmt="none", capsize=5, color="#24202f", label="p50 bar; upper whisker p95")
axes[0].legend(frameon=False, fontsize=9)
fig.suptitle("Native ASTC 8x8: matched offscreen PC stage", fontsize=16)
fig.text(.5, .025, "50 measured calls/mode, interleaved; exact ASTC within run. Upload, network and headset excluded.", ha="center", fontsize=9)
fig.tight_layout(rect=(0,.07,1,.93))
fig.savefig(root / "comparison.png", dpi=160)
