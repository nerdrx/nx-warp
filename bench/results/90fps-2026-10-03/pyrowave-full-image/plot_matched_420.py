from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

out = Path(__file__).resolve().parent
plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "axes.titlesize": 12,
    "axes.labelsize": 10,
    "svg.fonttype": "none",
})
cdf = "#6554C0"
haar = "#168A65"
ink = "#273247"
muted = "#657184"
red = "#CC4654"

# Paired bars show median and tail latency for both device GPU time and CPU total.
fig, axes = plt.subplots(1, 2, figsize=(11.5, 5.0), sharey=True)
for ax, title, p50, p95 in (
    (axes[0], "GPU decode", (16.8462, 12.4169), (17.2323, 12.9234)),
    (axes[1], "Synchronous decode call", (21.1093, 15.3183), (21.5177, 16.5999)),
):
    x = np.array([0.0, 1.0])
    width = 0.30
    bars = []
    for vals, hatch, alpha in ((p50, None, 1.0), (p95, "//", 0.68)):
        for i, (value, color) in enumerate(zip(vals, (cdf, haar))):
            b = ax.bar(x[len(bars) // 2], value,
                       width, color=color, alpha=alpha, hatch=hatch,
                       edgecolor="white" if hatch is None else color, linewidth=0.7)
            bars.append(b[0])
    # Arrange paired CDF/Haar bars by percentile, rather than by implementation.
    for i, bar in enumerate(bars):
        group = i // 2
        slot = i % 2
        bar.set_x(x[group] + (-width / 2 if slot == 0 else width / 2))
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.24,
                f"{bar.get_height():.2f}", ha="center", va="bottom", fontsize=8, color=ink)
    ax.set_xticks(x, ("p50", "p95"))
    ax.set_title(title, color=ink, fontweight="bold", pad=12)
    ax.set_xlim(-0.55, 1.55)
    ax.grid(axis="y", color="#dbe1ea", linewidth=0.8)
    ax.set_axisbelow(True)
    ax.spines[["top", "right", "left"]].set_visible(False)
    ax.spines["bottom"].set_color("#aab3c0")
    ax.tick_params(axis="y", length=0, colors=muted)
    ax.tick_params(axis="x", length=0, pad=8, colors=ink)
    ax.axhline(1000 / 90, color=red, ls="--", lw=1.5, zorder=0)
    ax.set_ylim(0, 25)

axes[0].set_ylabel("Milliseconds per full 4352 × 2176 frame", color=muted)
from matplotlib.patches import Patch
legend = [Patch(facecolor=cdf, label="Standard CDF 9/7"),
          Patch(facecolor=haar, label="Fused paired Haar"),
          Patch(facecolor="white", edgecolor="#7c8798", hatch="//", label="p95 (p50 is solid)"),
          plt.Line2D([0], [0], color=red, ls="--", label="11.11 ms at 90 Hz")]
fig.legend(handles=legend, loc="lower center", ncol=4, frameon=False,
           bbox_to_anchor=(0.5, 0.12), fontsize=9)
fig.suptitle("Matched native 4:2:0 decode latency", fontsize=16, fontweight="bold", color=ink, y=0.98)
fig.text(0.5, 0.925,
         "Adreno 650 • ~694 KB fixtures from the same source • 12 warmups, 30 samples • readback excluded",
         ha="center", color=muted, fontsize=9)
fig.text(0.5, 0.045,
         "Both paths exceed the 90 Hz frame budget in this isolated full-frame decode test.",
         ha="center", color=ink, fontsize=9)
fig.tight_layout(rect=(0.02, 0.20, 0.98, 0.89), w_pad=2.5)
fig.savefig(out / "matched-420-latency.svg", bbox_inches="tight")
fig.savefig(out / "matched-420-latency.png", dpi=190, bbox_inches="tight")
plt.close(fig)

# Source MAE comparison for the same 4:2:0 input and near-matched fixture size.
labels = ["Y", "Cb", "Cr"]
cdf_mae = np.array([1.211457, 1.082256, 0.950094])
haar_mae = np.array([1.47926, 1.31090, 1.10053])
x = np.arange(len(labels))
width = 0.34
fig, ax = plt.subplots(figsize=(7.8, 4.8))
for vals, shift, color, label in ((cdf_mae, -width/2, cdf, "Standard CDF 9/7"),
                                 (haar_mae, width/2, haar, "Fused paired Haar")):
    rects = ax.bar(x + shift, vals, width, color=color, label=label, zorder=3)
    for r, value in zip(rects, vals):
        ax.text(r.get_x() + r.get_width()/2, value + 0.025, f"{value:.3f}",
                ha="center", va="bottom", fontsize=9, color=ink)
ax.set_xticks(x, labels)
ax.set_ylabel("Mean absolute error (8-bit code values)", color=muted)
ax.set_ylim(0, 1.75)
ax.set_title("Source reconstruction at matched 4:2:0 rate", fontsize=15,
             fontweight="bold", color=ink, pad=14)
ax.text(0.5, 1.02, "Same 4352 × 2176 stereo input • fixtures 694,352 B (CDF) / 694,304 B (Haar)",
        transform=ax.transAxes, ha="center", color=muted, fontsize=9)
ax.grid(axis="y", color="#dbe1ea", linewidth=0.8)
ax.set_axisbelow(True)
ax.spines[["top", "right", "left"]].set_visible(False)
ax.spines["bottom"].set_color("#aab3c0")
ax.tick_params(axis="y", length=0, colors=muted)
ax.tick_params(axis="x", length=0, pad=8, colors=ink)
ax.legend(frameon=False, loc="upper right")
fig.tight_layout()
fig.savefig(out / "matched-420-source-quality.svg", bbox_inches="tight")
fig.savefig(out / "matched-420-source-quality.png", dpi=190, bbox_inches="tight")
plt.close(fig)
