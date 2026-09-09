#!/usr/bin/env python3
"""Plot live compact comparison from the archived last-30-window means."""
import json
from pathlib import Path

import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
SUMMARY = ROOT / "live-summary.json"


def main():
    rows = json.loads(SUMMARY.read_text())
    labels, fresh, gpu, offset = [], [], [], []
    for row in rows:
        name = Path(row["file"]).stem.removeprefix("live-").removesuffix("-client")
        labels.append(name)
        fresh.append(row["client"]["means"]["fresh_per_s"])
        gpu.append(row["decoder"]["means"]["nxvc_gpu_ms"])
        offset.append(row["client"]["means"]["source_offset_ms"])

    panels = (("Fresh updates / s", fresh), ("Decoder GPU (ms)", gpu),
              ("Source offset (ms)", offset))
    fig, axes = plt.subplots(1, 3, figsize=(11, 4.2))
    colors = ("#6c757d", "#1769aa", "#2a9d8f", "#8a5aa8")
    for axis, (title, values) in zip(axes, panels):
        bars = axis.bar(labels, values, color=colors, width=0.62)
        axis.set_title(title)
        axis.set_ylim(0, max(values) * 1.12)
        axis.grid(axis="y", alpha=0.25)
        axis.set_axisbelow(True)
        axis.tick_params(axis="x", rotation=18)
        for bar, value in zip(bars, values):
            axis.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                      f"{value:.2f}", ha="center", va="bottom", fontsize=9)
    fig.suptitle("Live compact decoder comparison (last 30 window means)")
    fig.tight_layout()
    fig.savefig(ROOT / "live-comparison.png", dpi=170)


if __name__ == "__main__":
    main()
