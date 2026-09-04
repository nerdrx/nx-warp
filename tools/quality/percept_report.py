#!/usr/bin/env python3
"""Markdown tables from one or more `percept_run.py` result files.

Emits, per sequence: the operating-point table, the bits-at-equal-foveated-
quality table with the PSNR-Y cost next to every saving, the coded-tile
fraction per eccentricity ring, and the comparison against the foveated
standard-codec anchor at the two rates it is asked for.

    python3 percept_report.py $NXQ_SCRATCH/results/tourney-percept-*.json

SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import argparse
import json
import math


def fmt(v, nd=2, dash="--"):
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return dash
    return f"{v:.{nd}f}"


def table(rows: list[list[str]], head: list[str]) -> str:
    out = ["| " + " | ".join(head) + " |",
           "|" + "|".join("---" for _ in head) + "|"]
    out += ["| " + " | ".join(r) + " |" for r in rows]
    return "\n".join(out)


def points_table(doc: dict) -> str:
    rows = []
    for p in doc["points"]:
        rows.append([
            p["label"],
            fmt(p["mbits"]), fmt(p["bpp"], 4),
            fmt(p["psnr_y"]), fmt(p["fov_psnr_y"]), fmt(p.get("fov2_psnr_y")),
            fmt(p["psnr_fovea"]), fmt(p["psnr_periphery"]),
            fmt(p["fov_ssim_y"], 4), fmt(p.get("fvvdp"), 3),
        ])
    return table(rows, ["point", "Mbit/s", "bpp", "PSNR-Y", "fovPSNR",
                        "fov2PSNR", "PSNR fovea", "PSNR periph", "fovSSIM",
                        "JOD"])


def equal_quality_table(doc: dict, metric: str) -> str:
    rows = []
    flat_psnr = {p["mbits"]: p["psnr_y"] for p in doc["points"] if p["arm"] == "flat"}
    for r in doc["equal_quality"].get(metric, []):
        need = r["flat_mbits_at_same_quality"]
        rows.append([
            r["label"], fmt(r["mbits"]), fmt(r["quality"], 3),
            fmt(need), fmt(r["saving_pct"], 1),
            fmt(r["psnr_y"]),
        ])
    if not rows:
        return "_(the arm's quality fell outside the flat-QP curve; no honest "
    return table(rows, ["point", "Mbit/s", metric, "flat Mbit/s at the same "
                        + metric, "saving %", "PSNR-Y"])


def rings_table(doc: dict, key: str) -> str:
    r = doc["rings"].get(key)
    if not r:
        return "_(no ring data)_"
    rows = [[x["ring"], str(x["tiles"]), fmt(x["coded_fraction"], 3),
             fmt(x["forced_skip_fraction"], 3), fmt(x["mean_qp"], 1),
             fmt(x["mean_res_level"], 2)] for x in r["rings"]]
    return table(rows, ["ring (deg)", "tiles", "coded fraction",
                        "forced-skip fraction", "mean QP", "mean res_level"])


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("json", nargs="+")
    ap.add_argument("--rings", default=None,
                    help="operating point whose ring table to print "
                         "(default: rc-full at the middle rate)")
    args = ap.parse_args(argv)

    for path in args.json:
        with open(path) as fh:
            doc = json.load(fh)
        print(f"\n### {doc['sequence']}  "
              f"({doc['width']}x{doc['height']} {doc['pix_fmt']}, "
              f"{doc['frames']} frames, {doc['layout']}, "
              f"{doc['ppd_center']:.2f} ppd on axis, rate scale "
              f"{doc['rate_scale']:.4g})\n")
        print(points_table(doc))
        for metric in ("fov_psnr_y", "fov2_psnr_y", "fvvdp"):
            if metric in doc.get("equal_quality", {}):
                print(f"\n**Bits at equal `{metric}` against the flat-QP curve**\n")
                print(equal_quality_table(doc, metric))
        keys = args.rings.split(",") if args.rings else \
            [k for k in doc["rings"] if k.startswith("rc-full")]
        for k in keys:
            print(f"\n**Rings, `{k}`**\n")
            print(rings_table(doc, k))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
