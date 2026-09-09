#!/usr/bin/env python3
"""Render the proposed static 2160x2160 per-eye FDM density map.

The current stream_defoveator builds a separable strip from integer foveation
ratios (abs(index - centre) + 1). This figure documents the proposed static
Chebyshev-radius policy in the same pixel-center convention: (x + .5) * 16.
The values are nominal shading footprints, not encoded resolution.
"""
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import ListedColormap, BoundaryNorm
from matplotlib.patches import Rectangle


def main():
    width = height = 2160
    texel = 16
    cols = rows = width // texel
    centers = (np.arange(cols) + 0.5) * texel
    x, y = np.meshgrid(centers, centers)
    radius = np.maximum(np.abs(x - width / 2), np.abs(y - height / 2))
    density = np.where(radius <= 512, 255, np.where(radius <= 768, 128, 64))

    cmap = ListedColormap(("#d9e6f2", "#79a9d1", "#174a7e"))
    norm = BoundaryNorm((0, 96, 192, 256), cmap.N)
    fig, ax = plt.subplots(figsize=(8.4, 8.4))
    extent = (0, width, 0, height)
    ax.imshow(density, origin="lower", extent=extent, cmap=cmap, norm=norm,
              interpolation="nearest", aspect="equal")

    # Mark the source's nominal 512 px-wide center separately from the
    # proposed full-density 1024 px square (the mappings are not identical).
    ax.add_patch(Rectangle((width / 2 - 256, height / 2 - 256), 512, 512,
                           fill=False, linestyle="--", linewidth=2,
                           edgecolor="#f4a261", label="native source centre 512 px (approx.)"))
    ax.add_patch(Rectangle((width / 2 - 512, height / 2 - 512), 1024, 1024,
                           fill=False, linestyle="-", linewidth=1.8,
                           edgecolor="#ffd166", label="full-density 1024 px square"))
    ax.add_patch(Rectangle((width / 2 - 768, height / 2 - 768), 1536, 1536,
                           fill=False, linestyle=":", linewidth=1.7,
                           edgecolor="#264653", label="mid band boundary (768 px)"))
    ax.scatter([width / 2], [height / 2], marker="+", s=100, c="#e76f51",
               linewidths=1.8, zorder=3)
    ax.annotate("full 255\n(1×1 footprint)", (1080, 1080), xytext=(1110, 1160),
                color="white", fontsize=10,
                arrowprops={"arrowstyle": "-", "color": "#102a43"})
    ax.annotate("mid 128\n(2×2 footprint)", (1600, 1400), xytext=(1710, 1510),
                color="#102a43", fontsize=10,
                arrowprops={"arrowstyle": "-", "color": "#102a43"})
    ax.annotate("outer 64\n(4×4 footprint)", (200, 200), xytext=(250, 390),
                color="#102a43", fontsize=10,
                arrowprops={"arrowstyle": "-", "color": "#102a43"})

    ax.set_title("Proposed static FDM density — one 2160×2160 eye")
    ax.set_xlabel("horizontal pixel coordinate (texel centers at (x + 0.5) × 16)")
    ax.set_ylabel("vertical pixel coordinate")
    ax.set_xticks(np.arange(0, 2161, 360))
    ax.set_yticks(np.arange(0, 2161, 360))
    ax.grid(color="white", linewidth=0.35, alpha=0.35)
    ax.legend(loc="upper left", fontsize=9, framealpha=0.92)
    fig.text(0.5, 0.01,
             "Values are nominal 1×1 / 2×2 / 4×4 shading footprints; they are not encoded resolution.",
             ha="center", fontsize=9)
    fig.tight_layout(rect=(0, 0.035, 1, 1))
    out = Path(__file__).with_name("density-map.png")
    fig.savefig(out, dpi=180)
    print(out)


if __name__ == "__main__":
    main()
