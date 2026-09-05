#!/usr/bin/env python3
"""Unit-level checks for the spatial hybrid, needing neither ffmpeg nor the codec.

The three things a silent mistake would hide, and which every number in
hybrid/RESULTS-SPATIAL.md depends on:

* the crop is a valid pinhole camera of its own (a wrong FOV is a silently
  wrong warp, docs/WARP.md 2.1);
* the composite puts the inset back exactly where the crop took it from --
  a one-tile misalignment still produces plausible-looking PSNR;
* the simulator's inset translates to the headset's by *angle*, not by a
  pixel scale, and the decoder cost model agrees with bench/README.md.

Exit codes: 0 pass, 1 fail, 77 skip (numpy absent).
"""

from __future__ import annotations

import math
import os
import sys
import tempfile

try:
    import numpy as np
except ImportError:
    print("SKIP: numpy is not available")
    sys.exit(77)

sys.path.insert(
    0,
    os.environ.get(
        "NXVCH_SIM_DIR",
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "hybrid", "sim"),
    ),
)

from nxvchybrid import spatial as sp  # noqa: E402

FAILED: list[str] = []


def check(cond: bool, msg: str) -> None:
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        FAILED.append(msg)


def test_crop_camera() -> None:
    """A centred crop keeps the focal length, so its FOV is an arctangent."""
    eye, hfov = 1024, 95.0
    f = sp.focal_px(eye, hfov)
    check(abs(sp.crop_fov_deg(eye, hfov, eye) - hfov) < 1e-9,
          "a full-width crop is the original FOV")
    got = sp.crop_fov_deg(eye, hfov, 512)
    want = 2 * math.degrees(math.atan(256.0 / f))
    check(abs(got - want) < 1e-9, f"a half-width crop is 2*atan(w/2f) ({got:.3f} deg)")
    check(got > hfov / 2,
          "and is *wider* than half the FOV: atan is concave, so halving the "
          "pixels does not halve the angle")


def test_tile_alignment() -> None:
    """A centred inset covers whole tiles only at multiples of 2*TILE."""
    geo = sp.Geometry(2048, 1024, "yuv420p", 1, 90.0, "sbs")
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "s.yuv")
        with open(src, "wb") as fh:
            fh.write(bytes(geo.frame_bytes))
        for good in (128, 256, 384, 512):
            try:
                sp.extract_inset(src, geo, good, os.path.join(d, "o.yuv"))
                ok = True
            except sp.SpatialError:
                ok = False
            check(ok, f"inset {good} is legal for a 1024 px eye")
        for bad in (192, 320, 448):
            try:
                sp.extract_inset(src, geo, bad, os.path.join(d, "o.yuv"))
                ok = False
            except sp.SpatialError:
                ok = True
            check(ok, f"inset {bad} is refused, not silently misaligned")


def test_crop_composite_roundtrip() -> None:
    """crop then composite with a hard seam is the identity."""
    geo = sp.Geometry(512, 256, "yuv420p", 3, 90.0, "sbs")
    rng = np.random.default_rng(7)
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "s.yuv")
        flat = os.path.join(d, "flat.yuv")
        with open(src, "wb") as a, open(flat, "wb") as b:
            for _ in range(geo.frames):
                buf = rng.integers(0, 256, geo.frame_bytes, dtype=np.uint8)
                a.write(buf.tobytes())
                b.write(bytes([128]) * geo.frame_bytes)
        inset = 128
        crop = os.path.join(d, "c.yuv")
        sp.extract_inset(src, geo, inset, crop)
        out = os.path.join(d, "o.yuv")
        # Composite the crop of the *source* over a flat grey periphery: every
        # pixel inside the inset must come back bit-exact.
        sp.composite(flat, crop, geo, inset, 0, out)
        eye, off = geo.eye_width, (geo.eye_width - inset) // 2
        bad = 0
        for i in range(geo.frames):
            sy = geo.read(src, i)[0]
            oy = geo.read(out, i)[0]
            for e in (0, 1):
                a = sy[off:off + inset, e * eye + off: e * eye + off + inset]
                b = oy[off:off + inset, e * eye + off: e * eye + off + inset]
                bad += int((a != b).sum())
        check(bad == 0, "a hard-seam composite of the source crop is bit-exact inside")
        # ...and untouched outside.
        oy = geo.read(out, 0)[0]
        mask = np.ones_like(oy, dtype=bool)
        for e in (0, 1):
            mask[off:off + inset, e * eye + off: e * eye + off + inset] = False
        check(bool((oy[mask] == 128).all()), "and leaves the periphery untouched")


def test_feather() -> None:
    a = sp.feather_alpha(128, 32)
    check(a.shape == (128, 128), "the feather map is one weight per inset pixel")
    check(a[0, 0] < 1e-3 and a[-1, -1] < 1e-3, "it is ~zero at the corners")
    check(abs(a[64, 64] - 1.0) < 1e-6, "and one in the middle")
    check(bool((np.diff(a[:64, 64]) >= -1e-7).all()),
          "it is monotone from the border inward")
    hard = sp.feather_alpha(64, 0)
    check(bool((hard == 1.0).all()), "feather 0 is a hard seam")


def test_rate_scaling() -> None:
    """The 2 x 2048^2 x 90 Hz convention, and its inverse."""
    bps = sp.sim_bps(100.0, 2048, 1024)
    check(abs(bps - 25e6) < 1.0, f"a 2 x 1024^2 frame is charged a quarter ({bps / 1e6:.1f} Mbit)")
    bits = bps * 36 / 90.0
    back = sp.mbit_from_bits(bits, 36, 2048, 1024, 90.0)
    check(abs(back - 100.0) < 1e-6, "and the inverse round-trips")


def test_device_translation() -> None:
    """Simulator inset -> headset inset is angular, and the angles are the paper's."""
    check(abs(sp.PICO4_PPD_CENTER * 180 / math.pi * math.tan(math.radians(40.6))
              - 1080) < 3.0,
          "ppd_center 22.0 reproduces RATECONTROL 6.3's +/-40.6 deg over 2160 px")
    d = sp.device_inset_px(512, 1024, 95.0)
    check(d == 1408, f"the simulator's 512 px inset is 1408 px on a Pico 4 (got {d})")
    check(sp.device_inset_px(256, 1024, 95.0) < sp.device_inset_px(384, 1024, 95.0),
          "the translation is monotone")
    w, h = sp.eye_box_inset(3.0)
    check((w, h) == (1344, 1088),
          f"the 20x15 deg eye box plus a 8 deg foveal radius is 1344x1088 (got {w}x{h})")
    w2, _ = sp.eye_box_inset(5.85)
    check(w2 > w, "the 57 ms pad of PAPER 5.1.4 asks for a wider inset than the 40 ms one")


def test_decoder_model() -> None:
    """The cost model has to reproduce the bench measurement it is built from."""
    full = sp.decode_ms(sp.K5_TILES)
    check(abs(full - sp.K5_MS_FULL_FRAME) < 1e-9,
          f"the whole frame costs K5's own 28.0 ms ({full:.2f})")
    check(abs(sp.decode_ms(1024) - full / 2) < 1e-9, "and the model is linear in tiles")
    faster = sp.decode_ms(sp.K5_TILES, clock_mhz=sp.PART_CLOCK_MHZ)
    check(faster < full, "the part's 587 MHz is faster than the bench's 441.6")
    check(abs(sp.decode_ms(512, chroma=True) / sp.decode_ms(512) - 1.5) < 1e-9,
          "4:2:0 chroma adds the bench's 50 percent")
    fit = sp.inset_fitting(5.0)
    check(fit == 832, f"832 px per eye is the largest 5 ms inset, luma only (got {fit})")
    check(sp.decode_ms(sp.tiles_for(fit)) <= 5.0 < sp.decode_ms(sp.tiles_for(fit + 64)),
          "and it is the largest one: the next tile step overshoots")


def main() -> int:
    for fn in (test_crop_camera, test_tile_alignment, test_crop_composite_roundtrip,
               test_feather, test_rate_scaling, test_device_translation,
               test_decoder_model):
        print(fn.__name__)
        fn()
    if FAILED:
        print(f"\n{len(FAILED)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
