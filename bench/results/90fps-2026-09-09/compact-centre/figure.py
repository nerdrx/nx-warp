#!/usr/bin/env python3
"""Draw the fixed-axis compact-centre storage map (no measurements)."""
from pathlib import Path
import argparse
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

def compact_eye(ax, x0, label):
    ax.add_patch(Rectangle((x0, 0), 928, 928, facecolor="#e8b86a",
                           edgecolor="white", linewidth=1.5))
    # Separable mapping leaves native-resolution cross-stripes through the
    # centre even though the corners are quarter-density.
    ax.add_patch(Rectangle((x0 + 208, 0), 512, 928,
                           facecolor="#7bc8bb", edgecolor="none"))
    ax.add_patch(Rectangle((x0, 208), 928, 512,
                           facecolor="#7bc8bb", edgecolor="none"))
    ax.add_patch(Rectangle((x0 + 208, 208), 512, 512,
                           facecolor="#43b7a5", edgecolor="#166b63",
                           linewidth=2))
    ax.text(x0 + 464, 464, "native\n512 × 512", ha="center", va="center",
            fontsize=8.5, color="white", fontweight="bold")
    ax.text(x0 + 104, 464, "outer ×¼", ha="center", va="center",
            rotation=90, fontsize=9)
    ax.text(x0 + 824, 464, "outer ×¼", ha="center", va="center",
            rotation=90, fontsize=9)
    ax.text(x0 + 464, 958, label, ha="center", va="bottom", fontsize=10,
            fontweight="bold")

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args(); a.out.mkdir(parents=True, exist_ok=True)
    fig, axs = plt.subplots(1, 2, figsize=(12, 5.8),
                            gridspec_kw={"width_ratios": [1, 1.35]})
    src, dst = axs
    src.set_xlim(0, 2176); src.set_ylim(2176, 0); src.set_aspect("equal")
    src.add_patch(Rectangle((0, 0), 2176, 2176, facecolor="#f3f4f6",
                            edgecolor="#333333", linewidth=1.2))
    src.add_patch(Rectangle((832, 832), 512, 512, facecolor="#43b7a5",
                            edgecolor="#166b63", linewidth=2))
    src.axvline(832, color="#b87919", linestyle="--", linewidth=1)
    src.axvline(1344, color="#b87919", linestyle="--", linewidth=1)
    src.axhline(832, color="#b87919", linestyle="--", linewidth=1)
    src.axhline(1344, color="#b87919", linestyle="--", linewidth=1)
    src.text(1088, 1088, "native\n512 × 512", ha="center", va="center",
             fontsize=9, color="#166b63", fontweight="bold")
    src.set_title("Source eye: 2176 × 2176")
    src.set_xlabel("source x (pixels)"); src.set_ylabel("source y (pixels)")
    dst.set_xlim(0, 1856); dst.set_ylim(980, 0); dst.set_aspect("equal")
    compact_eye(dst, 0, "eye 0")
    compact_eye(dst, 928, "eye 1")
    dst.set_title("Packed stereo luma: 1856 × 928")
    dst.set_xlabel("compact x (pixels)"); dst.set_ylabel("compact y (pixels)")
    dst.set_xticks([])
    dst.grid(axis="x", color="#ffffff", linewidth=1)
    fig.suptitle("Fixed-axis compact-centre mapping", fontsize=14,
                 fontweight="bold")
    fig.text(0.5, 0.01,
             "Each outer axis keeps representative source coordinate x mod 4 = 1; "
             "the 512 × 512 centre remains native. Chroma uses the same map at half size.",
             ha="center", fontsize=9)
    fig.tight_layout(rect=[0, 0.10, 1, 0.95])
    fig.savefig(a.out / "compact-centre-map.png", dpi=160)

if __name__ == "__main__":
    main()
