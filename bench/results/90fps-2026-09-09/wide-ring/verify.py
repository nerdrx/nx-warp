#!/usr/bin/env python3
"""Verify the wide PLANAR ring against the native stereo fixture outputs.

The script streams the 32-frame YUV420p files through memory maps; it never
writes or copies the source or decoded YUV payloads.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


W, H, FRAMES, EYES = 4352, 2176, 32, 2
EYE_W = W // EYES
Y_BYTES = W * H
UV_BYTES = Y_BYTES // 4
FRAME_BYTES = Y_BYTES + 2 * UV_BYTES


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ring_masks() -> list[tuple[str, np.ndarray]]:
    """Return native centre and PLANAR tile-distance masks for both eyes."""
    yy, xx = np.indices((H, W))
    eye = xx // EYE_W
    ex = xx % EYE_W
    cx0, cx1 = EYE_W // 2 - 256, EYE_W // 2 + 256
    cy0, cy1 = H // 2 - 256, H // 2 + 256
    inside = (ex >= cx0) & (ex < cx1) & (yy >= cy0) & (yy < cy1)
    tx = ex // 64
    ty = yy // 64
    left_tile, right_tile = cx0 // 64, (cx1 - 1) // 64
    top_tile, bottom_tile = cy0 // 64, (cy1 - 1) // 64
    dx = np.maximum(left_tile - tx, tx - right_tile)
    dy = np.maximum(top_tile - ty, ty - bottom_tile)
    dist = np.maximum(dx, dy)
    return [
        ("native centre 512x512", inside),
        ("fine 4px PLANAR cells (dist 1-4)", (~inside) & (dist <= 4)),
        ("normal 8px cells (dist 5-7)", (dist >= 5) & (dist <= 7)),
        ("merged 16px cells (dist 8-11)", (dist >= 8) & (dist <= 11)),
        ("merged 32px cells (dist >=12)", dist >= 12),
    ]


def plane_views(path: Path):
    raw = np.memmap(path, dtype=np.uint8, mode="r", shape=(FRAMES, FRAME_BYTES))
    for i in range(FRAMES):
        f = raw[i]
        yield f[:Y_BYTES].reshape(H, W), f[Y_BYTES:Y_BYTES + UV_BYTES].reshape(H // 2, W // 2), f[Y_BYTES + UV_BYTES:].reshape(H // 2, W // 2)


def y_frame(path: Path, index: int) -> np.ndarray:
    return next(y for i, (y, _, _) in enumerate(plane_views(path)) if i == index)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, default=Path("../../../../../nx-scratch/adaptive-tiles/large.yuv"))
    ap.add_argument("--default", dest="default_yuv", type=Path, default=Path("../../../../../nx-scratch/wide-ring/default.yuv"))
    ap.add_argument("--wide", dest="wide_yuv", type=Path, default=Path("../../../../../nx-scratch/wide-ring/wide.yuv"))
    ap.add_argument("--default-stream", type=Path, default=Path("../../../../../nx-scratch/wide-ring/default.nxv"))
    ap.add_argument("--wide-stream", type=Path, default=Path("../../../../../nx-scratch/wide-ring/wide.nxv"))
    ap.add_argument("--out", type=Path, default=Path(__file__).parent)
    args = ap.parse_args()
    masks = ring_masks()
    mask_pixels = {name: int(mask.sum()) for name, mask in masks}
    sums = {name: {"default": 0.0, "wide": 0.0} for name, _ in masks}
    counts = {name: 0 for name, _ in masks}
    centre_exact = {"max_abs": {"Y": 0, "U": 0, "V": 0}, "mismatched": 0}

    src = plane_views(args.source)
    default = plane_views(args.default_yuv)
    wide = plane_views(args.wide_yuv)
    for frame in range(FRAMES):
        src_y, src_u, src_v = next(src)
        def_y, def_u, def_v = next(default)
        wide_y, wide_u, wide_v = next(wide)
        for name, mask in masks:
            sums[name]["default"] += float(np.abs(src_y.astype(np.int16) - def_y.astype(np.int16))[mask].sum())
            sums[name]["wide"] += float(np.abs(src_y.astype(np.int16) - wide_y.astype(np.int16))[mask].sum())
            counts[name] += int(mask.sum())
        cmask = masks[0][1]
        for label, a, b in (("Y", def_y, wide_y), ("U", def_u, wide_u), ("V", def_v, wide_v)):
            # Native centre is 512x512 luma; chroma is sampled at the matching area.
            m = cmask if label == "Y" else cmask[::2, ::2]
            delta = np.abs(a.astype(np.int16) - b.astype(np.int16))
            centre_exact["max_abs"][label] = max(centre_exact["max_abs"][label], int(delta[m].max(initial=0)))
            centre_exact["mismatched"] += int(np.count_nonzero(delta[m]))
    ring_metrics = []
    for name, _ in masks:
        ring_metrics.append({"ring": name, "pixels": mask_pixels[name], "frames": FRAMES,
                             "default_luma_mae": sums[name]["default"] / counts[name],
                             "wide_luma_mae": sums[name]["wide"] / counts[name]})
    result = {
        "fixture": {"source": str(args.source), "width": W, "height": H, "frames": FRAMES, "eyes": EYES, "format": "yuv420p"},
        "native_centre_512x512_per_eye_all_planes_all_frames_exact": centre_exact,
        "decoded_all_planes_all_frames_equal": sha256(args.default_yuv) == sha256(args.wide_yuv),
        "streams": {"default_bytes": args.default_stream.stat().st_size, "wide_bytes": args.wide_stream.stat().st_size,
                    "delta_bytes": args.wide_stream.stat().st_size - args.default_stream.stat().st_size,
                    "default_sha256": sha256(args.default_stream), "wide_sha256": sha256(args.wide_stream)},
        "rings": ring_metrics,
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "metrics.json").write_text(json.dumps(result, indent=2) + "\n")
    # A small decoded crop makes the comparison auditable without packaging YUV.
    crop = (y_frame(args.default_yuv, 16)[512:1536, 576:1600],
            y_frame(args.wide_yuv, 16)[512:1536, 576:1600])
    make_figure(ring_metrics, args.out / "wide-ring.png", crop)
    print(json.dumps(result, indent=2))


def make_figure(rings, path: Path, crop) -> None:
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle
    labels = ["centre\n512²", "fine\n4px PLANAR", "normal\n8px", "merged\n16px", "merged\n32px"]
    colors = ["#202938", "#39a96b", "#e0aa35", "#e36b35", "#b83b5e"]
    fig, axes = plt.subplots(1, 4, figsize=(14, 4), gridspec_kw={"width_ratios": [1.2, 1, 1, 1]})
    ax0, ax1, ax2, ax3 = axes
    ax0.set_title("Per-eye ring geometry")
    # Transparent nested outlines: 512² centre, then 1024/1408/1920/2176px.
    sizes = [512, 1024, 1408, 1920, 2176]
    for size, color, label in reversed(list(zip(sizes, colors, labels))):
        lo = (2176 - size) / 2
        ax0.add_patch(Rectangle((lo, lo), size, size, fill=False, edgecolor=color, lw=2.2))
        ax0.text(lo + 8, lo + 18, label.replace("\n", " "), color=color, fontsize=8,
                 va="bottom", bbox={"facecolor": "white", "edgecolor": "none", "alpha": .75, "pad": 1})
    ax0.set_xlim(0, 2176); ax0.set_ylim(0, 2176); ax0.set_aspect("equal")
    ax0.set_xlabel("per-eye luma pixels"); ax0.set_ylabel("per-eye luma pixels")
    ax0.set_xticks([0, 512, 1024, 1536, 2176]); ax0.set_yticks([0, 512, 1024, 1536, 2176])
    x = np.arange(5)
    ax1.bar(x - .19, [r["default_luma_mae"] for r in rings], .38, label="default", color="#52698a")
    ax1.bar(x + .19, [r["wide_luma_mae"] for r in rings], .38, label="wide ring", color="#39a96b")
    ax1.set_xticks(x, labels, rotation=35, ha="right"); ax1.set_ylabel("source luma MAE")
    ax1.set_title("Source luma error")
    ax1.grid(axis="y", alpha=.25); ax1.legend(frameon=False, fontsize=8)
    ax2.imshow(crop[0], cmap="gray", vmin=0, vmax=255); ax2.set_title("decoded default\nframe 16 crop")
    ax3.imshow(crop[1], cmap="gray", vmin=0, vmax=255); ax3.set_title("decoded wide\nframe 16 crop")
    for ax in (ax2, ax3): ax.set_xticks([]); ax.set_yticks([])
    fig.suptitle("NXVC PLANAR wide-ring verification · 4352×2176 stereo · 32 frames", fontsize=12)
    fig.tight_layout(); fig.savefig(path, dpi=160); plt.close(fig)


if __name__ == "__main__":
    main()
