#!/usr/bin/env python3
"""The ref suite's atlas fixture, written to disk.

tests/ref/test_atlas.cpp builds its material in-process with make_scene() and
drives the pose with view_yaw(); ADR-0029's quality table was measured on that
material at 1088x1088 over 16 frames.  The GPU encoder is driven from files, so
to compare the two on ONE fixture the scene has to be reproducible outside the
ref binary.  This is that scene, transcribed:

    Scene s = make_scene(full_w, h, c444, f * 2.0, 30 + f * 4, panel)
    views   = view_yaw(f * yaw_per_frame)

Every expression below is the C one, in the same order and with the same
integer truncations -- `(int)pan`, the `(int)v` after clamping, and the `>> 4`
on tex()'s int result -- because a fixture that is nearly the ref's would make
the comparison meaningless in exactly the way it is meant to settle.
"""
import json, math, sys
import numpy as np


def tex(x, y):
    """The C tex(), vectorised.  x and y are integer arrays."""
    v = (128.0
         + 55.0 * np.sin(x * 0.031) * np.cos(y * 0.027)
         + 30.0 * np.sin((x * 3 + y * 5) * 0.11)
         + 18.0 * np.sin((x.astype(np.float64) ** 2 + y.astype(np.float64) ** 2)
                         * 0.00042))
    # `((x / 13 + y / 11) % 2) ? 12 : -12` -- C integer division, x,y >= 0 here
    # for luma; the chroma calls can pass y + 7 which is also non-negative.
    chk = ((x // 13 + y // 11) % 2)
    v = v + np.where(chk != 0, 12.0, -12.0)
    # `v < 0 ? 0 : (v > 255 ? 255 : (int)v)` -- truncation toward zero.
    out = np.where(v < 0, 0, np.where(v > 255, 255, np.trunc(v)))
    return out.astype(np.int64)


def make_scene(w, h, pan, obj, panel):
    cw, ch = (w + 1) // 2, (h + 1) // 2
    px = int(pan)                      # `const int px = (int)pan;`
    xs = np.arange(w)[None, :]
    ys = np.arange(h)[:, None]
    Y = tex(xs + px, np.broadcast_to(ys, (h, w)))
    Y = np.broadcast_to(Y, (h, w)).copy() if Y.shape != (h, w) else Y.copy()

    dx = xs - obj
    dy = ys - h // 2
    Y = np.where(dx * dx + dy * dy < 18 * 18, 230, Y)
    if panel:
        m = (xs < w // 3) & (ys < h // 3)
        chk = (((xs // 8) + (ys // 8)) % 2) != 0
        Y = np.where(m, np.where(chk, 235, 20), Y)

    f = 2                              # 4:2:0
    cxs = np.arange(cw)[None, :]
    cys = np.arange(ch)[:, None]
    U = 110 + (tex(cxs * f + px, np.broadcast_to(cys * f, (ch, cw))) >> 4)
    V = 140 - (tex(np.broadcast_to(cxs * f, (ch, cw)), cys * f + 7) >> 4)
    return (Y.astype(np.uint8), np.broadcast_to(U, (ch, cw)).astype(np.uint8),
            np.broadcast_to(V, (ch, cw)).astype(np.uint8))


def view_yaw(deg):
    a = deg * math.pi / 360.0
    return [0.0, math.sin(a), 0.0, math.cos(a)]


def main():
    w, h, frames, yaw, panel, out = (
        int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]),
        float(sys.argv[4]), int(sys.argv[5]), sys.argv[6])
    # make_scene's own content motion, in pixels per frame.  The ref suite uses
    # 2.0 (`f * 2.0`) and 30 + 4f for the disc.  At 192x192 that is a small part
    # of a 64-sample tile; at 1088x1088 it is enough to drive every tile to
    # INTRA under the integer mode decision, which leaves nothing skipped and
    # therefore no atlas to measure.  Passing 0 holds the world still so that
    # the only thing moving is the HEAD, which is the variable cross-tile
    # gather is a function of.
    pan_rate = float(sys.argv[7]) if len(sys.argv) > 7 else 2.0
    obj_rate = 4 if pan_rate else 0
    with open(out + ".yuv", "wb") as f:
        for n in range(frames):
            Y, U, V = make_scene(w, h, n * pan_rate, 30 + n * obj_rate, panel)
            f.write(Y.tobytes()); f.write(U.tobytes()); f.write(V.tobytes())
    with open(out + ".poses.json", "w") as f:
        json.dump({"version": 2,
                   "convention": {"id": "nxv-openxr-1"},
                   "fov_deg": {"h": 95.0, "v": 95.0},
                   "frames": [{"orientation_xyzw": view_yaw(n * yaw)}
                              for n in range(frames)]}, f, indent=1)
    print(f"{out}.yuv {out}.poses.json {w} {h} {frames} yaw={yaw}deg/frame "
          f"panel={panel}")


main()
