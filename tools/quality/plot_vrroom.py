#!/usr/bin/env python3
"""Figures for the vrroom headline table (ADR-0029).

Reads the JSON the vrtable harness writes and produces the two figures the
gallery carries.  Palette is the validated categorical default (slots 1-3);
the aqua slot fails the 3:1 contrast check against the surface, so every
series is BOTH legended and direct-labelled and carries its own marker shape --
identity is never colour alone.

  python3 tools/quality/plot_vrroom.py --in <work5> --out docs/assets
"""
import argparse, json, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#dcdbd6"
SERIES = {"all-ATLAS": "#2a78d6", "all-PICTURE": "#eb6834", "D=8": "#1baf7a"}
MARKER = {"all-ATLAS": "o", "all-PICTURE": "s", "D=8": "^"}
FIX = [("rest", "rest, 2.7 deg/s"), ("mid", "mid, 26.2 deg/s"),
       ("fast", "fast, 99.1 deg/s"), ("objmotion", "object motion, head at rest")]


def load(indir):
    rows = []
    for f, _ in FIX:
        p = os.path.join(indir, f"vrtable-{f}.json")
        if os.path.exists(p):
            rows += json.load(open(p))
    return rows


def fig_rd(rows, out):
    fig, axes = plt.subplots(2, 2, figsize=(11, 8.4), facecolor=SURFACE)
    for ax, (fx, title) in zip(axes.ravel(), FIX):
        ax.set_facecolor(SURFACE)
        for mode, colour in SERIES.items():
            pts = sorted([(r["bpf"], r["psnr"], r["qp"]) for r in rows
                          if r["fixture"] == fx and r["mode"] == mode
                          and r["effort"] == "e0" and r["planar"] == "rd"])
            if not pts:
                continue
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            ax.plot(xs, ys, color=colour, linewidth=2.0,
                    marker=MARKER[mode], markersize=8, markeredgecolor=SURFACE,
                    markeredgewidth=1.2, label=mode, zorder=3)
            # Direct label at the high-rate end: identity without colour.
            # Staggered per series because all-ATLAS and D=8 land on the same
            # point wherever the mode switch picks ATLAS, and two labels on one
            # point is worse than none.
            dy = {"all-ATLAS": 11, "all-PICTURE": 0, "D=8": -13}[mode]
            ax.annotate(mode, (xs[-1], ys[-1]), textcoords="offset points",
                        xytext=(8, dy), fontsize=8.5, color=INK2)
        ax.set_xscale("log")
        # Plain integers on the rate axis: matplotlib's default log labels
        # collide at these ranges and read as noise.
        ax.xaxis.set_major_locator(LogLocator(base=10.0, subs=(1.0, 2.0, 5.0),
                                              numticks=8))
        ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{int(v):,}"))
        ax.xaxis.set_minor_formatter(NullFormatter())
        ax.grid(True, which="major", color=GRID, linewidth=0.8, zorder=0)
        ax.set_axisbelow(True)
        for sp in ("top", "right"):
            ax.spines[sp].set_visible(False)
        for sp in ("left", "bottom"):
            ax.spines[sp].set_color(GRID)
        ax.tick_params(colors=INK2, labelsize=9)
        ax.set_title(title, color=INK, fontsize=11, loc="left", pad=8)
        ax.set_xlabel("bytes per frame (log)", color=INK2, fontsize=9)
        ax.set_ylabel("luma PSNR, dB", color=INK2, fontsize=9)
    h, l = axes[0][0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=3, frameon=False,
               fontsize=9.5, labelcolor=INK2, bbox_to_anchor=(0.5, -0.005))
    fig.suptitle("Figure 1  Rate-distortion on the vrroom corpus: QP 26 / 34 / 40",
                 color=INK, fontsize=13, x=0.055, ha="left", y=0.98)
    for ax in axes.ravel():
        ax.margins(x=0.16)
    fig.tight_layout(rect=(0, 0.05, 1, 0.95))
    fig.savefig(out, dpi=110, facecolor=SURFACE)
    plt.close(fig)


def fig_effort(rows, out):
    """The effort ladder and the planar mode, as DELTA from e0 + planar rd --
    the point being that the deltas are nearly zero, which a pair of absolute
    bars would hide inside the axis."""
    cfgs = [("e0", "rd"), ("e1", "rd"), ("trellis", "rd"),
            ("e0", "prefer"), ("e1", "prefer"), ("trellis", "prefer")]
    labels = ["effort 0", "effort 1", "trellis",
              "effort 0\nplanar prefer", "effort 1\nplanar prefer",
              "trellis\nplanar prefer"]
    fig, ax = plt.subplots(figsize=(10, 4.6), facecolor=SURFACE)
    ax.set_facecolor(SURFACE)
    fixcol = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
    fixmk = ["o", "s", "^", "D"]
    for fi, (fx, title) in enumerate(FIX):
        base = [r for r in rows if r["fixture"] == fx and r["effort"] == "e0"
                and r["planar"] == "rd" and r["qp"] == 34 and r["mode"] == "D=8"]
        if not base:
            continue
        b = base[0]["psnr"]
        xs, ys = [], []
        for ci, (e, p) in enumerate(cfgs):
            m = [r for r in rows if r["fixture"] == fx and r["effort"] == e
                 and r["planar"] == p and r["qp"] == 34 and r["mode"] == "D=8"]
            if m:
                xs.append(ci)
                ys.append(m[0]["psnr"] - b)
        ax.plot(xs, ys, color=fixcol[fi], marker=fixmk[fi], markersize=9,
                linewidth=0, markeredgecolor=SURFACE, markeredgewidth=1.2,
                label=title, zorder=3)
    ax.axhline(0.0, color=INK2, linewidth=1.0, zorder=2)
    ax.set_xticks(range(len(cfgs)))
    ax.set_xticklabels(labels, fontsize=9, color=INK2)
    ax.grid(True, axis="y", color=GRID, linewidth=0.8, zorder=0)
    ax.set_axisbelow(True)
    for sp in ("top", "right"):
        ax.spines[sp].set_visible(False)
    for sp in ("left", "bottom"):
        ax.spines[sp].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=9)
    ax.set_ylabel("dB against effort 0, planar rd", color=INK2, fontsize=9)
    ax.set_title("Figure 2  Effort ladder and planar mode, D=8 at QP 34: the "
                 "ladder does nothing, planar prefer costs",
                 color=INK, fontsize=12, loc="left", pad=10)
    ax.legend(frameon=False, fontsize=9, labelcolor=INK2, ncol=2,
              loc="center left")
    fig.tight_layout()
    fig.savefig(out, dpi=110, facecolor=SURFACE)
    plt.close(fig)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="indir",
                    default="/run/media/nerdrx/Lex/claude/nx-scratch/atlasprice/work5")
    ap.add_argument("--out", default="docs/assets")
    a = ap.parse_args()
    rows = load(a.indir)
    fig_rd(rows, os.path.join(a.out, "vrroom-rd.png"))
    fig_effort(rows, os.path.join(a.out, "vrroom-effort.png"))
    print("wrote", a.out)
