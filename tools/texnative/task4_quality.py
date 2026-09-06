#!/usr/bin/env python3
"""Task 4(b)(c)(d): ASTC quality, rate and payload-entropy on inter_pan8.yuv.

Layout trick: each 64x64 tile is placed on a grid whose pitch is a whole number
of ASTC blocks (64 for 4x4/8x8, 66 for 6x6, edge-replicated pad), so every tile
owns an exact, disjoint set of ASTC blocks.  That reproduces the codec model
(tiles compressed independently) in ONE astcenc invocation per frame, and lets
per-tile payload bytes be sliced straight out of the .astc file.

Outputs:
  task4_psnr_tiles.csv     one row per (mode, frame, tile)
  task4_summary.json       aggregates + entropy/zstd results
"""
import csv, json, os, subprocess, sys
import numpy as np
from PIL import Image

A = "/run/media/nerdrx/Lex/claude/nx-scratch/texnative-host/tools/astc-encoder/build/Source/astcenc-avx2"
D = "/run/media/nerdrx/Lex/claude/nx-scratch/texnative-host"
SRC = "/run/media/nerdrx/Lex/claude/nx-scratch/inter_pan8.yuv"
PREFIX = ["chrt", "-i", "0", "taskset", "-c", "0-7", "nice", "-n", "19"]
W = H = 1088
CW = CH = 544
NF, NT, TILE = 12, 17, 64
FB = W * H + 2 * CW * CH

WORK = f"{D}/work"
os.makedirs(WORK, exist_ok=True)


def frames():
    raw = np.fromfile(SRC, dtype=np.uint8).reshape(NF, FB)
    ys = raw[:, : W * H].reshape(NF, H, W)
    us = raw[:, W * H : W * H + CW * CH].reshape(NF, CH, CW)
    vs = raw[:, W * H + CW * CH :].reshape(NF, CH, CW)
    return ys, us, vs


def astc(mode, src, out, block, extra=()):
    cmd = PREFIX + [A, mode, src, out, block, "-medium", "-j", "8", *extra]
    p = subprocess.run(cmd, capture_output=True, text=True)
    assert p.returncode == 0, p.stderr + p.stdout
    return p.stdout


def psnr(a, b):
    d = a.astype(np.float64) - b.astype(np.float64)
    mse = float(np.mean(d * d))
    if mse == 0:
        return float("inf"), 0.0
    return 10.0 * np.log10(255.0 * 255.0 / mse), mse


def order0_bytes(buf):
    """Bytes needed if each byte were coded with an ideal order-0 model."""
    cnt = np.bincount(np.frombuffer(buf, dtype=np.uint8), minlength=256).astype(np.float64)
    n = cnt.sum()
    p = cnt[cnt > 0] / n
    bits = -(p * np.log2(p)).sum() * n
    return bits / 8.0


def zstd19(buf):
    p = subprocess.run(
        PREFIX + ["zstd", "-19", "-c", "-q"], input=buf, capture_output=True
    )
    assert p.returncode == 0, p.stderr
    return len(p.stdout)


def main():
    ys, us, vs = frames()
    rows = []
    summary = {}

    # ---- Option B (chosen): luma-only ASTC, chroma as a separate half-res 2ch ASTC
    # cell/pitch per block size so tiles land on block boundaries
    cfg = {
        "4x4": ("luma_c64", 64, 4),
        "6x6": ("luma_c66", 66, 6),
        "8x8": ("luma_c64", 64, 8),
    }
    for block, (dirn, cell, bs) in cfg.items():
        bpr = cell // bs  # blocks per tile row
        nbx = NT * bpr  # blocks per sheet row
        per_frame_tile_psnr = []
        ent = {"raw": 0, "zstd": 0, "order0": 0.0}
        for f in range(NF):
            src = f"{D}/img/{dirn}/f{f:02d}.png"
            rec = f"{WORK}/rec_{block}_f{f:02d}.png"
            comp = f"{WORK}/c_{block}_f{f:02d}.astc"
            astc("-tl", src, rec, block)
            astc("-cl", src, comp, block)
            R = np.array(Image.open(rec).convert("L"))
            for r in range(NT):
                for c in range(NT):
                    o = ys[f][r * TILE : (r + 1) * TILE, c * TILE : (c + 1) * TILE]
                    d = R[r * cell : r * cell + TILE, c * cell : c * cell + TILE]
                    pv, mse = psnr(o, d)
                    rows.append(["astc_luma_" + block, f, r * NT + c, f"{pv:.4f}", f"{mse:.5f}"])
                    per_frame_tile_psnr.append(pv)
            if f == 0:
                payload = open(comp, "rb").read()[16:]
                assert len(payload) == nbx * nbx * 16, (len(payload), nbx)
                ent["raw"] = len(payload)
                ent["zstd"] = zstd19(payload)
                ent["order0"] = order0_bytes(payload)
        v = np.array(per_frame_tile_psnr, dtype=np.float64)
        v = v[np.isfinite(v)]
        summary["astc_luma_" + block] = {
            "n": int(v.size),
            "mean": float(v.mean()),
            "median": float(np.median(v)),
            "p5": float(np.percentile(v, 5)),
            "p25": float(np.percentile(v, 25)),
            "min": float(v.min()),
            "max": float(v.max()),
            "std": float(v.std()),
            "entropy_frame0": ent,
        }
        print(block, summary["astc_luma_" + block]["mean"], flush=True)

    # ---- chroma leg of option B: half-res U,V in R,G
    ccfg = {"4x4": ("chroma_c32", 32, 4), "6x6": ("chroma_c36", 36, 6), "8x8": ("chroma_c32", 32, 8)}
    for block, (dirn, cell, bs) in ccfg.items():
        bpr = cell // bs
        nbx = NT * bpr
        pu, pv_ = [], []
        ent = {}
        for f in range(NF):
            src = f"{D}/img/{dirn}/f{f:02d}.png"
            rec = f"{WORK}/crec_{block}_f{f:02d}.png"
            comp = f"{WORK}/cc_{block}_f{f:02d}.astc"
            astc("-tl", src, rec, block, extra=["-cw", "1", "1", "0", "0"])
            astc("-cl", src, comp, block, extra=["-cw", "1", "1", "0", "0"])
            R = np.array(Image.open(rec).convert("RGB"))
            for r in range(NT):
                for c in range(NT):
                    ou = us[f][r * 32 : r * 32 + 32, c * 32 : c * 32 + 32]
                    ov = vs[f][r * 32 : r * 32 + 32, c * 32 : c * 32 + 32]
                    du = R[r * cell : r * cell + 32, c * cell : c * cell + 32, 0]
                    dv = R[r * cell : r * cell + 32, c * cell : c * cell + 32, 1]
                    pu.append(psnr(ou, du)[0])
                    pv_.append(psnr(ov, dv)[0])
            if f == 0:
                payload = open(comp, "rb").read()[16:]
                ent = {
                    "raw": len(payload),
                    "zstd": zstd19(payload),
                    "order0": order0_bytes(payload),
                }
        au = np.array(pu)[np.isfinite(pu)]
        av = np.array(pv_)[np.isfinite(pv_)]
        summary["astc_chroma_" + block] = {
            "u_mean": float(au.mean()),
            "u_median": float(np.median(au)),
            "u_p5": float(np.percentile(au, 5)),
            "v_mean": float(av.mean()),
            "v_median": float(np.median(av)),
            "v_p5": float(np.percentile(av, 5)),
            "entropy_frame0": ent,
        }
        print("chroma", block, summary["astc_chroma_" + block]["u_mean"], flush=True)

    # ---- Option A: RGB ASTC of the BT.709 upconverted frame, luma PSNR after
    # converting the decoded RGB back to Y (same matrix), vs source Y.
    for block in ("4x4", "6x6", "8x8"):
        if block == "6x6":
            # 6x6 does not divide 64; skip cell-aligned variant for option A and
            # note it -- option A is a sanity comparison, measured at 4x4/8x8.
            continue
        vals = []
        ent = {}
        for f in range(NF):
            src = f"{D}/img/rgb_c64/f{f:02d}.png"
            rec = f"{WORK}/rgbrec_{block}_f{f:02d}.png"
            comp = f"{WORK}/rgbc_{block}_f{f:02d}.astc"
            astc("-tl", src, rec, block)
            astc("-cl", src, comp, block)
            R = np.array(Image.open(rec).convert("RGB")).astype(np.float32)
            yb = (0.2126 * R[:, :, 0] + 0.7152 * R[:, :, 1] + 0.0722 * R[:, :, 2])
            yb = np.clip(yb * (219.0 / 255.0) + 16.0 + 0.5, 0, 255).astype(np.uint8)
            for r in range(NT):
                for c in range(NT):
                    o = ys[f][r * 64 : r * 64 + 64, c * 64 : c * 64 + 64]
                    d = yb[r * 64 : r * 64 + 64, c * 64 : c * 64 + 64]
                    p_, _ = psnr(o, d)
                    vals.append(p_)
                    rows.append(["astcA_rgb_" + block, f, r * NT + c, f"{p_:.4f}", ""])
            if f == 0:
                payload = open(comp, "rb").read()[16:]
                ent = {
                    "raw": len(payload),
                    "zstd": zstd19(payload),
                    "order0": order0_bytes(payload),
                }
        a = np.array(vals)[np.isfinite(vals)]
        summary["astcA_rgb_" + block] = {
            "mean": float(a.mean()),
            "median": float(np.median(a)),
            "p5": float(np.percentile(a, 5)),
            "min": float(a.min()),
            "entropy_frame0": ent,
        }
        print("optA", block, float(a.mean()), flush=True)

    with open(f"{D}/task4_psnr_tiles.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["mode", "frame", "tile", "psnr_db", "mse"])
        w.writerows(rows)
    with open(f"{D}/task4_summary.json", "w") as fh:
        json.dump(summary, fh, indent=2)
    print("done")


if __name__ == "__main__":
    main()
