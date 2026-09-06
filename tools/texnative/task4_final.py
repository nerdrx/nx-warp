#!/usr/bin/env python3
"""Task 4(c)(d)(e)(f): rate accounting, payload entropy, matched-PSNR and budgets.

Adds to task4_quality.py's whole-frame entropy numbers a PER-TILE-INDEPENDENT
zstd measurement, which is the realistic wire model for atlas patches (a patch
is sent on its own, so cross-tile LZ matches are not available).
"""
import json, subprocess
import numpy as np

D = "/run/media/nerdrx/Lex/claude/nx-scratch/texnative-host"
PREFIX = ["chrt", "-i", "0", "taskset", "-c", "0-7", "nice", "-n", "19"]
NT = 17

# (luma cell, chroma cell) per block size; cell is the block-aligned tile pitch
CFG = {"4x4": (64, 32, 4), "6x6": (66, 36, 6), "8x8": (64, 32, 8)}


def zstd19(buf):
    p = subprocess.run(PREFIX + ["zstd", "-19", "-c", "-q"], input=buf, capture_output=True)
    assert p.returncode == 0
    return len(p.stdout)


def order0_bytes(buf):
    cnt = np.bincount(np.frombuffer(buf, dtype=np.uint8), minlength=256).astype(np.float64)
    n = cnt.sum()
    p = cnt[cnt > 0] / n
    return float(-(p * np.log2(p)).sum() * n / 8.0)


def tile_payloads(path, cell, bs):
    """Slice the .astc block grid into per-tile block byte strings."""
    data = open(path, "rb").read()[16:]
    bpr = cell // bs
    nbx = NT * bpr
    assert len(data) == nbx * nbx * 16, (len(data), nbx)
    a = np.frombuffer(data, dtype=np.uint8).reshape(nbx, nbx, 16)
    out = []
    for r in range(NT):
        for c in range(NT):
            out.append(a[r * bpr:(r + 1) * bpr, c * bpr:(c + 1) * bpr].tobytes())
    return out


def main():
    res = {}

    # ---------- (c) rate accounting, exact, per 64x64 luma tile ----------
    rate = {}
    for block, (lcell, ccell, bs) in CFG.items():
        lblocks = (lcell // bs) ** 2
        cblocks = (ccell // bs) ** 2
        lb, cb = lblocks * 16, cblocks * 16
        rate[block] = {
            "luma_blocks_per_tile": lblocks,
            "luma_bytes_per_tile": lb,
            "chroma_blocks_per_tile": cblocks,
            "chroma_bytes_per_tile": cb,
            "total_bytes_per_tile": lb + cb,
            "format_bpp": 128.0 / (bs * bs),
            "luma_bpp_per_luma_pixel": lb * 8 / 4096,
            "chroma_bpp_per_luma_pixel": cb * 8 / 4096,
            "total_bpp_per_luma_pixel": (lb + cb) * 8 / 4096,
        }
    res["rate"] = rate

    # ---------- (d) per-tile-independent entropy ----------
    ent = {}
    for block, (lcell, ccell, bs) in CFG.items():
        e = {}
        for leg, cell, path in (
            ("luma", lcell, f"{D}/work/c_{block}_f00.astc"),
            ("chroma", ccell, f"{D}/work/cc_{block}_f00.astc"),
        ):
            tp = tile_payloads(path, cell, bs)
            raw = sum(len(t) for t in tp)
            z_ind = sum(zstd19(t) for t in tp)
            z_all = zstd19(b"".join(tp))
            o0_ind = sum(order0_bytes(t) for t in tp)
            o0_all = order0_bytes(b"".join(tp))
            e[leg] = {
                "raw_bytes": raw,
                "zstd19_whole_frame": z_all,
                "zstd19_whole_frame_pct": 100 * (1 - z_all / raw),
                "zstd19_per_tile_sum": z_ind,
                "zstd19_per_tile_pct": 100 * (1 - z_ind / raw),
                "order0_whole_frame": o0_all,
                "order0_whole_frame_pct": 100 * (1 - o0_all / raw),
                "order0_per_tile_sum": o0_ind,
                "order0_per_tile_pct": 100 * (1 - o0_ind / raw),
            }
        ent[block] = e
        print("entropy", block, "luma zstd whole %.1f%% per-tile %.1f%% order0 %.1f%%" % (
            e["luma"]["zstd19_whole_frame_pct"], e["luma"]["zstd19_per_tile_pct"],
            e["luma"]["order0_whole_frame_pct"]), flush=True)
    res["entropy"] = ent

    # ---------- (e) matched PSNR against nxvc ----------
    nx = json.load(open(f"{D}/task4e_nxvc.json"))
    nx.update(json.load(open(f"{D}/task4e_nxvc_lowqp.json")))
    pts = sorted(
        ((int(k[2:]), v["luma_psnr_tile_mean"], v["bytes_per_tile_mean"]) for k, v in nx.items()),
        key=lambda t: t[1],
    )
    qp_a = np.array([p[0] for p in pts], float)
    db_a = np.array([p[1] for p in pts], float)
    by_a = np.array([p[2] for p in pts], float)

    q = json.load(open(f"{D}/task4_summary.json"))
    match = {}
    for block in ("4x4", "6x6", "8x8"):
        target = q["astc_luma_" + block]["mean"]
        inside = db_a.min() <= target <= db_a.max()
        qp = float(np.interp(target, db_a, qp_a))
        # bytes: interpolate log(bytes) vs dB, which is close to linear
        nb = float(np.exp(np.interp(target, db_a, np.log(by_a))))
        if not inside:
            # linear extrapolation on the top two points of log(bytes) vs dB
            s = (np.log(by_a[-1]) - np.log(by_a[-2])) / (db_a[-1] - db_a[-2])
            nb = float(np.exp(np.log(by_a[-1]) + s * (target - db_a[-1])))
            sq = (qp_a[-1] - qp_a[-2]) / (db_a[-1] - db_a[-2])
            qp = float(qp_a[-1] + sq * (target - db_a[-1]))
        ab = rate[block]["total_bytes_per_tile"]
        match[block] = {
            "astc_luma_psnr_mean": target,
            "astc_bytes_per_tile": ab,
            "in_measured_nxvc_range": bool(inside),
            "nxvc_qp_matched": qp,
            "nxvc_bytes_per_tile_matched": nb,
            "ratio_astc_over_nxvc": ab / nb,
        }
        print("match", block, match[block], flush=True)
    res["matched"] = match

    # ---------- (f) budgets ----------
    BR = 100e6
    budgets = {}
    for fps in (90, 47):
        bpf = BR / fps / 8.0  # bytes per frame, one stream (mono / one eye)
        e = {"bytes_per_frame": bpf}
        for block in ("4x4", "6x6", "8x8"):
            e["astc_" + block] = bpf / rate[block]["total_bytes_per_tile"]
            e["nxvc_at_" + block + "_quality"] = bpf / match[block]["nxvc_bytes_per_tile_matched"]
        for qp in (22, 26, 30):
            e[f"nxvc_qp{qp}"] = bpf / nx[f"qp{qp}"]["bytes_per_tile_mean"]
        budgets[f"{fps}fps"] = e
    res["budgets_100mbit"] = budgets

    json.dump(res, open(f"{D}/task4_final.json", "w"), indent=2)
    print(json.dumps(budgets, indent=2))


if __name__ == "__main__":
    main()
