#!/usr/bin/env python3
"""Render one frame of `nxv-enc --rc-map` as a per-tile QP / res / skip picture.

The CSV `nxv-enc --rc --rc-map` writes carries every per-tile decision the
rate controller made.  This turns one frame of it into the three panels that
say what the allocator did with the picture: what QP each tile got, which
tiles the foveation ladder resampled, and which tiles the temporal ladder held
back.  The class map is drawn alongside, because the first two only make sense
once you can see which tiles the classifier called text.

    python3 percept_map_png.py --csv rc-full-40.csv --frame 4 --out map.png

SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import argparse
import csv
import math

import numpy as np
from PIL import Image, ImageDraw

#: Tile cell size in pixels; scaled per run so a 4x4 map and a 32x16 one both
#: come out legible.  MIN_W keeps the panel titles from being clipped.
CELL = 22
PAD = 34           # gutter for titles and the legend
MIN_W = 620
CLASS_NAMES = ("flat", "texture", "edge", "text")


def load(path: str, frame: int) -> dict:
    rows = []
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            if int(r["frame"]) == frame:
                rows.append(r)
    if not rows:
        raise SystemExit(f"{path}: no rows for frame {frame}")
    eyes = max(int(r["eye"]) for r in rows) + 1
    tx = max(int(r["col"]) for r in rows) + 1
    ty = max(int(r["row"]) for r in rows) + 1
    shape = (ty, tx * eyes)
    out = {k: np.zeros(shape, dtype=float) for k in
           ("qp", "res_level", "force_skip", "coded", "class", "ecc_deg")}
    for r in rows:
        y = int(r["row"])
        x = int(r["eye"]) * tx + int(r["col"])
        for k in out:
            out[k][y, x] = float(r[k])
    out["_eyes"] = eyes
    out["_tx"] = tx
    return out


def ramp(v: np.ndarray, lo: float, hi: float, cold, hot) -> np.ndarray:
    """Linear two-colour ramp, cold at *lo* and hot at *hi*."""
    t = np.clip((v - lo) / max(1e-6, hi - lo), 0.0, 1.0)[..., None]
    return (np.asarray(cold) * (1 - t) + np.asarray(hot) * t).astype(np.uint8)


def panel(rgb: np.ndarray, title: str, eyes: int, tx: int, cell: int) -> Image.Image:
    h, w = rgb.shape[:2]
    img = Image.fromarray(rgb).resize((w * cell, h * cell), Image.NEAREST)
    canvas = Image.new("RGB", (max(img.width, MIN_W), img.height + PAD),
                       (18, 18, 22))
    canvas.paste(img, (0, PAD))
    d = ImageDraw.Draw(canvas)
    d.text((4, 10), title, fill=(235, 235, 240))
    # The eye seam, so a stereo map is not read as one wide picture.
    for e in range(1, eyes):
        x = e * tx * cell
        d.line([(x, PAD), (x, PAD + img.height)], fill=(150, 150, 160), width=1)
    return canvas


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", required=True)
    ap.add_argument("--frame", type=int, default=4)
    ap.add_argument("--out", required=True)
    ap.add_argument("--title", default=None)
    args = ap.parse_args(argv)

    m = load(args.csv, args.frame)
    eyes, tx = m["_eyes"], m["_tx"]

    qp = m["qp"]
    qp_rgb = ramp(qp, float(qp.min()), float(qp.max()), (30, 60, 130), (250, 210, 60))

    res = m["res_level"]
    res_rgb = np.zeros(res.shape + (3,), dtype=np.uint8)
    for lvl, colour in ((0, (40, 45, 55)), (1, (90, 150, 210)), (2, (235, 120, 80))):
        res_rgb[res == lvl] = colour

    skip = m["force_skip"]
    coded = m["coded"]
    skip_rgb = np.zeros(skip.shape + (3,), dtype=np.uint8)
    skip_rgb[:] = (40, 45, 55)                       # coded, nothing asked
    skip_rgb[(coded == 0) & (skip == 0)] = (110, 110, 120)   # encoder's own skip
    skip_rgb[skip == 1] = (200, 70, 90)                       # temporal ladder

    cls = m["class"]
    cls_rgb = np.zeros(cls.shape + (3,), dtype=np.uint8)
    for c, colour in ((0, (55, 60, 70)), (1, (70, 130, 90)),
                      (2, (200, 170, 70)), (3, (220, 90, 200))):
        cls_rgb[cls == c] = colour

    cols = qp.shape[1]
    cell = max(12, min(40, MIN_W // max(1, cols)))

    panels = [
        panel(qp_rgb, f"coded QP  {qp.min():.0f} (blue) .. {qp.max():.0f} (yellow)",
              eyes, tx, cell),
        panel(res_rgb, "res_level: 0 full (dark) / 1 half (blue) / 2 quarter (orange)",
              eyes, tx, cell),
        panel(skip_rgb, "skip: red = temporal ladder, grey = encoder's own WARP_SKIP",
              eyes, tx, cell),
        panel(cls_rgb, "class: flat (dark) / texture (green) / edge (amber) / text (pink)",
              eyes, tx, cell),
    ]
    gap = 10
    width = max(p.width for p in panels)
    height = sum(p.height for p in panels) + gap * (len(panels) - 1) + PAD
    sheet = Image.new("RGB", (width, height), (18, 18, 22))
    d = ImageDraw.Draw(sheet)
    d.text((4, 10), args.title or f"{args.csv}  frame {args.frame}",
           fill=(255, 255, 255))
    y = PAD
    for p in panels:
        sheet.paste(p, (0, y))
        y += p.height + gap
    sheet.save(args.out)
    print(f"[png] {args.out}  {sheet.width}x{sheet.height}, "
          f"{m['qp'].shape[0]}x{m['qp'].shape[1]} tiles, {eyes} eye(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
