#!/usr/bin/env python3
"""Fold the ATLAS/PICTURE frame split into the Adreno proxy, and emit the ADR tables.

Under the atlas (ADR-0029) a skipped tile in an ATLAS frame writes no reference pixels:
its cost is a matrix compose, which the bench measures at 0.0048 ms for the whole picture,
i.e. nothing. In a PICTURE frame every tile is reconstructed, so a skipped tile pays the
full 34 us integer warp. Charging all skips at 34 us -- which the first pass did -- flatters
the hybrid, because the hybrid's whole claim is that it replaces those warps.

Frame flags bit 5 is `picture_frame` (docs/SYNTAX.md 275).
"""
import json, os, subprocess
import numpy as np

OUT = "/run/media/nerdrx/Lex/claude/nx-scratch/hybgpu"
NICE = ["chrt", "-i", "0", "taskset", "-c", "12-15", "nice", "-n", "19"]
TRAJ = ["still", "rest", "objmotion-still", "objmotion", "mid", "fast"]
TILES, NF = 578, 32.0
US_CODED, US_SKIP, US_BASE = 41.0, 34.0, 1.9


def frames_of(path):
    """[(picture_frame, coded, skipped)] per frame."""
    r = subprocess.run(NICE + ["nxv-info", "--in", path, "--tiles"],
                       check=True, capture_output=True, text=True)
    out, pic, c, s, started = [], False, 0, 0, False
    for line in r.stdout.splitlines():
        if line.startswith("frame "):
            if started:
                out.append((pic, c, s))
            started, c, s = True, 0, 0
            fl = line.split("flags")[1].strip().split()[0]
            pic = bool(int(fl, 16) & 0x20)
        elif line.strip().startswith("tile "):
            if line.split()[3] == "WARP_SKIP":
                s += 1
            else:
                c += 1
    if started:
        out.append((pic, c, s))
    return out


def proxy_today(fr):
    """ms per frame, averaged: coded tiles always, skips only in PICTURE frames."""
    tot = sum(c * US_CODED + (s * US_SKIP if p else 0.0) for p, c, s in fr)
    return tot / len(fr) / 1000.0, sum(1 for p, _, _ in fr if p) / len(fr)


def proxy_hybrid(fr):
    """The patch stream is a pure ATLAS stream; every tile it does not code is refreshed
    from the base picture at 1.9 us, which also resets that tile's matrix to identity."""
    tot = sum(c * US_CODED + (TILES - c) * US_BASE for _, c, _ in fr)
    return tot / len(fr) / 1000.0


R = json.load(open(f"{OUT}/results.json"))
for r in R["baseline"]:
    fr = frames_of(f"{OUT}/{r['traj']}_d8_qp{r['qp']}.nxv")
    r["adreno_ms"], r["picture_share"] = proxy_today(fr)
    r["adreno_ms_allwarp"] = (r["coded_per_frame"] * US_CODED +
                              r["skip_per_frame"] * US_SKIP) / 1000.0
for h in R["hybrid"]:
    tag = f"{h['traj']}_h{h['eye_w']}_{h['mbit']}M_{h['tau']}"
    fr = frames_of(f"{OUT}/{tag}.nxv")
    h["adreno_ms"] = proxy_hybrid(fr)
json.dump(R, open(f"{OUT}/results.json", "w"), indent=1)
print("annotated")
