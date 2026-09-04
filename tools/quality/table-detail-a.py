#!/usr/bin/env python3
"""Render the ref/RESULTS-detail-a.md tables from the row result JSONs.

Reads $NXQ_SCRATCH/results/tourney-detail-a/<row>-<pix>.json for the four rows
of tools/quality/all-detail-a.sh and prints the gate table, the verbatim gate
lines and the operating-point table as Markdown.
"""
import json
import os
import sys

ROWS = [
    ("base", "`--split4x4 off --cfl off` (the shipped v1.4 default)"),
    ("split4", "`+ XFORM_4X4_SPLIT`"),
    ("final", "`+ INTRA_CFL` (**the v1.5 default**)"),
]
ALSO = [("cflonly", "`INTRA_CFL` alone, without the split")]
SCRATCH = os.environ.get(
    "NXQ_SCRATCH", "/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp")
OUT = os.path.join(SCRATCH, "results", "tourney-detail-a")


def load(row, pix):
    path = os.path.join(OUT, f"{row}-{pix}.json")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return json.load(f)


def gate_row(label, d):
    if d is None:
        return f"| {label} | (not measured) | | | |"
    bd = d["bd_rate"]["psnr_y"]["x264-intra"]
    p = d["phase1"]
    return ("| {} | {:+.2f} % | {:.3f} dB | {:.3f} dB at {:.1f} Mbit/s | {} |"
            .format(label, bd["bd_rate_pct"], p["mean_delta_db"],
                    p["worst_delta_db"], p["worst_at_mbps"],
                    "PASS" if p["pass"] else "FAIL"))


def points(d):
    key = d["codec_key"]
    out = []
    for pt in d["codecs"][key]["points"]:
        out.append((pt["qp"], pt["bitrate_mbps"], pt["psnr_y"],
                    pt.get("ssim_y"), pt.get("vmaf"), pt.get("wall_s")))
    return out


def main():
    for pix in ("yuv444p", "yuv420p"):
        print(f"\n**{'4:4:4' if pix == 'yuv444p' else '4:2:0'}**\n")
        print("| | BD-rate vs x264 intra | mean deficit | worst deficit | verdict |")
        print("|---|---|---|---|---|")
        for row, label in ROWS + ALSO:
            print(gate_row(label, load(row, pix)))

        d = load("final", pix)
        if d:
            print(f"\nverbatim, {pix}:\n```")
            bd = d["bd_rate"]["psnr_y"]["x264-intra"]
            p = d["phase1"]
            print("  BD-rate of final on PSNR-Y (negative is better):")
            print("    vs x264-intra   {:+8.2f} %   BD-PSNR {:.3f} dB   "
                  "(overlap {:.2f}-{:.2f} dB)".format(
                      bd["bd_rate_pct"], bd.get("bd_psnr_db", float("nan")),
                      bd.get("overlap_lo", float("nan")),
                      bd.get("overlap_hi", float("nan"))))
            print("\n  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 "
                  "intra, 100-400 Mbit):")
            print("    {}: worst {:.3f} dB at {:.1f} Mbit/s, mean {:.3f} dB "
                  "over {}".format(
                      "PASS" if p["pass"] else "FAIL", p["worst_delta_db"],
                      p["worst_at_mbps"], p["mean_delta_db"],
                      p["covered_mbps"]))
            print("```")

        print(f"\noperating points, {pix}:\n")
        base = load("base", pix)
        fin = load("final", pix)
        if base and fin:
            print("| QP | base Mbit/s | base PSNR-Y | final Mbit/s | final "
                  "PSNR-Y | final SSIM-Y | final VMAF | rate |")
            print("|---|---|---|---|---|---|---|---|")
            for (q, mb, py, _, _, _), (q2, mb2, py2, ss2, vm2, _) in zip(
                    points(base), points(fin)):
                assert q == q2
                print("| {} | {:.1f} | {:.2f} | {:.1f} | {:.2f} | {} | {} | "
                      "{:+.1f} % |".format(
                          q, mb, py, mb2, py2,
                          "%.4f" % ss2 if ss2 is not None else "-",
                          "%.1f" % vm2 if vm2 is not None else "-",
                          100.0 * (mb2 - mb) / mb))
    return 0


if __name__ == "__main__":
    sys.exit(main())
