#!/usr/bin/env python3
"""Bounded offline phase/edge/4:2:0 checks for projected ASTC endpoints."""
import json
import math
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(__file__).resolve().parent
FIX = Path("/run/media/nerdrx/Lex/claude/nx-scratch/astc-native-colour-20261004/crops")
DEC = Path("/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003/decode_astc")
ENC = {
    "base": ROOT / "base/build/astc-gpu",
    "projected": ROOT / "build/astc-gpu",
}
W, H = 1920, 1080


def run(cmd, **kw):
    return subprocess.run([str(x) for x in cmd], check=True, capture_output=True, **kw)


def rgba_bytes(rgb):
    alpha = np.full((*rgb.shape[:2], 1), 255, dtype=np.uint8)
    return np.concatenate((rgb.astype(np.uint8), alpha), axis=2).tobytes()


def encode_decode(rgb, mode, quality, tag, work):
    raw, astc, decoded = (work / f"{tag}.{ext}" for ext in ("rgba", "astc", "decoded.rgba"))
    raw.write_bytes(rgba_bytes(rgb))
    h, w = rgb.shape[:2]
    # Each binary loads `build/encode.spv` relative to cwd; use its own build root.
    run([ENC[mode], raw, astc, w, h, 3, quality, "resident"], cwd=ENC[mode].parent.parent)
    run([DEC, astc, decoded])
    expected = w * h * 4
    if decoded.stat().st_size != expected:
        raise RuntimeError(f"decoder byte count {decoded.stat().st_size}, expected {expected}: {tag}")
    pixels = np.fromfile(decoded, dtype=np.uint8).reshape(h, w, 4)[:, :, :3].copy()
    blocks = astc.read_bytes()[16:]
    zstd = run(["zstd", "-3", "-q", "-c"], input=blocks).stdout
    # ASTC and temporary files are intentionally deleted when the temp dir closes.
    return pixels, len(blocks), len(zstd)


def psnr(a, b):
    d = a.astype(np.float64) - b.astype(np.float64)
    mse = float(np.mean(d * d))
    return 99.0 if mse == 0 else 10 * math.log10(255.0**2 / mse)


def motion_phases(src, mode, quality, work, tag):
    phase_rows, aligned_prev, aligned_rms = [], None, []
    payload, raw_payload = [], []
    for shift in range(8):
        moved = np.roll(src, shift, axis=1)
        dec, raw_n, zstd_n = encode_decode(moved, mode, quality, f"{tag}-p{shift}", work)
        phase_rows.append({"phase": shift, "psnr_rgb_db": round(psnr(moved, dec), 5)})
        payload.append(zstd_n)
        raw_payload.append(raw_n)
        # Undo the synthetic horizontal motion, then compare adjacent decoded frames.
        aligned = np.roll(dec, -shift, axis=1)[16:-16, 16:-16].astype(np.int16)
        if aligned_prev is not None:
            delta = aligned - aligned_prev
            aligned_rms.append(float(np.sqrt(np.mean(delta.astype(np.float64) ** 2))))
        aligned_prev = aligned
    return {
        "mode": mode, "quality": quality, "phases": phase_rows,
        "mean_psnr_rgb_db": round(float(np.mean([r["psnr_rgb_db"] for r in phase_rows])), 5),
        "motion_aligned_adjacent_rms_rgb": round(float(np.mean(aligned_rms)), 5),
        "synthetic_grid_stability_only": True,
        "mean_raw_astc_payload_bytes": round(float(np.mean(raw_payload)), 2),
        "mean_zstd3_payload_bytes": round(float(np.mean(payload)), 2),
    }


def rgb_to_420_nearest(rgb):
    f = rgb.astype(np.float32) / 255.0
    r, g, b = f[..., 0], f[..., 1], f[..., 2]
    y = 0.2126*r + 0.7152*g + 0.0722*b
    cb = (b-y)/1.8556 + 0.5
    cr = (r-y)/1.5748 + 0.5
    h, w = y.shape
    cb420 = cb.reshape(h//2, 2, w//2, 2).mean((1, 3))
    cr420 = cr.reshape(h//2, 2, w//2, 2).mean((1, 3))
    cbn = np.repeat(np.repeat(cb420, 2, 0), 2, 1)
    crn = np.repeat(np.repeat(cr420, 2, 0), 2, 1)
    out = np.stack((y+1.5748*(crn-.5), y-.187324*(cbn-.5)-.468124*(crn-.5), y+1.8556*(cbn-.5)), -1)
    return np.rint(np.clip(out, 0, 1)*255).astype(np.uint8)


def edge_cases(work):
    cases = []
    # Constant 1x1 / q0 exercises degenerate covariance and a legal 1-block ASTC image.
    flat = np.array([[[73, 73, 73]]], dtype=np.uint8)
    for mode in ("base", "projected"):
        d, raw_n, zstd_n = encode_decode(flat, mode, 0, f"flat1-{mode}", work)
        cases.append({"case": "constant-1x1-q0", "mode": mode, "valid": bool(d.shape == flat.shape and np.isfinite(d).all()), "decoded_pixel_rgb": [int(x) for x in d[0, 0]], "raw_payload_bytes": raw_n, "zstd3_bytes": zstd_n})
    # Odd extent exercises ceil-divided blocks and edge-clamped source sampling.
    yy, xx = np.mgrid[0:9, 0:17]
    odd = np.stack(((xx*13+yy*7)%256, (xx*3+yy*29)%256, (xx*19+yy*11)%256), -1).astype(np.uint8)
    for mode in ("base", "projected"):
        d, raw_n, zstd_n = encode_decode(odd, mode, 6, f"odd17x9-{mode}", work)
        cases.append({"case": "odd-17x9-q6", "mode": mode, "valid": bool(d.shape == odd.shape and np.isfinite(d).all()), "psnr_rgb_db": round(psnr(odd, d), 5), "raw_payload_bytes": raw_n, "zstd3_bytes": zstd_n})
    if not all(c["valid"] for c in cases):
        raise RuntimeError("edge-case assertion failed")
    return cases


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    report = {"purpose": "offline synthetic pan phase and endpoint edge validation", "photos_are_private_inputs": True, "motion_alignment_excludes_border_px": 16, "scenes": {}, "edge_cases": None}
    with tempfile.TemporaryDirectory(prefix="astc-projected-validation-") as tmp:
        work = Path(tmp)
        for scene in ("dark", "forest"):
            p = FIX / f"{scene}-native-1920x1080.png"
            with Image.open(p) as im:
                src = np.asarray(im.convert("RGB"), dtype=np.uint8)
            if src.shape != (H, W, 3):
                raise RuntimeError(f"unexpected fixture dimensions: {scene} {src.shape}")
            rows = []
            for quality in (2, 6):
                for mode in ("base", "projected"):
                    rows.append(motion_phases(src, mode, quality, work, f"{scene}-q{quality}-{mode}"))
            # Approximate full-range BT.709 4:2:0 box-average + nearest reconstruction.
            subsampled = rgb_to_420_nearest(src)
            variants = []
            for mode in ("base", "projected"):
                d, raw_n, zstd_n = encode_decode(subsampled, mode, 6, f"{scene}-420-q6-{mode}", work)
                variants.append({"mode": mode, "input_vs_original_psnr_rgb_db": round(psnr(subsampled, src), 5), "decoded_vs_original_psnr_rgb_db": round(psnr(src, d), 5), "decoded_vs_420_input_psnr_rgb_db": round(psnr(subsampled, d), 5), "raw_astc_payload_bytes": raw_n, "zstd3_bytes": zstd_n})
            report["scenes"][scene] = {"motion": rows, "approx_420_q6": variants}
        report["edge_cases"] = edge_cases(work)
    path = OUT / "validation.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(path)


if __name__ == "__main__":
    main()
