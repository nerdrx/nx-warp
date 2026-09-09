#!/usr/bin/env python3
"""Plot the offline live-cost summary from live.json."""
from pathlib import Path
import json

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent
COLORS = ("#4c78a8", "#59a14f", "#f28e2b", "#9b59b6", "#00a0a0")
LABELS = {"cadence-control": "control", "transform-bypass": "bypass",
          "transform-repeat": "repeat", "pace-accumulate": "paced",
          "pace-confirm": "confirm"}


def main() -> None:
    with (ROOT / "live.json").open() as stream:
        records = json.load(stream)
    labels = [LABELS[row["label"]] for row in records]
    encode = [row["server"]["last30_means"]["encode_ms"] for row in records]
    fresh = [row["client"]["client"]["means"]["fresh_per_s"] for row in records]
    offset = [row["client"]["client"]["means"]["source_offset_ms"] for row in records]

    fig, axes = plt.subplots(1, 3, figsize=(10.2, 4.4), constrained_layout=False)
    panels = (
        (axes[0], encode, "Server encode time", "ms", "lower is better"),
        (axes[1], fresh, "Fresh updates", "updates / s", "higher is better"),
        (axes[2], offset, "Source offset", "ms", "NOT physical latency"),
    )
    x = np.arange(len(labels))
    for axis, data, title, unit, note in panels:
        bars = axis.bar(x, data, color=COLORS, width=0.66)
        axis.set_title(title + "\n" + note, fontsize=10)
        axis.set_ylabel(unit)
        axis.set_xticks(x, labels, rotation=20, fontsize=8)
        axis.grid(axis="y", alpha=0.25)
        axis.set_axisbelow(True)
        for bar, value in zip(bars, data):
            axis.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                      f"{value:.2f}", ha="center", va="bottom", fontsize=8)
    fig.suptitle("90 Hz Pico: transform bypass and pacing; temporal caching off", fontsize=13)
    fig.subplots_adjust(top=0.78, bottom=0.19, wspace=0.30)
    fig.text(0.5, 0.03,
             "Source offset is a producer timestamp offset; it does not measure physical end-to-end latency.",
             ha="center", fontsize=8, color="#555")
    fig.savefig(ROOT / "live-cost.png", dpi=170)
    plt.close(fig)


if __name__ == "__main__":
    main()
