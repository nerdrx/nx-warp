#!/usr/bin/env python3
"""Compare Pico nxvc-vkdec readbacks with the retained CPU nxv-dec YUVs."""
from pathlib import Path
import json
import numpy as np

P = Path(__file__).parent
W, H, FRAMES = 4352, 2176, 8
PLANE = (W * H, (W // 2) * (H // 2), (W // 2) * (H // 2))
FRAME = sum(PLANE)

def region(a, b, x0, y0, x1, y1):
    # Y plane only; rectangle coordinates are pixels, end exclusive.
    out = []
    for f in range(FRAMES):
        off = f * FRAME
        aa = a[off + y0 * W + x0:off + y1 * W + x1]
        bb = b[off + y0 * W + x0:off + y1 * W + x1]
        # Rows are strided in the flattened frame.
        aa = a[off:off + PLANE[0]].reshape(H, W)[y0:y1, x0:x1]
        bb = b[off:off + PLANE[0]].reshape(H, W)[y0:y1, x0:x1]
        out.append(np.abs(aa.astype(np.int16) - bb.astype(np.int16)))
    d = np.concatenate([x.ravel() for x in out])
    return {"pixels": int(d.size), "mismatch": int(np.count_nonzero(d)),
            "mae": float(d.mean()), "max": int(d.max())}

def main():
    result = {"dimensions": [W, H], "frames": FRAMES,
              "centre_tiles_per_eye": [13, 13, 8, 8],
              "streams": {}}
    for name in ("control", "colour", "round-colour"):
        cpu = np.fromfile(P / f"{name}.yuv", dtype=np.uint8)
        gpu = np.fromfile(P / f"{name}-gpu.yuv", dtype=np.uint8)
        if cpu.size != gpu.size or cpu.size != FRAME * FRAMES:
            raise SystemExit(f"{name}: size cpu={cpu.size} gpu={gpu.size}")
        d = np.abs(cpu.astype(np.int16) - gpu.astype(np.int16))
        planes = []
        for pi, sz in enumerate(PLANE):
            q = np.concatenate([d[f * FRAME + sum(PLANE[:pi]):
                                  f * FRAME + sum(PLANE[:pi + 1])] for f in range(FRAMES)])
            planes.append({"mismatch": int(np.count_nonzero(q)),
                           "mae": float(q.mean()), "max": int(q.max())})
        # Quarter centre rectangle, plus its four 64x64 corner tiles.
        x0, y0, tw, th = 13 * 64, 13 * 64, 8 * 64, 8 * 64
        rect = region(cpu, gpu, x0, y0, x0 + tw, y0 + th)
        corners = {}
        for label, x, y in (("tl", x0, y0), ("tr", x0 + tw - 64, y0),
                            ("bl", x0, y0 + th - 64), ("br", x0 + tw - 64, y0 + th - 64)):
            corners[label] = region(cpu, gpu, x, y, x + 64, y + 64)
        result["streams"][name] = {"bytes": int((P / f"{name}.nxv").stat().st_size),
                                    "planes": planes, "centre_rect_y": rect,
                                    "centre_corner_tiles_y": corners}
        # --compact-centre emits each eye's 8x8 (512px) centre verbatim and
        # samples each outer 64px tile at four-pixel spacing (2176 -> 928).
        # Chroma uses the same mapping with 32px tiles (1088 -> 464).
        nv = np.fromfile(P / f"{name}-compact.nv12", dtype=np.uint8)
        cw, ch = 1856, 928
        cframe = cw * ch + cw * (ch // 2)
        if nv.size != cframe * FRAMES:
            raise SystemExit(f"{name}: compact size {nv.size}, expected {cframe * FRAMES}")
        cy, cu, cv = [], [], []
        for f in range(FRAMES):
            base = f * FRAME
            full_y = cpu[base:base + PLANE[0]].reshape(H, W)
            full_u = cpu[base + PLANE[0]:base + PLANE[0] + PLANE[1]].reshape(H // 2, W // 2)
            full_v = cpu[base + PLANE[0] + PLANE[1]:base + FRAME].reshape(H // 2, W // 2)
            def compact_indices(tile, centre0, ntile, inner):
                return np.concatenate([np.arange(t * tile + (0 if centre0 <= t < centre0 + 8 else 1),
                                                    t * tile + (tile if centre0 <= t < centre0 + 8 else tile),
                                                    1 if centre0 <= t < centre0 + 8 else 4)
                                       for t in range(ntile)])
            yi = compact_indices(64, 13, 34, 64)
            ci = compact_indices(32, 13, 34, 32)
            ycrop = np.concatenate((full_y[np.ix_(yi, yi)], full_y[np.ix_(yi, yi + 2176)]), axis=1)
            ucrop = np.concatenate((full_u[np.ix_(ci, ci)], full_u[np.ix_(ci, ci + 1088)]), axis=1)
            vcrop = np.concatenate((full_v[np.ix_(ci, ci)], full_v[np.ix_(ci, ci + 1088)]), axis=1)
            co = f * cframe
            gy = nv[co:co + cw * ch].reshape(ch, cw)
            guv = nv[co + cw * ch:co + cframe].reshape(ch // 2, cw)
            gu, gv = guv[:, 0::2], guv[:, 1::2]
            cy.append(np.abs(gy.astype(np.int16) - ycrop.astype(np.int16)))
            cu.append(np.abs(gu.astype(np.int16) - ucrop.astype(np.int16)))
            cv.append(np.abs(gv.astype(np.int16) - vcrop.astype(np.int16)))
        def stat(parts):
            q = np.concatenate([x.ravel() for x in parts])
            return {"pixels": int(q.size), "mismatch": int(np.count_nonzero(q)),
                    "mae": float(q.mean()), "max": int(q.max())}
        result["streams"][name]["compact_vs_cpu_crop"] = {
            "Y": stat(cy), "U": stat(cu), "V": stat(cv),
            "Y_corners": {k: stat([x[sy:sy+64, sx:sx+64] for x in cy])
                          for k, sx, sy in (("tl", 208, 208), ("tr", 208+512-64, 208),
                                            ("bl", 208, 208+512-64),
                                            ("br", 208+512-64, 208+512-64))}}
    print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
