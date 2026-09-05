#!/usr/bin/env python3
"""End to end over the real-capture path, without a headset.

`ingest_wivrn.py` sits between a recorded WiVRn session and every gate number
this project will state on real material.  Nothing downstream of it can detect
its mistakes: a deinterleave that swapped Cb and Cr, a 10-bit shift off by two,
a frame joined to the wrong eye's pose -- none of those crash, none produce an
illegal bitstream, and all of them present as a codec that is merely bad.  That
is the failure mode `docs/WARP-AUDIT.md` was written about, and this file is the
measurement that closes it for the capture path.

Four things are asserted, in order of how badly each has already gone wrong
somewhere in this project's history:

1. **The pixels round trip bit for bit.**  A synthetic v2 sequence is split
   into a `raw_dump.cpp`-format dump and ingested back; the bytes must be
   identical.  8-bit NV12 and 10-bit P010 both, because the P010 path has a
   shift in it and a shift is a thing you get wrong once.
2. **The poses round trip.**  Quaternions, FOV and the angular velocity the
   BD-rate split is computed from.
3. **The convention is right end to end**, through `nxv-enc`: a pure-rotation
   pair of a band-limited analytic world, forced to `WARP_SKIP`, must clear
   **35 dB** on the warped frame -- the same shape of measurement as
   `tests/ref/test_warp_convention.cpp`.  And the negative control: conjugate
   every quaternion in the ingested pose log, which is the single most likely
   convention error, and the same measurement must collapse.  Without the
   control the threshold proves nothing, because a test that passes for the
   wrong reason is worse than no test.
4. **A foveated capture is refused**, and `--allow-foveation` actually converts
   it and marks it as not gate material.  A switch that is read and ignored is
   the same as no switch.

Run it either way::

    python3 -m pytest tools/quality/capture/test_ingest_wivrn.py -q
    python3 tools/quality/capture/test_ingest_wivrn.py
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys

import numpy as np
import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
QUALITY = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(QUALITY))
sys.path.insert(0, QUALITY)

from capture import fake_raw_dump, gen_synthetic, ingest_wivrn  # noqa: E402
from nxq import cpu, yuv  # noqa: E402

EYE = 64
FRAMES = 3

#: The warped-frame floor.  `tests/ref/test_warp_convention.cpp` measures 50 to
#: 58 dB with the convention right on this class of material; 35 dB is the bar
#: PAPER 2.11 item 2 states and is far above anything a wrong convention
#: reaches, so it separates the two without being a tuned number.
WARP_FLOOR_DB = 35.0


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    d = a.astype(np.float64) - b.astype(np.float64)
    mse = float(np.mean(d * d))
    return 1000.0 if mse == 0 else 10.0 * np.log10(255.0 * 255.0 / mse)


def centre_crop(p: np.ndarray, border: int = 8) -> np.ndarray:
    """Drop a 1/8 border, which is the crop `warp/RESULTS.md` reports on.

    Full-frame PSNR on a rotation is dominated by the disocclusion strip at the
    leading edge -- content that was not in the previous frame at all, which no
    predictor can invent and which the encoder answers with INTRA.  The centre
    is the part that describes the predictor.
    """
    h, w = p.shape
    return p[h // border: h - h // border, w // border: w - w // border]


def frames_of(path: str, w: int, h: int, pix: str = "yuv420p") -> int:
    fs = w * h * 3 // 2 if pix == "yuv420p" else w * h * 3
    return os.path.getsize(path) // fs


def luma(path: str, w: int, h: int, i: int, pix: str = "yuv420p") -> np.ndarray:
    fs = w * h * 3 // 2 if pix == "yuv420p" else w * h * 3
    with open(path, "rb") as fh:
        fh.seek(i * fs)
        return np.frombuffer(fh.read(w * h), np.uint8).reshape(h, w)


# ------------------------------------------------------------------ fixtures


@pytest.fixture(scope="module")
def synth_seq(tmp_path_factory):
    """A tiny band-limited stereo sequence: the known-good side of the round trip."""
    d = str(tmp_path_factory.mktemp("seq"))
    assert gen_synthetic.main([
        "--out", d, "--name", "rt", "--frames", str(FRAMES),
        "--eye-width", str(EYE), "--eye-height", str(EYE),
        "--motion", "mixed", "--layout", "sbs", "--pix", "yuv420p",
        "--seed", "1", "--quiet"]) == 0
    return {"dir": d,
            "sidecar": os.path.join(d, "rt.yuv420p.json"),
            "yuv": os.path.join(d, "rt.yuv420p.yuv"),
            "poses": os.path.join(d, "rt.poses.json")}


def _dump_and_ingest(synth_seq, tmp_path, depth: int, name: str = "cap", **kw):
    dump = str(tmp_path / f"dump{depth}")
    out = str(tmp_path / f"out{depth}")
    assert fake_raw_dump.main([
        "from-sequence", "--seq", synth_seq["sidecar"], "--out", dump,
        "--depth", str(depth)] + kw.pop("fake_args", [])) == 0
    res = ingest_wivrn.ingest(dump, name, out, ["yuv420p"], "sbs", "left",
                              None, 0, "ordered", kw.pop("allow_foveation", False),
                              1.0, quiet=True)
    return dump, out, res


# ---------------------------------------------------------------- the pixels


class TestPixelRoundTrip:
    def test_nv12_is_lossless(self, synth_seq, tmp_path):
        """NV12 is an interleave of the same 4:2:0 samples, so nothing may move."""
        _, out, res = _dump_and_ingest(synth_seq, tmp_path, 8)
        got = os.path.join(out, "cap.yuv420p.yuv")
        assert open(got, "rb").read() == open(synth_seq["yuv"], "rb").read()
        assert res["frames"] == FRAMES
        assert res["width"] == 2 * EYE and res["height"] == EYE

    def test_p010_round_trips_exactly(self, synth_seq, tmp_path):
        """8 -> 10 -> 8 is exact: the dither threshold is below the shifted bit."""
        _, out, _ = _dump_and_ingest(synth_seq, tmp_path, 10)
        got = os.path.join(out, "cap.yuv420p.yuv")
        assert open(got, "rb").read() == open(synth_seq["yuv"], "rb").read()

    def test_chroma_planes_are_not_swapped(self, synth_seq, tmp_path):
        """Cb and Cr are adjacent bytes in NV12 and a swap is invisible in luma."""
        _, out, _ = _dump_and_ingest(synth_seq, tmp_path, 8)
        fmt = yuv.Format(2 * EYE, EYE, "yuv420p")
        a = yuv.read_frame(synth_seq["yuv"], fmt, 1)
        b = yuv.read_frame(os.path.join(out, "cap.yuv420p.yuv"), fmt, 1)
        assert np.array_equal(a.u, b.u) and np.array_equal(a.v, b.v)
        # ... and the two planes actually differ, or the check above is vacuous.
        assert not np.array_equal(a.u, a.v)

    def test_eyes_are_side_by_side_in_order(self, synth_seq, tmp_path):
        """Eye 0 is the left half. Swapping them is a silent stereo inversion."""
        _, out, _ = _dump_and_ingest(synth_seq, tmp_path, 8)
        src = luma(synth_seq["yuv"], 2 * EYE, EYE, 0)
        got = luma(os.path.join(out, "cap.yuv420p.yuv"), 2 * EYE, EYE, 0)
        assert np.array_equal(src[:, :EYE], got[:, :EYE])
        assert not np.array_equal(src[:, :EYE], src[:, EYE:])  # the halves differ

    def test_dither_none_also_round_trips(self, synth_seq, tmp_path):
        dump = str(tmp_path / "dn")
        assert fake_raw_dump.main(["from-sequence", "--seq", synth_seq["sidecar"],
                                   "--out", dump, "--depth", "10"]) == 0
        res = ingest_wivrn.ingest(dump, "cap", str(tmp_path / "outn"), ["yuv420p"],
                                  "sbs", "left", None, 0, "none", False, 1.0, True)
        assert open(res["files"][0], "rb").read() == open(synth_seq["yuv"], "rb").read()
        assert res["poses"]["capture"]["dither"] == "none"


# ----------------------------------------------------------------- the poses


class TestPoseRoundTrip:
    def test_orientations_survive(self, synth_seq, tmp_path):
        _, _, res = _dump_and_ingest(synth_seq, tmp_path, 8)
        src = json.load(open(synth_seq["poses"]))
        for a, b in zip(src["frames"], res["poses"]["frames"]):
            assert a["orientation_xyzw"] == pytest.approx(b["orientation_xyzw"], abs=1e-9)

    def test_fov_survives_as_degrees(self, synth_seq, tmp_path):
        """The .jsonl carries tangents; the sidecar owes nxv-enc degrees."""
        _, _, res = _dump_and_ingest(synth_seq, tmp_path, 8)
        src = json.load(open(synth_seq["poses"]))
        assert res["poses"]["fov_deg"]["h"] == pytest.approx(src["fov_deg"]["h"], abs=1e-6)
        assert res["poses"]["fov_deg"]["v"] == pytest.approx(src["fov_deg"]["v"], abs=1e-6)

    def test_convention_block_is_the_one_nxv_enc_implements(self, synth_seq, tmp_path):
        _, _, res = _dump_and_ingest(synth_seq, tmp_path, 8)
        assert res["poses"]["version"] == 2
        assert res["poses"]["convention"]["id"] == "nxv-openxr-1"

    def test_the_scrapers_see_the_right_values(self, synth_seq, tmp_path):
        """`nxv-enc` reads this file with a substring scraper, not a JSON parser.

        It takes the FIRST `"id"`, the FIRST `"fov_deg"` and EVERY
        `"orientation_xyzw"`.  The capture block is bulky and sits in the middle
        of the document, so this pins that it cannot shadow any of the three.
        """
        _, _, res = _dump_and_ingest(synth_seq, tmp_path, 8)
        txt = json.dumps(res["poses"], indent=1)
        assert txt.count('"orientation_xyzw"') == FRAMES
        assert txt.index('"id"') < txt.index('"capture"')
        assert txt.index('"fov_deg"') < txt.index('"capture"')

    def test_angular_velocity_is_computed(self, synth_seq, tmp_path):
        """compare.py splits BD-rate on this field; a zero column kills the split."""
        _, _, res = _dump_and_ingest(synth_seq, tmp_path, 8)
        av = [f["angular_velocity_deg_s"] for f in res["poses"]["frames"][1:]]
        assert all(v > 0.0 for v in av)

    def test_foveation_runs_are_preserved(self, synth_seq, tmp_path):
        _, _, res = _dump_and_ingest(synth_seq, tmp_path, 8)
        fov = res["poses"]["capture"]["foveation"]
        assert fov["identity"] is True
        assert fov["variants"] and fov["variants"][0][0]["x"] == [EYE]


# --------------------------------------------------------------- the switches


class TestFoveationGate:
    def test_a_foveated_capture_is_refused(self, synth_seq, tmp_path):
        dump = str(tmp_path / "fov")
        assert fake_raw_dump.main(["from-sequence", "--seq", synth_seq["sidecar"],
                                   "--out", dump, "--foveation", "on"]) == 0
        with pytest.raises(ingest_wivrn.IngestError, match="FOVEATED"):
            ingest_wivrn.ingest(dump, "cap", str(tmp_path / "o1"), ["yuv420p"],
                                "sbs", "left", None, 0, "ordered", False, 1.0, True)

    def test_allow_foveation_converts_and_marks_it(self, synth_seq, tmp_path):
        dump = str(tmp_path / "fov2")
        assert fake_raw_dump.main(["from-sequence", "--seq", synth_seq["sidecar"],
                                   "--out", dump, "--foveation", "on"]) == 0
        res = ingest_wivrn.ingest(dump, "cap", str(tmp_path / "o2"), ["yuv420p"],
                                  "sbs", "left", None, 0, "ordered", True, 1.0, True)
        assert res["gate_material"] is False
        assert res["poses"]["capture"]["foveation"]["identity"] is False
        assert os.path.getsize(res["files"][0]) > 0


class TestInfoValidation:
    def test_a_lying_info_file_is_refused(self, synth_seq, tmp_path):
        dump = str(tmp_path / "bad")
        assert fake_raw_dump.main(["from-sequence", "--seq", synth_seq["sidecar"],
                                   "--out", dump]) == 0
        p = os.path.join(dump, "stream0-info.json")
        info = json.load(open(p))
        info["frame_bytes"] += 1
        json.dump(info, open(p, "w"))
        with pytest.raises(ingest_wivrn.IngestError, match="frame_bytes"):
            ingest_wivrn.load_info(dump, 0)

    def test_mismatched_eye_geometry_is_refused(self, synth_seq, tmp_path):
        dump = str(tmp_path / "bad2")
        assert fake_raw_dump.main(["from-sequence", "--seq", synth_seq["sidecar"],
                                   "--out", dump]) == 0
        p = os.path.join(dump, "stream1-info.json")
        info = json.load(open(p))
        info["width"] //= 2
        info["frame_bytes"] = info["width"] * info["height"] * 3 // 2
        info["planes"][0]["width"] = info["width"]
        info["planes"][1]["width"] = info["width"] // 2
        json.dump(info, open(p, "w"))
        with pytest.raises(ingest_wivrn.IngestError, match="disagree"):
            ingest_wivrn.ingest(dump, "cap", str(tmp_path / "o3"), ["yuv420p"],
                                "sbs", "left", None, 0, "ordered", False, 1.0, True)

    def test_a_dotted_name_is_refused(self, synth_seq, tmp_path):
        dump = str(tmp_path / "ok")
        assert fake_raw_dump.main(["from-sequence", "--seq", synth_seq["sidecar"],
                                   "--out", dump]) == 0
        with pytest.raises(ingest_wivrn.IngestError, match="dot"):
            ingest_wivrn.ingest(dump, "a.b", str(tmp_path / "o4"), ["yuv420p"],
                                "sbs", "left", None, 0, "ordered", False, 1.0, True)


# ------------------------------------------------ the convention, through nxv-enc


def _bin(name: str) -> str | None:
    for d in (os.path.join(REPO, "build-ref", "bin"), os.path.join(REPO, "build", "bin")):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return shutil.which(name)


@pytest.fixture(scope="module")
def rotation_dump(tmp_path_factory):
    """A pure 2-degree yaw pair of the band-limited analytic world, ingested."""
    d = str(tmp_path_factory.mktemp("rot"))
    dump, out = os.path.join(d, "dump"), os.path.join(d, "out")
    assert fake_raw_dump.main(["rotation", "--out", dump, "--eye", "256",
                               "--frames", "2", "--yaw-step", "2.0"]) == 0
    res = ingest_wivrn.ingest(dump, "rot", out, ["yuv420p"], "sbs", "left",
                              None, 0, "ordered", False, 1.0, quiet=True)
    return res


def warp_psnr(res: dict, work: str, poses: str | None = None) -> float:
    """Encode frame 1 as nothing but the pose warp of frame 0, and score it.

    `--intra-period` beyond the clip stops the rolling refresh and
    `--skip-thresh` puts the `WARP_SKIP` gate above anything real content
    reaches, so the decoded second frame is the predictor applied to a bit-exact
    copy of the first and nothing else -- exactly what `ref/warp_chain.py` does,
    and what `tests/ref/test_warp_convention.cpp` does in C++.
    """
    enc, dec = _bin("nxv-enc"), _bin("nxv-dec")
    if not enc or not dec:
        pytest.skip("nxv-enc/nxv-dec not built (cmake --build build-ref)")
    os.makedirs(work, exist_ok=True)
    raw = res["files"][0]
    W, H = res["width"], res["height"]
    bs, rec = os.path.join(work, "w.nxv"), os.path.join(work, "w.yuv")
    p = cpu.run([enc, "--in", raw, "--w", str(W), "--h", str(H), "--pix", "yuv420p",
                 "--lossless", "--eyes", "2", "--inter", "on",
                 "--poses", poses or res["pose_log"],
                 "--intra-period", "1000000", "--skip-thresh", "100000",
                 "--out", bs, "--quiet"], check=False)
    assert p.returncode == 0, p.stderr or p.stdout
    p = cpu.run([dec, "--in", bs, "--out", rec, "--quiet"], check=False)
    assert p.returncode == 0, p.stderr or p.stdout
    assert frames_of(rec, W, H) >= 2
    truth = luma(raw, W, H, 1)[:, :W // 2]
    got = luma(rec, W, H, 1)[:, :W // 2]
    return psnr(centre_crop(truth), centre_crop(got))


class TestWarpConventionEndToEnd:
    def test_frame_0_is_lossless(self, rotation_dump, tmp_path):
        """If frame 0 were not exact, frame 1's score would not be the warp's."""
        enc, dec = _bin("nxv-enc"), _bin("nxv-dec")
        if not enc or not dec:
            pytest.skip("nxv-enc/nxv-dec not built")
        work = str(tmp_path / "w0")
        warp_psnr(rotation_dump, work)
        W, H = rotation_dump["width"], rotation_dump["height"]
        assert np.array_equal(luma(rotation_dump["files"][0], W, H, 0),
                              luma(os.path.join(work, "w.yuv"), W, H, 0))

    def test_warped_frame_clears_the_floor(self, rotation_dump, tmp_path):
        """THE test.  A pose conversion error lands here and nowhere else."""
        db = warp_psnr(rotation_dump, str(tmp_path / "w1"))
        assert db > WARP_FLOOR_DB, (
            f"first warped frame {db:.2f} dB, needs > {WARP_FLOOR_DB}. The pose "
            "conversion in ingest_wivrn.py, the FOV, or the quaternion "
            "convention is wrong -- see docs/WARP.md 2.1.")

    def test_a_conjugated_quaternion_collapses_it(self, rotation_dump, tmp_path):
        """The negative control: the most likely convention error, priced.

        Conjugating the quaternion is world-to-camera instead of camera-to-world
        -- the rotation applied backwards.  It is legal, it decodes, it produces
        no warning anywhere, and it must be worth tens of dB here or this file's
        threshold is measuring nothing.
        """
        good = warp_psnr(rotation_dump, str(tmp_path / "wg"))
        doc = json.load(open(rotation_dump["pose_log"]))
        for f in doc["frames"]:
            x, y, z, w = f["orientation_xyzw"]
            f["orientation_xyzw"] = [-x, -y, -z, w]
        bad_poses = str(tmp_path / "conj.poses.json")
        json.dump(doc, open(bad_poses, "w"), indent=1)
        bad = warp_psnr(rotation_dump, str(tmp_path / "wb"), poses=bad_poses)
        assert bad < WARP_FLOOR_DB
        assert good - bad > 10.0, (
            f"conjugating every quaternion cost only {good - bad:.2f} dB "
            f"({good:.2f} -> {bad:.2f}); the material is not discriminating and "
            "the 35 dB floor above proves nothing")


def main() -> int:
    return pytest.main([os.path.abspath(__file__), "-q"] + sys.argv[1:])


if __name__ == "__main__":
    raise SystemExit(main())
