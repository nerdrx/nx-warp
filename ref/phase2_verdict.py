#!/usr/bin/env python3
"""Evaluate the Phase 2 exit criteria from a tools/quality/compare.py result.

`compare.py` runs the codec and the anchors, measures every operating point and
splits the frames at the angular-velocity percentile PAPER.md 2.11 item 1 asks
for.  What it does not do is turn that split into a BD-rate, because BD-rate on
a subset of frames is a Phase 2 question and `tools/quality` has an owner.  So
this script reads its JSON and finishes the job:

    ref/phase2_verdict.py --results run.json [run2.json ...]

It prints, per sequence:

* BD-rate of the codec against the anchor over all frames;
* BD-rate over the fastest `--velocity-pct` of frames, and over the rest;
* the verdict PAPER.md 2.11 item 1 states, **verbatim**:
  "within 10 percent at rest and at least 30 percent better on the motion
  frames".

**The honest caveat, printed with every run.** The rate axis of the split
figures is the whole sequence's rate, not the subset's: one bitstream carries
both subsets and neither this harness nor ffmpeg reports per-frame sizes for
every anchor.  So the split BD-rate answers "at a given overall bitrate, how
much better is the codec on the fast frames", which is the question the paper
is asking, but it is not a BD-rate over an independently rate-controlled
subset.  Anywhere that distinction could change a verdict it is said again.

`--chain` instead reads a warp-only chain measurement (2.11 item 2) written by
ref/warp_chain.py and prints the per-frame PSNR decay against the paper's
"above 35 dB for 30 frames on textured content".
"""

from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "quality"))

from nxq import bdrate  # noqa: E402


def _curve(points, rate_key, dist_key):
    r, d = [], []
    for p in points:
        v = p.get(dist_key)
        rate = p.get(rate_key)
        if v is None or rate is None or rate <= 0:
            continue
        r.append(float(rate))
        d.append(float(v))
    return r, d


def _bd(anchor, codec, label):
    ar, ad = anchor
    cr, cd = codec
    if len(ar) < 4 or len(cr) < 4:
        return {"label": label,
                "error": f"need 4 points per curve (anchor {len(ar)}, codec {len(cr)})"}
    out = bdrate.bd_summary(ar, ad, cr, cd)
    out["label"] = label
    return out


def _fmt(bd):
    if "error" in bd:
        return f"{bd['label']:<28} not computable: {bd['error']}"
    rate = bd.get("bd_rate_pct")
    psnr = bd.get("bd_psnr_db")
    parts = [f"{bd['label']:<28}"]
    parts.append(f"BD-rate {rate:+7.2f} %" if rate is not None else "BD-rate      n/a")
    parts.append(f"BD-PSNR {psnr:+6.3f} dB" if psnr is not None else "BD-PSNR    n/a")
    return "  ".join(parts)


def evaluate(doc: dict, anchor_name: str, codec_name: str | None,
             tol_rest: float, need_motion: float) -> dict:
    codecs = doc.get("codecs", {})
    if codec_name is None:
        for name, e in codecs.items():
            if e.get("kind") == "codec":
                codec_name = name
                break
    if codec_name is None or codec_name not in codecs:
        return {"error": "no codec entry in the result file"}
    if anchor_name not in codecs:
        avail = ", ".join(n for n, e in codecs.items() if e.get("kind") == "anchor")
        return {"error": f"anchor {anchor_name!r} not in the results (have: {avail})"}

    res: dict = {"sequence": doc.get("sequence", {}).get("name", "?"),
                 "codec": codec_name, "anchor": anchor_name}

    overall = _bd(_curve(codecs[anchor_name]["points"], "bitrate_mbps", "psnr_y"),
                  _curve(codecs[codec_name]["points"], "bitrate_mbps", "psnr_y"),
                  "overall (all frames)")
    res["overall"] = overall

    vs = doc.get("velocity_split", {})
    res["velocity_pct"] = vs.get("percentile")
    res["threshold_deg_s"] = vs.get("threshold_deg_s")
    res["high_frames"] = vs.get("high_frames")
    res["total_frames"] = vs.get("total_frames")
    sp = vs.get("codecs", {})
    if anchor_name in sp and codec_name in sp:
        res["fast"] = _bd(
            _curve(sp[anchor_name], "bitrate_mbps", "psnr_y_high_velocity"),
            _curve(sp[codec_name], "bitrate_mbps", "psnr_y_high_velocity"),
            f"fastest {vs.get('percentile', 20):.0f} % of frames")
        res["rest"] = _bd(
            _curve(sp[anchor_name], "bitrate_mbps", "psnr_y_low_velocity"),
            _curve(sp[codec_name], "bitrate_mbps", "psnr_y_low_velocity"),
            "the remaining frames")
    else:
        res["fast"] = {"label": "fastest frames", "error": "no velocity split in the results"}
        res["rest"] = {"label": "the rest", "error": "no velocity split in the results"}

    # The verdict, in the paper's own terms.
    rest_bd = res["rest"].get("bd_rate_pct")
    fast_bd = res["fast"].get("bd_rate_pct")
    v: dict = {"tolerance_at_rest_pct": tol_rest, "required_on_motion_pct": need_motion}
    if rest_bd is None or fast_bd is None:
        v["verdict"] = "NOT EVALUATED"
        v["why"] = "one of the two split curves is not computable"
    else:
        at_rest_ok = rest_bd <= tol_rest
        motion_ok = fast_bd <= -need_motion
        v["at_rest_pass"] = bool(at_rest_ok)
        v["on_motion_pass"] = bool(motion_ok)
        v["verdict"] = "PASS" if (at_rest_ok and motion_ok) else "FAIL"
    res["phase2_gate"] = v
    return res


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", nargs="+", required=True)
    ap.add_argument("--anchor", default="x265-p")
    ap.add_argument("--codec", default=None)
    ap.add_argument("--tolerance-at-rest", type=float, default=10.0,
                    help="percent BD-rate the codec may be WORSE at rest (paper: 10)")
    ap.add_argument("--required-on-motion", type=float, default=30.0,
                    help="percent BD-rate the codec must be BETTER by on the "
                         "motion frames (paper: 30)")
    ap.add_argument("--json", default=None, help="also write the verdicts here")
    args = ap.parse_args(argv)

    out = []
    for path in args.results:
        with open(path) as fh:
            doc = json.load(fh)
        r = evaluate(doc, args.anchor, args.codec, args.tolerance_at_rest,
                     args.required_on_motion)
        r["source"] = os.path.basename(path)
        out.append(r)

        print(f"\n=== {r.get('sequence', '?')}  ({r['source']})")
        if "error" in r:
            print(f"  {r['error']}")
            continue
        print(f"  codec {r['codec']} against {r['anchor']}, PSNR-Y")
        if r.get("threshold_deg_s") is not None:
            print(f"  velocity split at the {r['velocity_pct']:.0f}th percentile "
                  f"= {r['threshold_deg_s']:.1f} deg/s "
                  f"({r['high_frames']} of {r['total_frames']} frames)")
        for k in ("overall", "fast", "rest"):
            print("    " + _fmt(r[k]))
        g = r["phase2_gate"]
        print("  Phase 2 kill test (PAPER.md 2.11 item 1):")
        print("    \"within 10 percent at rest and at least 30 percent better "
              "on the motion frames\"")
        if g["verdict"] == "NOT EVALUATED":
            print(f"    NOT EVALUATED: {g['why']}")
        else:
            print(f"    at rest   : BD-rate {r['rest']['bd_rate_pct']:+.2f} % "
                  f"(allowed up to {g['tolerance_at_rest_pct']:+.0f} %)  "
                  f"{'PASS' if g['at_rest_pass'] else 'FAIL'}")
            print(f"    on motion : BD-rate {r['fast']['bd_rate_pct']:+.2f} % "
                  f"(needs {-g['required_on_motion_pct']:+.0f} % or better)  "
                  f"{'PASS' if g['on_motion_pass'] else 'FAIL'}")
            print(f"    VERDICT   : {g['verdict']}")
        print("  note: the split BD-rate uses the whole sequence's rate on the "
              "rate axis;\n        one bitstream carries both subsets.")

    if args.json:
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=1)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
