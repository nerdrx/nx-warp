#!/usr/bin/env python3
"""Before/after tables for ref/RESULTS-rdo-a.md out of the compare.py JSONs."""
import json, os, sys, glob

D = os.environ.get("NXQ_SCRATCH", "") + "/results/tourney"


def load(tag, kind, pix):
    p = "%s/%s-%s-%s.json" % (D, kind, tag, pix)
    return json.load(open(p)) if os.path.exists(p) else None


def codec_points(d):
    for name, c in d["codecs"].items():
        if name.startswith("x26"):
            continue
        return name, c["points"]
    return None, []


def main():
    for kind, anchor in (("p1", "x264-intra"), ("kA", "x265-p"), ("kB", "x265-p")):
        for pix in ("yuv444p", "yuv420p"):
            b, a = load("base", kind, pix), load("rdoa", kind, pix)
            if not b and not a:
                continue
            print("\n### %s %s" % (kind, pix))
            for tag, d in [x for x in (("before", b), ("after", a)) if x[1]]:
                name, pts = codec_points(d)
                bd = d["bd_rate"]["psnr_y"][anchor]["bd_rate_pct"]
                print("  %-6s %-14s BD-rate %+8.2f %%" % (tag, name, bd))
                bdc = d["bd_rate"].get("psnr_ycbcr", {}).get(anchor, {}).get("bd_rate_pct")
                if bdc is not None:
                    print("         (PSNR-YCbCr BD-rate %+8.2f %%)" % bdc)
                enc = sum(x["wall_s"] for x in pts) / sum(x["frames"] for x in pts)
                print("         encode %.0f ms/frame" % (enc * 1e3))
                for p in pts:
                    print("     qp %-3d %8.1f Mbit/s  PSNR-Y %6.2f  PSNR-YCbCr %6.2f  SSIM %.4f"
                          % (p["qp"], p["bitrate_mbps"], p["psnr_y"],
                             p["psnr_ycbcr"], p.get("ssim_y", 0)))
                if d.get("phase1"):
                    ph = d["phase1"]
                    print("     phase1 %s worst %.3f dB mean %.3f dB over %.1f-%.1f"
                          % ("PASS" if ph["pass"] else "FAIL", ph["worst_delta_db"],
                             ph["mean_delta_db"], ph["covered_mbps"][0],
                             ph["covered_mbps"][1]))


main()
