#!/usr/bin/env python3
"""Numerically compare linear homogeneous-q interpolation with native f32 math.

This models a prospective noperspective vertex output; it is not a bit-exact
shader proof and intentionally leaves validity/qz tests as fragment checks.
"""
import json, math, random, struct
from pathlib import Path

def f(x):
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]

def dot(row, x, y, scale):
    return f(f(f(row[0] * x) + f(row[1] * y)) + f(row[2])) / scale

def q_at(m, x, y):
    # Matches shader h=(pixel-picture/2, 1), with f32 intermediates.
    return tuple(dot(m[i], f(x), f(y), (1 << 21) if i < 2 else (1 << 29)) for i in range(3))

def source(q, w, h):
    if abs(q[2]) < 1e-8:
        return None
    return (f(q[0] / q[2] + f(w * .5)), f(q[1] / q[2] + f(h * .5)))

def matrix(rng):
    # Mild affine warp with perspective terms; entries use the shader's Q21/Q29.
    a, b = rng.uniform(.7, 1.3), rng.uniform(-.12, .12)
    c, d = rng.uniform(-.12, .12), rng.uniform(.7, 1.3)
    tx, ty = rng.uniform(-180, 180), rng.uniform(-180, 180)
    px, py = rng.uniform(-2e-4, 2e-4), rng.uniform(-2e-4, 2e-4)
    return ([round(a*(1<<21)), round(b*(1<<21)), round(tx*(1<<21))],
            [round(c*(1<<21)), round(d*(1<<21)), round(ty*(1<<21))],
            [round(px*(1<<29)), round(py*(1<<29)), 1<<29])

def run(w, h, seed=20260908):
    rng = random.Random(seed + w * 17 + h)
    maxerr, count, skipped = 0.0, 0, 0
    # Include complete cells, clipped right/bottom cells, and both orientations.
    for x0 in range(0, w, 64):
        for y0 in range(0, h, 64):
            x1, y1 = min(x0 + 64, w), min(y0 + 64, h)
            m = matrix(rng)
            for rev in (False, True):
                verts = ((x0, y0), (x1, y0), (x1, y1)) if not rev else ((x0, y0), (x1, y1), (x0, y1))
                qv = [q_at(m, x - w*.5, y - h*.5) for x, y in verts]
                for _ in range(4):
                    u, v = rng.random(), rng.random()
                    if u + v > 1: u, v = 1-u, 1-v
                    z = 1-u-v
                    x = f(f(u*verts[1][0]) + f(v*verts[2][0]) + f(z*verts[0][0]))
                    y = f(f(u*verts[1][1]) + f(v*verts[2][1]) + f(z*verts[0][1]))
                    direct = source(q_at(m, x-w*.5, y-h*.5), w, h)
                    qi = tuple(f(f(u*qv[1][i]) + f(v*qv[2][i]) + f(z*qv[0][i])) for i in range(3))
                    interp = source(qi, w, h)
                    if direct is None or interp is None:
                        skipped += 1; continue
                    maxerr = max(maxerr, math.hypot(interp[0]-direct[0], interp[1]-direct[1]))
                    count += 1
    return {"width": w, "height": h, "samples": count, "qz_skipped": skipped, "max_source_error_px": maxerr}

def main():
    results = [run(2176, 2176), run(2173, 1089), run(2176, 1088)]
    out = {"method": "float32 linear interpolation of homogeneous q before divide",
           "results": results,
           "conclusion": "small numerical error in this model; retain fragment invalid flag and abs(q.z)<1e-8 check; not bit-exact proof"}
    print(json.dumps(out, indent=2))
    p = Path(__file__).with_name("homography-vertex-f32-summary.json")
    p.write_text(json.dumps(out, indent=2) + "\n")

if __name__ == "__main__":
    main()
