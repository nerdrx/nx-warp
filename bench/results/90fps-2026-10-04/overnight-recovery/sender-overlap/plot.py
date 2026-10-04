import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

base = Path(__file__).resolve().parent
with (base / "results/sender-wait.csv").open() as f:
    rows = list(csv.DictReader(f))

def percentile(mode, p):
    values = sorted(float(r["cycle_ms"]) for r in rows if r["mode"] == mode)
    assert len(values) == 100
    return values[int((len(values) - 1) * p)]

plt.rcParams.update({"figure.facecolor": "#101019", "axes.facecolor": "#171724",
                    "text.color": "#ededf4", "axes.labelcolor": "#ededf4",
                    "xtick.color": "#ededf4", "ytick.color": "#ededf4",
                    "axes.edgecolor": "#55556a", "font.size": 11})
fig, axes = plt.subplots(1, 2, figsize=(11, 4), constrained_layout=True)
for ax, rate in zip(axes, (250, 500)):
    for shift, mode, label, color in ((-.18, "legacy", "Wait before backend", "#8585a5"),
                                    (.18, "deferred", "Wait after backend", "#be8cff")):
        values = [percentile(f"{mode}_{rate}", p) for p in (.5, .95)]
        bars = ax.bar([shift, 1 + shift], values, .34, label=label, color=color)
        ax.bar_label(bars, labels=[f"{v:.2f}" for v in values], padding=4, color="#ededf4")
    ax.set_xticks([0, 1], ["p50", "p95"])
    ax.set_ylim(0, 25)
    ax.set_title(f"Simulated {rate} Mbit/s FIFO")
    ax.set_ylabel("Complete PC cycle (ms)")
    ax.grid(axis="y", alpha=.13)
axes[1].legend(loc="upper left", facecolor="#171724", labelcolor="#ededf4", fontsize=9)
fig.suptitle("Sender overlap: mixed tails, production option rejected", fontsize=15)
fig.text(.5, -.045, "100 samples/condition; native stereo; observed GPU busy 99–100%; no Wi-Fi/headset timing",
         ha="center", color="#b6b6cb", fontsize=10)
fig.savefig(base / "sender-overlap.png", dpi=160, bbox_inches="tight")
