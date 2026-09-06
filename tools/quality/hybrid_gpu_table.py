#!/usr/bin/env python3
"""The ADR-0030 tables, from results.json. Prints markdown."""
import json, sys
import numpy as np

R = json.load(open(sys.argv[1] if len(sys.argv) > 1 else
                   "/run/media/nerdrx/Lex/claude/nx-scratch/hybgpu/results.json"))
base, hyb = R["baseline"], R["hybrid"]
TRAJ = ["still", "rest", "objmotion-still", "objmotion", "mid", "fast"]
NF = 32.0
TILES = 578          # 34x17 over the eye pair

# The repo's own per-tile Adreno figures (docs/NXWARP-HYBRID.md 3.2 and ADR-0029), used
# only as a RATIO: they come from one bench, so the 1.57x timestamp inflation
# docs/ATLAS-DECODER.md warns about divides out of every comparison below.
US_CODED, US_SKIP, US_BASE = 41.0, 34.0, 1.9

def link(h):
    """What the hybrid puts on the air: the HEVC base plus the patch tiles' own bytes.

    The patch term is the summed tile PAYLOAD of the skip-mapped nxvc stream, not the
    file: a real hybrid sends the patch tiles and nothing else. It is an UNDER-estimate
    of a real patch -- these tiles predict from nxvc's own warped history, which is a
    better predictor than an upsampled low-res base -- so it is conservative in the
    hybrid's favour, which is the direction a rejection has to be conservative in."""
    return (h["base_bytes"] + h["patch_payload_bytes"]) / NF

def adreno_today(r):
    return r["adreno_ms"]

def adreno_hybrid(h):
    return h["adreno_ms"]

def bytes_at_psnr(t, target):
    c = sorted((r["psnr"], r["bytes_per_frame"]) for r in base if r["traj"] == t)
    ps = [x[0] for x in c]; bs = [x[1] for x in c]
    if target < ps[0] or target > ps[-1]:
        return None
    return float(np.exp(np.interp(target, ps, np.log(bs))))

def psnr_at_bytes(t, target):
    c = sorted((r["bytes_per_frame"], r["psnr"]) for r in base if r["traj"] == t)
    bs = [x[0] for x in c]; ps = [x[1] for x in c]
    if target < bs[0] or target > bs[-1]:
        return None
    return float(np.interp(target, bs, ps))

def nearest(t, p):
    return min((r for r in base if r["traj"] == t), key=lambda r: abs(r["psnr"] - p))

print("### 1. The nxvc-only curve (today)\n")
print("| trajectory | QP | B/frame | PSNR-Y | coded tiles/f | WARP_SKIP tiles/f | PICTURE frames | Adreno proxy ms/f |")
print("|---|---|---|---|---|---|---|---|")
for t in TRAJ:
    for r in sorted((r for r in base if r["traj"] == t), key=lambda r: r["qp"]):
        print(f"| {t} | {r['qp']} | {r['bytes_per_frame']:.0f} | {r['psnr']:.2f} | "
              f"{r['coded_per_frame']:.0f} | {r['skip_per_frame']:.0f} | "
              f"{r['picture_share']*100:.0f} % | {adreno_today(r):.2f} |")

print("\n### 2. Every hybrid operating point\n")
print("| traj | base/eye | Mbit | tau | base B/f | patch B/f | total B/f | base PSNR | hybrid PSNR "
      "| patch set /f | nxvc coded /f | nxvc-only B/f at equal PSNR | link cost | nxvc-only PSNR at equal bytes |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for h in hyb:
    t = h["traj"]
    tot = link(h)
    eq = bytes_at_psnr(t, h["psnr"])
    eb = psnr_at_bytes(t, tot)
    eq_s = f"{eq:.0f}" if eq else "above the curve"
    ra_s = f"{tot/eq:.2f}x" if eq else "n/a"
    eb_s = f"{eb:.2f}" if eb else "off the curve"
    print(f"| {t} | {h['eye_w']} | {h['mbit']} | {h['tau']} | {h['base_bytes']/NF:.0f} | "
          f"{h['patch_payload_bytes']/NF:.0f} | {tot:.0f} | {h['base_psnr']:.2f} | {h['psnr']:.2f} | "
          f"{h['patch_tiles_per_frame']:.0f} | {h['coded_per_frame']:.0f} | "
          f"{eq_s} | {ra_s} | {eb_s} |")

print("\n### 3. The best each scheme reaches, per trajectory\n")
print("nxvc-only is at QP 22, its best point on the sweep; the hybrid is the 1088-per-eye "
      "10 Mbit base at tau = 34 dB, the cheapest point that beats its own base layer.\n")
print("| trajectory | nxvc-only B/f | nxvc-only PSNR | hybrid B/f | hybrid PSNR | link | quality "
      "| PICTURE frames | Adreno today | Adreno hybrid | GPU |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for t in TRAJ:
    b = max((r for r in base if r["traj"] == t), key=lambda r: r["psnr"])
    h = next(h for h in hyb if h["traj"] == t and h["eye_w"] == 1088
             and h["mbit"] == 10 and h["tau"] == "34dB")
    tot = link(h)
    at, ah = adreno_today(b), adreno_hybrid(h)
    print(f"| {t} | {b['bytes_per_frame']:.0f} | {b['psnr']:.2f} | {tot:.0f} | {h['psnr']:.2f} | "
          f"**{tot/b['bytes_per_frame']:.2f}x** | **{h['psnr']-b['psnr']:+.2f} dB** | "
          f"{b['picture_share']*100:.0f} % | {at:.2f} ms | {ah:.2f} ms | "
          f"**{(1-ah/at)*100:+.0f} %** |")

print("\n### 4. The floor: the base layer alone, against the whole nxvc stream\n")
print("| trajectory | nxvc-only at its best (B/f, PSNR) | the 1088px 10 Mbit base alone (B/f, PSNR) | base / whole stream |")
print("|---|---|---|---|")
for t in TRAJ:
    b = max((r for r in base if r["traj"] == t), key=lambda r: r["psnr"])
    h = next(h for h in hyb if h["traj"] == t and h["eye_w"] == 1088
             and h["mbit"] == 10 and h["tau"] == "34dB")
    print(f"| {t} | {b['bytes_per_frame']:.0f} B, {b['psnr']:.2f} dB | "
          f"{h['base_bytes']/NF:.0f} B, {h['base_psnr']:.2f} dB | "
          f"**{(h['base_bytes']/NF)/b['bytes_per_frame']:.1f}x** |")
