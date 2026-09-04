#!/usr/bin/env python3
"""Graft a previously measured anchor curve onto a codec-only compare.py run.

An anchor curve is a property of the sequence, the anchor encoder and the QP
ladder.  It does not depend on which revision of *our* encoder produced the
codec curve, so measuring it again for every candidate tool costs an hour and
buys nothing.  This tool takes a run made with ``--anchors ''`` and the run
that already holds the anchor, checks that the two describe the same sequence
and the same anchor ladder, copies the anchor entry across and recomputes
every derived quantity (BD-rate, the Phase 1 gate, the velocity split) with
compare.py's own functions, so the result file is byte-for-byte the shape
compare.py would have written.

    splice_anchor.py --into run.json --anchor-from base.json --anchor x265-p

It refuses rather than guesses: a sequence name, frame count or anchor QP
ladder that disagrees is an error, because the whole value of the shortcut is
that the anchor is the same measurement.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare  # noqa: E402


def _ladder(entry: dict) -> list:
    return [p.get("qp", p.get("crf")) for p in entry.get("points", [])]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--into", required=True, help="codec-only run to complete")
    ap.add_argument("--anchor-from", required=True, help="run holding the anchor")
    ap.add_argument("--anchor", default="x265-p", help="anchor key to copy")
    ap.add_argument("--velocity-pct", type=float, default=20.0)
    ap.add_argument("--phase1-anchor", default="x264-intra")
    ap.add_argument("--phase1-band", default="100,400")
    ap.add_argument("--phase1-tolerance", type=float, default=1.0)
    args = ap.parse_args()

    with open(args.into) as fh:
        dst = json.load(fh)
    with open(args.anchor_from) as fh:
        src = json.load(fh)

    for field in ("name", "frames", "width", "height", "pix_fmt"):
        a, b = dst["sequence"].get(field), src["sequence"].get(field)
        if a != b:
            print(f"splice_anchor: sequence {field} differs: {a!r} vs {b!r}",
                  file=sys.stderr)
            return 2
    if args.anchor not in src["codecs"]:
        print(f"splice_anchor: {args.anchor!r} not in {args.anchor_from}",
              file=sys.stderr)
        return 2
    if args.anchor in dst["codecs"]:
        if _ladder(dst["codecs"][args.anchor]) != _ladder(src["codecs"][args.anchor]):
            print("splice_anchor: anchor ladders differ", file=sys.stderr)
            return 2

    dst["codecs"][args.anchor] = src["codecs"][args.anchor]
    dst["spliced_anchor"] = {"anchor": args.anchor,
                             "from": os.path.abspath(args.anchor_from)}

    ck = dst["codec_key"]
    metrics = ["psnr_y"]
    if any("ssim_y" in p for p in dst["codecs"][ck].get("points", [])):
        metrics.append("ssim_y")
    dst["bd_rate"] = {m: compare.bd_table(dst, ck, m) for m in metrics}
    band = tuple(float(x) for x in args.phase1_band.split(","))
    dst["phase1"] = compare.phase1_gate(dst, ck, args.phase1_anchor, band,
                                        args.phase1_tolerance)
    log = dst["sequence"].get("pose_log")
    if log and os.path.exists(log):
        poses = compare.read_pose_log(log)[: dst["sequence"]["frames"]]
        dst["velocity_split"] = compare.velocity_split(dst, poses,
                                                       args.velocity_pct)

    with open(args.into, "w") as fh:
        json.dump(dst, fh, indent=1)
        fh.write("\n")
    compare._print_summary(dst)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
