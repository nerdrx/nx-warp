#!/usr/bin/env python3
"""Render the offline adaptive-tile fixture as two small scientific figures."""
from pathlib import Path
import json

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


ROOT = Path(__file__).resolve().parent
WIDTH, HEIGHT, FRAMES = 1024, 512, 16
FRAME_BYTES = WIDTH * HEIGHT * 3 // 2


def read_luma(path: Path) -> np.ndarray:
    raw = np.fromfile(path, dtype=np.uint8)
    expected = FRAME_BYTES * FRAMES
    if raw.size < expected:
        raise ValueError(f"{path} has {raw.size} bytes; expected {expected}")
    # Y is the first WIDTH*HEIGHT bytes in each YUV420 frame.
    return np.stack([
        raw[i * FRAME_BYTES:i * FRAME_BYTES + WIDTH * HEIGHT]
        .reshape(HEIGHT, WIDTH) for i in range(FRAMES)
    ])


def cadence_figure(rows: np.ndarray) -> None:
    frames = rows[:, 0]
    fig, ax = plt.subplots(figsize=(8.2, 4.6))
    ax.plot(frames, rows[:, 1], "o-", label="fit", color="#1769aa")
    ax.plot(frames, rows[:, 2], "o-", label="reuse", color="#2e7d32")
    ax.plot(frames, rows[:, 3], "o-", label="hot", color="#c62828")
    ax.set(title="Adaptive tile cadence", xlabel="Frame", ylabel="Tile count")
    ax.set_xticks(frames)
    ax.grid(True, alpha=0.25)
    ax.legend(frameon=False, ncol=3)
    fig.tight_layout(rect=(0, 0.09, 1, 1))
    fig.text(0.5, 0.025, "Offline synthetic fixture; this is not a Pico screenshot",
             ha="center", fontsize=8, color="#555555")
    fig.savefig(ROOT / "cadence.png", dpi=160)
    plt.close(fig)


def comparison_figure(old: np.ndarray, on: np.ndarray) -> None:
    old_last, on_last = old[-1].astype(np.int16), on[-1].astype(np.int16)
    delta = np.abs(on_last - old_last)
    fig, axes = plt.subplots(1, 3, figsize=(12, 4.25), constrained_layout=True)
    for axis, image, title, cmap, vmax in (
        (axes[0], old_last, "Old decoded · frame 15", "gray", 255),
        (axes[1], on_last, "Adaptive decoded · frame 15", "gray", 255),
        (axes[2], delta, "|Δ luminance| · frame 15", "magma", max(1, int(delta.max()))),
    ):
        view = axis.imshow(image, cmap=cmap, vmin=0, vmax=vmax, interpolation="nearest")
        axis.set_xlabel("x (pixels)")
        axis.set_ylabel("y (pixels)")
        axis.set_title(title)
        fig.colorbar(view, ax=axis, fraction=0.046, pad=0.04)
    fig.suptitle("Luminance comparison — offline synthetic fixture (Y plane)")
    fig.savefig(ROOT / "decoded-comparison.png", dpi=160)
    plt.close(fig)


def main() -> None:
    with (ROOT / "checks.json").open() as stream:
        rows = np.asarray(json.load(stream)["cadence"], dtype=float)
    if rows.shape != (FRAMES, 5):
        raise ValueError(f"expected 16 cadence rows with 5 columns, got {rows.shape}")
    cadence_figure(rows)
    comparison_figure(read_luma(ROOT / "old-decoded.yuv"),
                      read_luma(ROOT / "on-decoded.yuv"))


if __name__ == "__main__":
    main()
