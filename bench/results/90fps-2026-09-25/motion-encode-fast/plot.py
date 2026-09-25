#!/usr/bin/env python3
"""Render recorded offline encoder timings and the separate regional probe."""
import csv
import json
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).resolve().parent
BG, PANEL, TEXT, MUTED = "#100d18", "#191526", "#f2eef8", "#b9b2c8"
GREY, VIOLET, CYAN = "#777386", "#b88aff", "#70d6d1"
plt.rcParams.update({
    "figure.facecolor": BG, "axes.facecolor": PANEL,
    "axes.edgecolor": "#39334a", "text.color": TEXT,
    "axes.labelcolor": TEXT, "xtick.color": MUTED, "ytick.color": MUTED,
    "font.family": "DejaVu Sans", "font.size": 11,
})


def canvas(title, subtitle):
    fig, ax = plt.subplots(figsize=(12, 6))
    fig.subplots_adjust(left=.09, right=.97, top=.79, bottom=.2)
    fig.text(.09, .93, title, fontsize=19, weight="bold")
    fig.text(.09, .875, subtitle, fontsize=10, color=MUTED)
    ax.grid(axis="y", alpha=.12)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    return fig, ax


def main():
    data = json.loads((HERE / "production/summary.json").read_text())
    keys = ["scene_a_motion", "scene_b_motion", "scene_a_to_b_cut_then_static", "scene_a_zero_ack_fallback"]
    labels = ["Photo A\nmoving centre", "Photo B\nmoving centre",
              "Post-cut static\nrepeated centre", "No usable ACK\nindependent fallback"]
    fig, ax = canvas("LESS ENCODER WORK. SAME RECONSTRUCTED PIXELS.",
                     "Offscreen production codec · photographic native centres / fixed synthetic periphery · 48 samples per arm and case")
    x = np.arange(len(keys))
    for mode, offset, color, label in [("baseline", -.18, GREY, "Original motion encoder"),
                                        ("candidate", .18, VIOLET, "Optimized encoder")]:
        medians = [data[k][mode]["p50_ms"] for k in keys]
        tails = [data[k][mode]["p95_ms"] for k in keys]
        ax.bar(x + offset, medians, .34, color=color, label=label)
        ax.errorbar(x + offset, medians,
                    yerr=[np.zeros(len(keys)), np.subtract(tails, medians)],
                    fmt="none", ecolor=TEXT, capsize=4, linewidth=1)
        for pos, value, tail in zip(x + offset, medians, tails):
            ax.text(pos, tail + .16, f"{value:.2f}", ha="center", fontsize=10)
    ax.set_xticks(x, labels)
    ax.set_ylabel("Encode time (ms) · bars p50 / whiskers p95")
    ax.set_ylim(0, max(data[k][m]["p95_ms"] for k in keys for m in ("baseline", "candidate")) * 1.2)
    ax.legend(frameon=False, labelcolor=TEXT, fontsize=10)
    fig.text(.09, .06, "Same payload sizes in these fixtures. No live FPS or photon-latency claim. The independent fallback control is slower in this run.", fontsize=9, color=MUTED)
    fig.savefig(HERE / "encoder-cost.png", dpi=150)
    plt.close(fig)

    with (HERE / "mixed-motion/results.csv").open() as f:
        rows = list(csv.DictReader(f))
    fig, ax = canvas("NEXT EXPERIMENT: MORE THAN ONE MOTION VECTOR",
                     "Artificial native-centre region translations · estimated shifts · exact reconstruction · not integrated or measured on Pico")
    x = np.arange(len(rows))
    for key, offset, color, label in [("global_saved_pct", -.18, GREY, "One global vector"),
                                      ("local_saved_pct", .18, CYAN, "Two / four fixed regions")]:
        values = [float(r[key]) for r in rows]
        ax.bar(x + offset, values, .34, color=color, label=label)
        for pos, value in zip(x + offset, values):
            ax.text(pos, value + (1.5 if value >= 0 else -4), f"{value:.1f}%", ha="center", fontsize=10)
    ax.axhline(10, color=MUTED, linestyle="--", linewidth=1)
    ax.axhline(0, color=MUTED, linewidth=.7)
    ax.set_xticks(x, ["Photo A\nleft / right", "Photo B\ntop / bottom", "Photo A\nfour quadrants", "Scene cut\nfallback selected"])
    ax.set_ylim(-21, 77)
    ax.set_ylabel("Detail bytes saved vs independent encoding (%)")
    ax.legend(frameon=False, labelcolor=TEXT, fontsize=10, loc="upper left")
    fig.text(.09, .06, "Dashed line: 10% selection gate. Includes prototype vector metadata; excludes reference delivery, safety, FEC and transport padding.", fontsize=9, color=MUTED)
    fig.savefig(HERE / "regional-motion.png", dpi=150)
    plt.close(fig)


if __name__ == "__main__":
    main()
