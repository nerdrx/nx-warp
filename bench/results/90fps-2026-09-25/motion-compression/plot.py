#!/usr/bin/env python3
"""Render the motion-compression summary as a compact three-panel PNG."""
import argparse
import json
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


HERE = Path(__file__).resolve().parent
COLORS = {
    "bg": "#100d18",
    "panel": "#191526",
    "grid": "#39334a",
    "text": "#f2eef8",
    "muted": "#b9b2c8",
    "baseline": "#777386",
    "violet": "#b88aff",
    "light": "#e0c9ff",
    "cyan": "#70d6d1",
    "bad": "#ff7d9b",
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", type=Path, default=HERE / "summary.json")
    parser.add_argument("--output", type=Path, default=HERE / "comparison.png")
    args = parser.parse_args()
    data = json.loads(args.summary.read_text())

    plt.rcParams.update({
        "figure.facecolor": COLORS["bg"],
        "axes.facecolor": COLORS["panel"],
        "axes.edgecolor": COLORS["grid"],
        "axes.labelcolor": COLORS["text"],
        "xtick.color": COLORS["muted"],
        "ytick.color": COLORS["muted"],
        "text.color": COLORS["text"],
        "font.family": "DejaVu Sans",
        "font.size": 9,
    })
    fig, (size_ax, decode_ax, scope_ax) = plt.subplots(
        1, 3, figsize=(17, 6.4), gridspec_kw={"width_ratios": [0.95, 1.15, 1.8]}
    )
    fig.suptitle("MOTION RESIDUAL COMPRESSION  /  OFFLINE EVIDENCE",
                 x=0.055, y=0.98, ha="left", fontsize=15,
                 fontweight="bold", color=COLORS["text"])
    fig.text(0.055, 0.935,
             "Controlled duplicate-eye photo fixtures · byte-exact · independent reference frame and transport overhead excluded",
             color=COLORS["muted"], fontsize=9)

    # Panel 1: detail size on the two controlled global-translation wins.
    wins = data["global_translation_results"][:2]
    labels = [f"Capture {item['capture']}\n+8 px" for item in wins]
    x = np.arange(len(wins))
    width = 0.32
    baseline_kb = np.array([item["baseline_bytes"] for item in wins]) / 1000
    proposal_kb = np.array([item["proposal_bytes"] for item in wins]) / 1000
    size_ax.bar(x - width / 2, baseline_kb, width, color=COLORS["baseline"], label="Independent")
    size_ax.bar(x + width / 2, proposal_kb, width, color=COLORS["violet"], label="Motion residual")
    for i, item in enumerate(wins):
        size_ax.text(i + width / 2, proposal_kb[i] + 1.4,
                     f"−{item['saved_percent']:.1f}%", ha="center",
                     color=COLORS["light"], fontsize=8, fontweight="bold")
    size_ax.set_title("01  DETAIL SIZE", loc="left", color=COLORS["text"], fontweight="bold")
    size_ax.set_ylabel("kB · safety/FEC/padding excluded")
    size_ax.set_xticks(x, labels)
    size_ax.set_ylim(0, max(baseline_kb) * 1.22)
    size_ax.legend(frameon=False, labelcolor=COLORS["muted"], fontsize=8, loc="upper right")

    # Panel 2: latest NEON Pico decode distributions, not encoder or presentation time.
    modes = ["baseline", "candidate cached reference", "candidate uncached reference"]
    mode_labels = ["Baseline", "Motion cached", "Motion uncached"]
    mode_colors = [COLORS["baseline"], COLORS["violet"], COLORS["light"]]
    percentiles = ["p50_ms", "p95_ms", "p99_ms"]
    percentile_labels = ["p50", "p95", "p99"]
    measurements = data["pico_decode"]["measurements"]
    xx = np.arange(3)
    for capture, marker, dash in [("A", "o", "-"), ("B", "s", "--")]:
        for mode, mode_label, color in zip(modes, mode_labels, mode_colors):
            row = next(item for item in measurements
                       if item["capture"] == capture and item["mode"] == mode)
            yy = [row[p] for p in percentiles]
            # Use hue for decoder mode and line style for capture.
            decode_ax.plot(xx, yy, marker=marker, linestyle=dash, color=color,
                           linewidth=1.8, markersize=4,
                           label=mode_label if capture == "A" else None)
    decode_ax.set_title("02  PICO DECODE", loc="left", color=COLORS["text"], fontweight="bold")
    decode_ax.set_ylabel("ms · decode only")
    decode_ax.set_xticks(xx, percentile_labels)
    decode_ax.legend(frameon=False, labelcolor=COLORS["muted"], fontsize=7, loc="upper left")
    decode_ax.text(0.98, 0.98, "solid A  /  dashed B", transform=decode_ax.transAxes,
                   ha="right", va="top", fontsize=7, color=COLORS["muted"])

    # Panel 3: savings (negative means a larger proposal); include local-tile contrast.
    scope = []
    for item in data["global_translation_results"]:
        scope.append((f"Global · {item['capture']} · {item['case']}", item["saved_percent"], "global"))
    for item in data["local_tile_results"]:
        scope.append((f"Per-tile · {item['capture']} · {item['case']}", item["saved_percent"], "local"))
    # Keep chart compact while preserving the decisive fallback cases.
    chosen = [entry for entry in scope if any(key in entry[0] for key in
              ("scene cut", "rotation", "translation 32", "translation 64", "+8 px"))]
    chosen.sort(key=lambda entry: entry[1])
    chosen = [(label.replace("Global · ", "G ").replace("Per-tile · ", "Tile ")
                     .replace("Capture ", "").replace("controlled translation", "")
                     .replace("translation ", "shift ").replace("rotation ", "rot ")
                     .replace("degrees", "°").replace("degree", "°"), value, kind)
               for label, value, kind in chosen]
    ypos = np.arange(len(chosen))
    vals = [entry[1] for entry in chosen]
    colors = [COLORS["violet"] if value >= 10 else COLORS["cyan"] if value > 0 else COLORS["bad"]
              for value in vals]
    scope_ax.barh(ypos, vals, color=colors, height=0.72)
    scope_ax.axvline(0, color=COLORS["muted"], linewidth=0.8)
    scope_ax.axvline(10, color=COLORS["muted"], linewidth=0.6, linestyle=":")
    for y, value in zip(ypos, vals):
        scope_ax.text(value + (0.8 if value >= 0 else -0.8), y,
                      f"{value:+.1f}%", va="center",
                      ha="left" if value >= 0 else "right", fontsize=7,
                      color=COLORS["text"])
    scope_ax.set_yticks(ypos, [entry[0] for entry in chosen], fontsize=7)
    scope_ax.set_title("03  MOTION SCOPE / FALLBACK", loc="left",
                       color=COLORS["text"], fontweight="bold")
    scope_ax.set_xlabel("Saved bytes (%) · 10% selection gate · G = global")
    scope_ax.grid(axis="x", color=COLORS["grid"], linewidth=0.6)
    scope_ax.set_axisbelow(True)
    scope_ax.set_xlim(min(vals) - 7, max(vals) + 12)

    for ax in (size_ax, decode_ax, scope_ax):
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.grid(axis="y", color=COLORS["grid"], linewidth=0.55, alpha=0.75)
        ax.set_axisbelow(True)
    scope_ax.grid(axis="y", visible=False)
    fig.text(0.055, 0.025,
             "Decode samples: 160 per mode × 3 modes × 2 pairs. Motion estimates are fixture-specific; ACK/cache-loss and live transport are untested.",
             fontsize=8, color=COLORS["muted"])
    fig.tight_layout(rect=(0.045, 0.06, 0.99, 0.91), w_pad=1.4)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=180, facecolor=fig.get_facecolor(), bbox_inches="tight")
    print(args.output)


if __name__ == "__main__":
    main()
