#!/usr/bin/env python3
"""BD-rate of one `tools/quality/compare.py` run against another.

`ref/phase2_verdict.py` answers "how far is the codec from the anchor". This
answers the other question a tool package has to answer: "what did this tool
change", which is a BD-rate between two runs of the *same* codec on the same
material, one with the tool and one without.

    ref/bd_between.py --base base.json --test tool.json [--metric psnr_y]
    ref/bd_between.py --base base.json --test tool.json --split

Both files must be `compare.py` output over the same sequence and the same
frame set. Negative output means the test run needs less rate for the same
quality, which is the sign convention `phase2_verdict.py` and every table in
`ref/RESULTS-*.md` use.

`--split` prints the fastest-20-percent and the remaining frames separately,
from `compare.py`'s own velocity split, with the caveat `phase2_verdict.py`
states with every run: one bitstream carries both subsets, so the rate axis is
the whole sequence's rate and not the subset's.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "quality"))

from nxq import bdrate  # noqa: E402


def _codec(doc, name=None):
    key = name or doc.get("codec_key")
    c = doc.get("codecs", {}).get(key)
    if c is None:
        for k, v in doc.get("codecs", {}).items():
            if v.get("kind") == "codec":
                return k, v
        return None, None
    return key, c


def _curve(points, rate_key, dist_key):
    r, d = [], []
    for p in points:
        rate, v = p.get(rate_key), p.get(dist_key)
        if rate is None or v is None or rate <= 0:
            continue
        r.append(float(rate))
        d.append(float(v))
    return r, d


def _bd(base, test, label):
    br, bdd = base
    tr, td = test
    if len(br) < 4 or len(tr) < 4:
        print(f"  {label:<16} not enough points ({len(br)} vs {len(tr)})")
        return None
    v = bdrate.bd_rate(br, bdd, tr, td)
    if v is None:
        print(f"  {label:<16} no overlapping quality range")
        return None
    print(f"  {label:<16} {v:+8.2f} %")
    return v


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--codec", default=None, help="codec key, if it differs")
    ap.add_argument("--metric", default="psnr_y",
                    help="psnr_y (default), ssim_y or ms_ssim_y")
    ap.add_argument("--split", action="store_true",
                    help="also split by angular velocity")
    a = ap.parse_args(argv)

    bdoc = json.load(open(a.base))
    tdoc = json.load(open(a.test))
    bkey, bc = _codec(bdoc, a.codec)
    tkey, tc = _codec(tdoc, a.codec)
    if bc is None or tc is None:
        print("no codec run in one of the files", file=sys.stderr)
        return 2
    bseq = bdoc.get("sequence", {}).get("name")
    tseq = tdoc.get("sequence", {}).get("name")
    if bseq != tseq:
        print(f"different sequences: {bseq} vs {tseq}", file=sys.stderr)
        return 2

    print(f"{bseq}: {os.path.basename(a.test)} against "
          f"{os.path.basename(a.base)}")
    print(f"  BD-rate on {a.metric}, negative is better:")
    _bd(_curve(bc["points"], "bitrate_mbps", a.metric),
        _curve(tc["points"], "bitrate_mbps", a.metric), "overall")

    if a.split:
        bs = bdoc.get("velocity_split", {}).get("codecs", {}).get(bkey)
        ts = tdoc.get("velocity_split", {}).get("codecs", {}).get(tkey)
        if not bs or not ts:
            print("  (no velocity split in these files)")
            return 0
        base_m = a.metric.replace("_y", "")
        for suffix, label in (("high_velocity", "fastest 20 %"),
                              ("low_velocity", "the rest")):
            key = f"{base_m}_y_{suffix}"
            _bd(_curve(bs, "bitrate_mbps", key),
                _curve(ts, "bitrate_mbps", key), label)
        print("  the split's rate axis is the WHOLE sequence's rate: one "
              "bitstream\n  carries both subsets (ref/phase2_verdict.py states "
              "this too).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
