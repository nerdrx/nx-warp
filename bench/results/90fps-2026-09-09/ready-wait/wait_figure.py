#!/usr/bin/env python3
"""Plot ready-wait behavior from summarize.py's client-window summary."""
import json
from pathlib import Path

import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent


def main():
    summary = json.loads((ROOT / "summary.json").read_text())
    labels, attempts, successes, per_wait = [], [], [], []
    for row in summary:
        means = row["client"]["means"]
        labels.append(Path(row["file"]).stem.replace("-recovered", ""))
        a = means.get("wait_attempts", 0)
        s = means.get("wait_successes", 0)
        total = means.get("wait_total_ms", 0)
        attempts.append(a)
        successes.append(s)
        per_wait.append(total / a if a else 0)

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.5), layout="constrained")
    positions = range(len(labels))
    width = 0.36
    bars_a = axes[0].bar([p - width / 2 for p in positions], attempts, width, label="attempts", color="#607d8b")
    bars_s = axes[0].bar([p + width / 2 for p in positions], successes, width, label="successes", color="#2a9d8f")
    axes[0].bar_label(bars_a, fmt="%.1f", padding=2)
    axes[0].bar_label(bars_s, fmt="%.1f", padding=2)
    axes[0].set_title("Mean waits per window")
    axes[0].set_ylabel("count")
    axes[0].set_xticks(list(positions), labels, rotation=18)
    axes[0].legend()
    axes[1].bar(labels, per_wait, color="#bc6c25")
    axes[1].set_title("Average time per attempted wait")
    axes[1].set_ylabel("milliseconds")
    axes[1].tick_params(axis="x", rotation=18)
    axes[1].bar_label(axes[1].containers[0], fmt="%.3f", padding=2)
    for axis in axes:
        axis.margins(y=0.18)
        axis.grid(axis="y", alpha=0.25)
        axis.set_axisbelow(True)
    fig.suptitle("Ready-wait behavior (approximately 2-second window means)")
    fig.supxlabel("Configured cap is nominal; it is not a wall-time guarantee.", fontsize=9)
    fig.savefig(ROOT / "wait-behavior.png", dpi=170)


if __name__ == "__main__":
    main()
