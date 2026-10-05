#!/usr/bin/env python3
"""Plot archived matched microbenchmark rows; never rerun timed work."""
from pathlib import Path
import csv, statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p = Path(__file__).resolve().parent
rows = list(csv.DictReader((p / "reproduced/timing.raw.csv").open()))
assert len(rows) == 60
fig, axes = plt.subplots(1, 2, figsize=(10, 4.5), layout="constrained")
for ax, field, label in zip(axes, ["wall_ms", "process_cpu_ms"], ["Elapsed wall time", "Process CPU time"]):
    values = {t: {int(r["pair"]): float(r[field]) * 1000 for r in rows if r["treatment"] == t}
              for t in ["legacy", "direct"]}
    assert set(values["legacy"]) == set(values["direct"]) == set(range(30))
    for pair in range(30):
        ax.plot([0, 1], [values["legacy"][pair], values["direct"][pair]], color="#aaa", alpha=.45, lw=.7)
    for i, treatment in enumerate(values):
        samples = list(values[treatment].values())
        ax.scatter([i] * 30, samples, s=18, color=["#666", "#7946d2"][i], zorder=2)
        ax.scatter(i, statistics.median(samples), s=130, marker="_", color="black", zorder=3)
    ax.set_xticks([0, 1], ["Flatten then copy", "Direct ring copy"])
    ax.set_ylabel(label + " (µs / modeled eye-frame)")
    ax.set_ylim(bottom=0)
    ax.grid(axis="y", alpha=.2)
fig.suptitle("Scratch history-copy gate: 30 matched pairs, all samples shown")
fig.supxlabel("500 Mbit/s stereo / 90 Hz payload model · history ON / FEC OFF\nNo socket send, GPU, Pico, or live latency measurement. Direct publication ordering differs.", fontsize=9)
fig.savefig(p / "paired-cost.png", dpi=170)
