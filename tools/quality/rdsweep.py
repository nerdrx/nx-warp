#!/usr/bin/env python3
"""A fast rate-distortion sweep of nxv-enc against a fixed reference curve.

compare.py is the instrument the gate is decided on; this is the loop used
while tuning, because it does not re-run an anchor.  It encodes one sequence
at a QP ladder, measures bpp and PSNR-Y, and reports the BD-rate of one run
against another run's JSON -- so "did this change help" is one command and one
number rather than a whole comparison.
"""
import argparse, json, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from nxq import yuv, metrics, bdrate, cpu


def run(cmd):
    return subprocess.run(cpu.prefix() + cmd, check=True,
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seq", required=True)
    ap.add_argument("--frames", type=int, default=6)
    ap.add_argument("--qp", default="0,8,16,24")
    ap.add_argument("--enc", required=True)
    ap.add_argument("--dec", required=True)
    ap.add_argument("--work", default=os.environ.get("NXQ_SCRATCH", "/tmp") + "/rdsweep")
    ap.add_argument("--out", required=True)
    ap.add_argument("--vs", default=None, help="a previous --out to BD-rate against")
    a = ap.parse_args()

    meta = json.load(open(a.seq))
    d = os.path.dirname(os.path.abspath(a.seq))
    src = os.path.join(d, meta["path"])
    fmt = yuv.Format(meta["width"], meta["height"], meta["pix_fmt"])
    n = min(a.frames, meta["frames"])
    os.makedirs(a.work, exist_ok=True)
    ref = list(yuv.read_sequence(src, fmt, limit=n))

    pts = []
    for qp in [int(x) for x in a.qp.split(",")]:
        bs = os.path.join(a.work, "s%d.nxv" % qp)
        rec = os.path.join(a.work, "s%d.yuv" % qp)
        enc = a.enc.split() + ["--in", src, "--w", str(fmt.width), "--h", str(fmt.height),
                               "--pix", fmt.pix_fmt, "--qp", str(qp), "--frames", str(n),
                               "--out", bs]
        t0 = time.perf_counter()
        run(enc)
        t_enc = (time.perf_counter() - t0) / n
        t0 = time.perf_counter()
        run(a.dec.split() + ["--in", bs, "--out", rec, "--pix", fmt.pix_fmt])
        t_dec = (time.perf_counter() - t0) / n
        dis = list(yuv.read_sequence(rec, fmt, limit=n))
        pf = [metrics.psnr_frame(r, s) for r, s in zip(ref, dis)]
        psnr = float(np.mean([m["psnr_y"] for m in pf]))
        psnr_all = float(np.mean([m["psnr_ycbcr"] for m in pf]))
        bits = os.path.getsize(bs) * 8.0
        bpp = bits / (fmt.width * fmt.height * n)
        pts.append(dict(qp=qp, bpp=bpp, psnr_y=psnr, psnr_ycbcr=psnr_all,
                        enc_ms=t_enc * 1e3, dec_ms=t_dec * 1e3))
        print("  qp=%-3d %8.5f bpp  Y %7.3f dB  YCbCr %7.3f dB  "
              "enc %7.1f ms/f  dec %6.1f ms/f"
              % (qp, bpp, psnr, psnr_all, t_enc * 1e3, t_dec * 1e3), flush=True)
    res = dict(seq=a.seq, frames=n, enc=a.enc, points=pts)
    json.dump(res, open(a.out, "w"), indent=1)
    if a.vs:
        base = json.load(open(a.vs))
        for key in ("psnr_y", "psnr_ycbcr"):
            bd = bdrate.bd_rate([p["bpp"] for p in base["points"]],
                                [p[key] for p in base["points"]],
                                [p["bpp"] for p in pts], [p[key] for p in pts])
            print("BD-rate(%s) vs %s: %+.2f %%"
                  % (key, os.path.basename(a.vs), bd))
        et = np.mean([p["enc_ms"] for p in pts]) / np.mean([p["enc_ms"] for p in base["points"]])
        print("encode time: %.2fx" % et)


if __name__ == "__main__":
    main()
