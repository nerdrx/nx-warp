import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
root = Path(__file__).resolve().parent
rows = list(csv.DictReader((root / "pico.csv").open()))
modes = ["ordinary_l3", "ordinary_l1", "compact_l1"]
labels = ["Ordinary\nZstd 3", "Ordinary\nZstd 1", "Compact\nZstd 1"]
p50, p95, sizes = [], [], []
for mode in modes:
    group = [r for r in rows if r["mode"] == mode]
    assert len(group) == 100
    times = sorted(float(r["pair_us"])/1000 for r in group)
    p50.append(times[int((len(times)-1)*.5)])
    p95.append(times[int((len(times)-1)*.95)])
    sizes.append((int(group[0]["left_bytes"])+int(group[0]["right_bytes"]))/1000)
fig, axes = plt.subplots(1, 2, figsize=(11, 4.6))
colors = ["#788296", "#9270dd", "#5623a6"]
for ax, values, title, unit in zip(axes, [p50, sizes], ["Sequential stereo strict decode", "Selected stereo packets"], ["ms", "decimal kB"]):
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
fig.suptitle("Native ASTC 8x8: Pico CPU component check", fontsize=16)
fig.text(.5, .025, "100 calls/mode, interleaved; exact ASTC. Unlocked clocks; GPU, network and display excluded.", ha="center", fontsize=9)
fig.tight_layout(rect=(0,.07,1,.93))
fig.savefig(root / "comparison.png", dpi=160)
