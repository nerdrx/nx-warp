#!/usr/bin/env python3
"""Task 4(e): nxvc rate/quality sweep on the same clip, for matched-PSNR comparison.

Configuration choice: INTRA-ONLY (nxv-enc default, --inter off).  That is the
apples-to-apples setting against ASTC, which has no temporal prediction at all:
under a texture-native atlas every refreshed tile is an independently decodable
patch, so the nxvc cost of a refreshed tile is its INTRA cost.  Inter-coded
nxvc tiles would be far cheaper but are not the thing ASTC is replacing.

Per-tile bytes come from `nxv-info --tiles`; luma PSNR from decoding with
nxv-dec and comparing the Y plane tile by tile against the source.
"""
import csv, json, os, re, subprocess
import numpy as np

B = "/run/media/nerdrx/Lex/claude/nx-warp/build-vk/bin"
D = "/run/media/nerdrx/Lex/claude/nx-scratch/texnative-host"
SRC = "/run/media/nerdrx/Lex/claude/nx-scratch/inter_pan8.yuv"
PREFIX = ["chrt", "-i", "0", "taskset", "-c", "0-7", "nice", "-n", "19"]
W = H = 1088
CW = CH = 544
NF, NT, TILE = 12, 17, 64
FB = W * H + 2 * CW * CH
QPS = [2, 6, 10, 14, 18, 22, 26, 30, 34, 38]

TILELINE = re.compile(r"^\s*tile\s+(\d+)\s+e\d+\s+(\S+).*?(\d+)\s+B\s*$")


def src_y():
    raw = np.fromfile(SRC, dtype=np.uint8).reshape(NF, FB)
    return raw[:, : W * H].reshape(NF, H, W)


def dec_y(path):
    raw = np.fromfile(path, dtype=np.uint8)
    n = raw.size // FB
    return raw[: n * FB].reshape(n, FB)[:, : W * H].reshape(n, H, W)


def run(cmd):
    p = subprocess.run(PREFIX + cmd, capture_output=True, text=True)
    assert p.returncode == 0, " ".join(cmd) + "\n" + p.stderr[-2000:]
    return p.stdout


def main():
    os.makedirs(f"{D}/nxvc", exist_ok=True)
    ys = src_y()
    rows, summary = [], {}
    for qp in QPS:
        nxv = f"{D}/nxvc/q{qp}.nxv"
        yuv = f"{D}/nxvc/q{qp}.yuv"
        run([f"{B}/nxv-enc", "--in", SRC, "--w", "1088", "--h", "1088", "--pix",
             "yuv420p", "--qp", str(qp), "--threads", "8", "--quiet", "--out", nxv])
        run([f"{B}/nxv-dec", "--in", nxv, "--out", yuv, "--pix", "yuv420p", "--quiet"])
        # per-tile bytes
        info = run([f"{B}/nxv-info", "--in", nxv, "--tiles"])
        tb, modes = [], {}
        for line in info.splitlines():
            m = TILELINE.match(line)
            if m:
                tb.append(int(m.group(3)))
                modes[m.group(2)] = modes.get(m.group(2), 0) + 1
        tb = np.array(tb, dtype=np.float64)
        assert tb.size == NF * NT * NT, (tb.size, qp)
        # per-tile luma PSNR
        dy = dec_y(yuv)
        assert dy.shape[0] == NF
        ps = []
        for f in range(NF):
            for r in range(NT):
                for c in range(NT):
                    o = ys[f][r * TILE:(r + 1) * TILE, c * TILE:(c + 1) * TILE].astype(np.float64)
                    d = dy[f][r * TILE:(r + 1) * TILE, c * TILE:(c + 1) * TILE].astype(np.float64)
                    e = o - d
                    mse = float((e * e).mean())
                    v = float("inf") if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
                    ps.append(v)
                    rows.append([qp, f, r * NT + c, f"{v:.4f}", int(tb[f * NT * NT + r * NT + c])])
        a = np.array(ps)
        af = a[np.isfinite(a)]
        # whole-sequence luma PSNR from the pooled MSE, too
        e = ys.astype(np.float64) - dy.astype(np.float64)
        pooled = 10 * np.log10(255.0 ** 2 / float((e * e).mean()))
        summary[f"qp{qp}"] = {
            "stream_bytes": os.path.getsize(nxv),
            "bytes_per_tile_mean": float(tb.mean()),
            "bytes_per_tile_median": float(np.median(tb)),
            "bytes_per_tile_p95": float(np.percentile(tb, 95)),
            "bytes_per_frame_mean": float(tb.sum() / NF),
            "luma_psnr_tile_mean": float(af.mean()),
            "luma_psnr_tile_median": float(np.median(af)),
            "luma_psnr_tile_p5": float(np.percentile(af, 5)),
            "luma_psnr_tile_min": float(af.min()),
            "luma_psnr_pooled": float(pooled),
            "tile_modes": modes,
        }
        print(qp, summary[f"qp{qp}"]["bytes_per_tile_mean"],
              summary[f"qp{qp}"]["luma_psnr_tile_mean"], flush=True)
        os.remove(yuv)

    with open(f"{D}/task4e_nxvc_tiles.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["qp", "frame", "tile", "luma_psnr_db", "bytes"])
        w.writerows(rows)
    with open(f"{D}/task4e_nxvc.json", "w") as fh:
        json.dump(summary, fh, indent=2)


if __name__ == "__main__":
    main()
