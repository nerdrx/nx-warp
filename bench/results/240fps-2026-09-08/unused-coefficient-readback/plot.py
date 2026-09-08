"""Rebuild the figure from the measured summary; no fitted or inferred FPS."""
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
rows = json.loads((root / "summary.json").read_text())
fig, axes = plt.subplots(1, 2, figsize=(11, 4.5), layout="constrained")
colors = ["#526377", "#087e70", "#526377"]
for ax, key, title in zip(axes, ["encoder", "selection"],
                         ["Host encoder", "Encode start → render selection"]):
    for i, row in enumerate(rows):
        values = (row["profile_all_frames"]["p50_p95_p99_ms"] if key == "encoder"
                  else row["mapped_pipeline"]["stages"]["blit"]["p50_p95_p99_ms"])
        ax.plot([i, i], [values[0], values[2]], color=colors[i], linewidth=2)
        for value, marker, label in zip(values, ["o", "s", "^"], ["p50", "p95", "p99"]):
            ax.scatter(i, value, marker=marker, color=colors[i], s=48,
                       label=label if i == 1 else None, zorder=3)
        ax.annotate(f"{values[0]:.2f}", (i, values[0]), xytext=(9, -4),
                    textcoords="offset points", fontsize=10)
    ax.set(xticks=range(3), xticklabels=["Original", "Copy removed", "Original restored"],
           ylabel="Latency (ms)", title=title, xlim=(-0.4, 2.7), ylim=(0, None))
    ax.grid(axis="y", alpha=0.2)
    ax.spines[["top", "right"]].set_visible(False)
    ax.legend(frameon=False, loc="upper right")
axes[0].axhline(1000 / 240, color="#b45626", linestyle="--", linewidth=1)
axes[0].text(-0.32, 4.4, "4.17 ms stage budget", color="#b45626", fontsize=9)
fig.suptitle("Native stereo: removing an unused 55 MiB coefficient copy", fontsize=15)
fig.supxlabel("Pico 4 · QP40 Lite · target cache enabled · ordered live runs\n"
               "Markers are percentiles, not confidence intervals. Selection is not photon latency; no 240 FPS claim.",
               fontsize=9)
fig.savefig(root / "coefficient-copy-comparison.png", dpi=180)
