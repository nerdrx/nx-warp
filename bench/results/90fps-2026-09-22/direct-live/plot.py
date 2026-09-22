#!/usr/bin/env python3
"""Plot paired render pacing metrics extracted from summary.json."""
import json
from pathlib import Path
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
DATA = json.loads((ROOT / "summary.json").read_text())

def target_plot():
    x = list(range(len(DATA["automatic_target_mbit"])))
    fig, ax = plt.subplots(figsize=(6.4, 3.4), constrained_layout=True)
    ax.plot(x, DATA["automatic_target_mbit"], marker="o", color="#7700ff", linewidth=2)
    ax.set_xticks(x, [str(i + 1) for i in x])
    ax.set_xlabel("controller event")
    ax.set_ylabel("target bitrate (Mbit/s)")
    ax.set_title("Automatic target falls by controller event")
    ax.grid(alpha=0.25)
    fig.savefig(ROOT / "automatic-target.png", dpi=160)
    plt.close(fig)

def pacing_plot():
    labels = ["39/72", "adaptive\n500/90", "fixed before\n500/90", "fixed after\n500/90"]
    keys = ["low39_72", "adaptive500_90", "fixed500_before", "fixed500_after"]
    sub = [DATA["windows"][k]["submitted_layer_fps_mean"] for k in keys]
    fresh = [DATA["windows"][k]["fresh_update_fps_mean"] for k in keys]
    x = list(range(len(keys)))
    fig, ax = plt.subplots(figsize=(8.0, 4.2), constrained_layout=True)
    width = 0.36
    ax.bar([i - width / 2 for i in x], sub, width, label="submitted a layer", color="#7700ff")
    ax.bar([i + width / 2 for i in x], fresh, width, label="new-source", color="#00a6c7")
    ax.axhline(90, color="#555", linestyle="--", linewidth=0.8, label="90 Hz")
    ax.set_xticks(x, labels)
    ax.set_ylabel("frames/s (paired render intervals)")
    ax.set_title("Application pacing versus fresh updates")
    ax.grid(axis="y", alpha=0.25)
    ax.legend(fontsize=8)
    fig.savefig(ROOT / "freshness-vs-submission.png", dpi=160)
    plt.close(fig)

if __name__ == "__main__":
    target_plot()
    pacing_plot()
