#!/usr/bin/env python3
"""CPU preview of reprojection.glsl peripheral_smooth == 6."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parent
FIX = Path("/run/media/nerdrx/Lex/claude/nx-scratch/astc-partition-rate-neutral-20261004/qpolicy-reconstructed")
CASES = {
    "dark-purple-hair": (FIX / "dark.rgba", (320, 240, 512, 512)),
    "forest-white-hair": (FIX / "forest.rgba", (704, 128, 512, 512)),
}
W, H = 1920, 1080
Y709 = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def smooth(src: np.ndarray) -> tuple[np.ndarray, dict]:
    """Mirror shader's centre .5 + two +/- diagonal taps .25 each."""
    x = np.arange(W)
    y = np.arange(H)
    # Production taps are at +/- three texels on both axes; clamp to eye bounds.
    p = src[np.clip(y[:, None] + 3, 0, H - 1), np.clip(x[None, :] + 3, 0, W - 1), :3].astype(np.float32) / 255.0
    m = src[np.clip(y[:, None] - 3, 0, H - 1), np.clip(x[None, :] - 3, 0, W - 1), :3].astype(np.float32) / 255.0
    c = src[:, :, :3].astype(np.float32) / 255.0
    delta = ((p + m) * 0.25 - c * 0.5)
    delta -= np.sum(delta * Y709, axis=2, keepdims=True)
    room = np.where(delta > 0.0, 1.0 - c, c)
    limits = np.where(np.abs(delta) > 1e-6, room / np.maximum(np.abs(delta), 1e-6), 1.0)
    amount = np.clip(np.min(limits, axis=2, keepdims=True), 0.0, 1.0)
    out = c + delta * amount
    # Approximate UNORM attachment conversion; retain alpha as shader does.
    rgba = np.empty_like(src)
    rgba[:, :, :3] = np.clip(np.floor(out * 255.0 + 0.5), 0, 255).astype(np.uint8)
    rgba[:, :, 3] = src[:, :, 3]
    outside = np.maximum(-out, 0.0) + np.maximum(out - 1.0, 0.0)
    metrics = {
        "max_abs_luma_delta_float": float(np.max(np.abs(np.sum((out - c) * Y709, axis=2)))),
        "max_abs_luma_delta_code_after_unorm": float(np.max(np.abs(np.sum((rgba[:, :, :3].astype(np.float32) - src[:, :, :3].astype(np.float32)) * Y709, axis=2)))),
        "float_channels_outside_gamut_by_more_than_1e-6": int(np.count_nonzero(outside > 1e-6)),
        "max_float_gamut_overshoot": float(np.max(outside)),
        "unorm8_output_in_range": bool(rgba[:, :, :3].min() >= 0 and rgba[:, :, :3].max() <= 255),
        "alpha_changed_pixels": int(np.count_nonzero(rgba[:, :, 3] != src[:, :, 3])),
        "changed_rgb_pixels": int(np.count_nonzero(np.any(rgba[:, :, :3] != src[:, :, :3], axis=2))),
        "mean_abs_rgb_change_code": float(np.mean(np.abs(rgba[:, :, :3].astype(np.float32) - src[:, :, :3].astype(np.float32)))),
    }
    return rgba, metrics


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    report = {
        "title": "ASTC q6 decoded crop CPU preview of peripheral_smooth=6",
        "dimensions": [W, H],
        "fixtures": {},
        "method": "CPU reference of current client/shaders/reprojection.glsl mode 6: two diagonal taps at +/-3 texels, centre .5 and taps .25 each; subtract BT.709 encoded-RGB luma from delta; gamut-scale before UNORM8 output.",
        "limits": ["CPU preview only; no GPU readback or device validation", "Only local 512x512 crops are emitted", "No PSNR or quality-win claim"],
    }
    for name, (path, roi) in CASES.items():
        raw = np.fromfile(path, dtype=np.uint8)
        if raw.size != W * H * 4:
            raise ValueError(f"{path}: expected {W*H*4} bytes, got {raw.size}")
        src = raw.reshape(H, W, 4)
        dst, stats = smooth(src)
        x, y, w, h = roi
        before = Image.fromarray(src[y:y+h, x:x+w, :3], "RGB")
        after = Image.fromarray(dst[y:y+h, x:x+w, :3], "RGB")
        panel = Image.new("RGB", (w * 2, h + 32), "#15131d")
        panel.paste(before, (0, 32)); panel.paste(after, (w, 32))
        draw = ImageDraw.Draw(panel)
        draw.text((8, 8), "q6 decoded · before", fill="white")
        draw.text((w + 8, 8), "mode 6 CPU preview · after", fill="white")
        figure = OUT / f"{name}-512-before-after.png"
        panel.save(figure, optimize=True)
        report["fixtures"][name] = {
            "path": str(path), "sha256": sha(path), "roi_xywh": list(roi),
            "preview": figure.name, "preview_sha256": sha(figure), **stats,
        }
    (OUT / "metrics.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
