import csv
from pathlib import Path
import matplotlib.pyplot as plt

rows = list(csv.DictReader(open("results/zstd-compact.csv", newline="")))
conditions = ["ordinary_l3", "ordinary_l1", "compact_l1", "compact_l3"]
labels = ["Raw ASTC\nZstd 3", "Raw ASTC\nZstd 1", "14 B blocks\nZstd 1", "14 B blocks\nZstd 3"]
colors = ["#46657d", "#56a983", "#e0a442", "#bc6a60"]

def pct(values, q):
    values = sorted(values)
    return values[int((len(values) - 1) * q)]

summary = {}
for name in conditions:
    group = [r for r in rows if r["condition"] == name]
    cpu = [float(r["two_eye_serial_cpu_ms"]) for r in group]
    wire250 = [float(r["wire_250_mbps_ms"]) for r in group]
    wire500 = [float(r["wire_500_mbps_ms"]) for r in group]
    summary[name] = {
        "cpu50": pct(cpu, .5), "cpu95": pct(cpu, .95),
        "model250": pct(cpu, .5) + pct(wire250, .5),
        "model500": pct(cpu, .5) + pct(wire500, .5),
    }

fig, axes = plt.subplots(1, 3, figsize=(12.4, 3.9), layout="constrained")
x = range(4)
for ax, metric, title, ylabel in [
    (axes[0], "cpu50", "Measured CPU", "ms per two-eye packet pair"),
    (axes[1], "model250", "CPU + wire model at 250 Mbit/s", "ms per two-eye pair"),
    (axes[2], "model500", "CPU + wire model at 500 Mbit/s", "ms per two-eye pair"),
]:
    vals = [summary[c][metric] for c in conditions]
    bars = ax.bar(x, vals, color=colors, width=.68)
    ax.set_title(title, fontsize=10)
    ax.set_ylabel(ylabel, fontsize=8)
    ax.set_xticks(list(x), labels, fontsize=8)
    ax.grid(axis="y", alpha=.22)
    ax.set_axisbelow(True)
    for bar, val in zip(bars, vals):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + max(vals)*.018,
                f"{val:.2f}", ha="center", va="bottom", fontsize=8)
    ax.set_ylim(0, max(vals) * 1.16)

fig.suptitle("Native q6 ASTC packet compression: pinned CPU probe", fontsize=12)
fig.text(.5, -.035, "Wire time is calculated from packet bytes and bitrate; no network was used.",
         ha="center", fontsize=8, color="#555555")
out = Path("results")
fig.savefig(out / "zstd-level-comparison.png", dpi=180, bbox_inches="tight")
fig.savefig(out / "zstd-level-comparison.svg", bbox_inches="tight")
