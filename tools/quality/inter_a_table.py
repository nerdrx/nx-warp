#!/usr/bin/env python3
"""Collect the tourney/inter-a runs into the before/after table.

One row per (tag, sequence, band): the codec's operating points, the BD-rate
against x265-p over all frames and over the velocity split, and the encode and
decode time per frame.  It reads the same result files ref/phase2_verdict.py
reads and computes the same quantities with the same functions, so the two
cannot drift apart; what this adds is the *comparison across tags*, which is
what a before/after table is and what neither tool does on its own.

    inter_a_table.py --dir <results dir> --tags base t1 t2 t3 all
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from nxq import bdrate  # noqa: E402


def curve(points, metric="psnr_y"):
    r, d = [], []
    for p in points:
        v, rate = p.get(metric), p.get("bitrate_mbps")
        if v is None or not rate or rate <= 0:
            continue
        r.append(float(rate))
        d.append(float(v))
    return r, d


def bd(anchor_pts, codec_pts, metric="psnr_y"):
    ar, ad = curve(anchor_pts, metric)
    cr, cd = curve(codec_pts, metric)
    if len(ar) < 4 or len(cr) < 4:
        return None
    s = bdrate.bd_summary(ar, ad, cr, cd)
    return s.get("bd_rate_pct")


def split_bd(res):
    """BD-rate on the fastest frames and on the rest.

    compare.py's velocity split carries the mean PSNR of each subset per
    operating point against the WHOLE sequence's rate, which is the caveat
    ref/phase2_verdict.py prints with every run and which applies here word for
    word: this is "at a given overall bitrate, how much better is the codec on
    the fast frames", not a BD-rate over an independently rate-controlled
    subset.
    """
    cod = (res.get("velocity_split") or {}).get("codecs") or {}
    ck = res["codec_key"]
    out = {}
    if ck not in cod or "x265-p" not in cod:
        return out
    for subset, metric in (("high", "psnr_y_high_velocity"),
                           ("low", "psnr_y_low_velocity")):
        out[subset] = bd(cod["x265-p"], cod[ck], metric)
    return out


def load(path):
    with open(path) as fh:
        return json.load(fh)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--tags", nargs="+", required=True)
    args = ap.parse_args()

    print(f"{'tag':<8} {'seq':<12} {'bd':<2} {'bpp@mid':>8} {'PSNR-Y@mid':>11} "
          f"{'BD-rate':>9} {'fast':>9} {'rest':>9} {'enc+dec ms/f':>13}")
    for tag in args.tags:
        for path in sorted(glob.glob(os.path.join(args.dir, f"{tag}-*.json"))):
            base = os.path.basename(path)[len(tag) + 1: -5]
            key, band = base.rsplit("-", 1)
            try:
                res = load(path)
            except (OSError, ValueError) as exc:
                print(f"{tag:<8} {key:<12} {band:<2}  unreadable: {exc}")
                continue
            ck = res["codec_key"]
            cod = res["codecs"].get(ck, {}).get("points", [])
            anc = res["codecs"].get("x265-p", {}).get("points", [])
            if not cod:
                print(f"{tag:<8} {key:<12} {band:<2}  no codec points")
                continue
            mid = cod[len(cod) // 2]
            seq = res["sequence"]
            pix = seq["width"] * seq["height"] * seq["fps"]
            bpp = mid["bitrate_mbps"] * 1e6 / pix
            over = bd(anc, cod)
            sp = split_bd(res)
            # wall_s is the encode AND the decode of the whole sequence; the
            # split between them is measured separately, in RESULTS-inter-a.md.
            ms = 1000.0 * mid["wall_s"] / max(1, seq["frames"])
            def f(v, unit=""):
                return f"{v:+8.2f}{unit}" if isinstance(v, float) else "     n/a"
            print(f"{tag:<8} {key:<12} {band:<2} {bpp:8.4f} {mid['psnr_y']:11.2f} "
                  f"{f(over,'%')} {f(sp.get('high'),'%')} {f(sp.get('low'),'%')} "
                  f"{ms:13.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
