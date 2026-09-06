#!/usr/bin/env python3
"""Figures for docs/COMPOSITOR-POSE-DISPLAY.md.

Two pictures, both from `nxv-posestats --csv`:

  atlasdec-dominant-pose.png   how much of the atlas shares one pose, and how
                               many tiles each display scheme therefore warps
  atlasdec-display-path.png    the path itself: atlas -> dominant-pose
                               composite -> compositor timewarp

Nothing here reads a device.  The counts are tile counts, not milliseconds.
"""
import argparse
import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

ACCENT = "#7700FF"
INK = "#1b1b1f"
MUTED = "#8a8a94"
GRID = "#e3e3ea"


def load(path):
    arms = defaultdict(list)
    with open(path) as f:
        for row in csv.DictReader(f):
            arms[row["arm"]].append(
                dict(
                    deg=float(row["deg_per_frame"]),
                    frame=int(row["frame"]),
                    picture=int(row["picture"]),
                    valid=int(row["valid"]),
                    dominant=int(row["dominant"]),
                    poses=int(row["poses"]),
                    coded=int(row["coded"]),
                )
            )
    return arms


def fig_dominant(arms, out, disp):
    order = ["rest", "creep", "slow", "mid", "fast"]
    order = [a for a in order if a in arms]
    colours = [ACCENT, "#0aa1a1", "#e08600", "#c02a5e", MUTED]

    fig, (ax0, ax1) = plt.subplots(
        2, 1, figsize=(9.2, 7.4), sharex=True,
        gridspec_kw=dict(height_ratios=[1.0, 1.15], hspace=0.16))

    for a, c in zip(order, colours):
        rows = arms[a][1:]
        xs = [r["frame"] for r in rows]
        ys = [100.0 * r["dominant"] / r["valid"] if r["valid"] else 0.0
              for r in rows]
        deg = arms[a][0]["deg"]
        pic = sum(r["picture"] for r in rows)
        lab = f"{a}  {deg:.2f}°/frame ({deg*90:.0f}°/s)"
        if pic:
            lab += f"  — {pic}/{len(rows)} PICTURE"
        ax0.plot(xs, ys, color=c, lw=2.0, label=lab)
    ax0.set_ylabel("entries at the dominant pose  (%)")
    ax0.set_ylim(0, 104)
    ax0.axhline(100, color=GRID, lw=1, zorder=0)
    ax0.grid(True, color=GRID, lw=0.8)
    ax0.set_axisbelow(True)
    ax0.legend(loc="lower right", fontsize=8.5, framealpha=0.95, ncol=2)
    ax0.set_title(
        f"Compositor-pose display  —  1088×1088, 289 tiles, one eye, "
        f"QP 28, D={disp}", color=INK, fontsize=11, pad=10)

    # Only the arms where the two schemes differ carry information here.
    for a, c in zip(order, colours):
        rows = arms[a][1:]
        xs = [r["frame"] for r in rows]
        new = [r["valid"] - r["dominant"] for r in rows]
        today = [r["valid"] - r["coded"] for r in rows]
        ax1.plot(xs, today, color=c, lw=1.2, ls="--", alpha=0.75)
        # The at-rest arm is the headline: a flat zero against a flat 289.
        ax1.plot(xs, new, color=c, lw=3.2 if a == "rest" else 2.0,
                 zorder=5 if a == "rest" else 3)
    ax1.set_ylabel("tiles warped by the display pass")
    ax1.set_xlabel("frame")
    ax1.grid(True, color=GRID, lw=0.8)
    ax1.set_axisbelow(True)
    ax1.set_ylim(-14, 310)
    from matplotlib.lines import Line2D
    ax1.legend(handles=[
        Line2D([], [], color=INK, lw=2.0, label="proposal: not at the dominant pose"),
        Line2D([], [], color=INK, lw=1.2, ls="--", label="today: every entry the frame did not code"),
    ], loc="center right", fontsize=8.5, framealpha=0.95)
    ax1.annotate("289 = every tile (a PICTURE frame's assembly)",
                 xy=(0.99, 289), xycoords=("axes fraction", "data"),
                 ha="right", va="bottom", fontsize=8, color=MUTED)
    ax1.axhline(289, color=MUTED, lw=0.9, ls=":", zorder=0)

    for ax in (ax0, ax1):
        for sp in ax.spines.values():
            sp.set_color(GRID)
        ax.tick_params(colors=MUTED, labelsize=9)
        ax.yaxis.label.set_color(INK)
        ax.xaxis.label.set_color(INK)
    fig.savefig(out, dpi=150, bbox_inches="tight", facecolor="white")
    plt.close(fig)


def fig_path(out):
    fig, ax = plt.subplots(figsize=(11.0, 5.4))
    ax.set_xlim(0, 10.4)
    ax.set_ylim(0, 5.4)
    ax.axis("off")

    def box(x, y, w, h, title, lines, edge=INK, face="white", tc=INK):
        ax.add_patch(FancyBboxPatch(
            (x, y), w, h, boxstyle="round,pad=0.08,rounding_size=0.10",
            linewidth=1.4, edgecolor=edge, facecolor=face))
        ax.text(x + w / 2, y + h - 0.30, title, ha="center", va="center",
                fontsize=10.5, color=tc, fontweight="bold")
        # Lines are CENTRED on their baseline, and the block is sized from the
        # line count, so a two-line box and a four-line box both fit their
        # text instead of the last line falling through the border.
        top = y + h - 0.70
        for i, ln in enumerate(lines):
            ax.text(x + w / 2, top - i * 0.30, ln, ha="center", va="center",
                    fontsize=8.6, color=MUTED)

    def arrow(x0, y0, x1, y1, colour=INK):
        ax.add_patch(FancyArrowPatch(
            (x0, y0), (x1, y1), arrowstyle="-|>", mutation_scale=15,
            linewidth=1.5, color=colour, shrinkA=2, shrinkB=2))

    box(0.15, 2.30, 2.5, 2.00, "The atlas",
        ["u16, coded domain", "one entry per tile", "each at its OWN pose",
         "NORMATIVE — untouched"], edge=ACCENT)
    box(3.35, 3.00, 3.0, 1.30, "at the dominant pose",
        ["straight sample, NO warp", "100% at rest, ~62% at 9°/s"],
        edge="#0aa1a1")
    box(3.35, 1.30, 3.0, 1.30, "at some other pose",
        ["warp to the DOMINANT pose", "not to the current one"],
        edge="#e08600")
    box(7.05, 2.30, 3.0, 2.00, "Composite",
        ["stamped with the", "DOMINANT pose", "(one per eye)"], edge=INK)

    arrow(2.65, 3.55, 3.35, 3.65)
    arrow(2.65, 3.05, 3.35, 2.00)
    arrow(6.35, 3.65, 7.05, 3.55)
    arrow(6.35, 2.00, 7.05, 3.05)

    ax.add_patch(FancyBboxPatch(
        (7.05, 0.62), 3.0, 1.28,
        boxstyle="round,pad=0.08,rounding_size=0.10", linewidth=1.4,
        linestyle="--", edgecolor=MUTED, facecolor="#fafafd"))
    ax.text(8.55, 1.60, "Pico compositor", ha="center", va="center",
            fontsize=10.5, color=MUTED, fontweight="bold")
    ax.text(8.55, 1.22, "re-warps to the latest pose", ha="center",
            va="center", fontsize=8.6, color=MUTED)
    ax.text(8.55, 0.92, "happens anyway — free", ha="center", va="center",
            fontsize=8.6, color=MUTED)
    arrow(8.55, 2.30, 8.55, 1.94, colour=MUTED)

    ax.text(5.2, 5.10,
            "Display path: the second warp is not avoided, it is USED",
            ha="center", va="center", fontsize=11.5, color=INK,
            fontweight="bold")
    ax.text(5.2, 4.72,
            "at rest 100% of entries take the no-warp path; at 9°/s about 62%",
            ha="center", va="center", fontsize=8.8, color=MUTED)
    ax.text(5.2, 0.18,
            "Display-only. No atlas pixel, no byte of the 64-byte table, and "
            "nothing conformance compares depends on any of this.",
            ha="center", va="center", fontsize=8.4, color=MUTED,
            style="italic")
    fig.savefig(out, dpi=150, bbox_inches="tight", facecolor="white")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--disp", default="8")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    arms = load(a.csv)
    fig_dominant(arms, os.path.join(a.out, "atlasdec-dominant-pose.png"), a.disp)
    fig_path(os.path.join(a.out, "atlasdec-display-path.png"))
    print("wrote atlasdec-dominant-pose.png and atlasdec-display-path.png")


if __name__ == "__main__":
    main()
