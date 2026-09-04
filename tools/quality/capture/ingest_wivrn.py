#!/usr/bin/env python3
"""Turn a ``WIVRN_RAW_DUMP`` directory into a harness sequence.

The capture branch of WiVRn NX (``server/encoder/raw_dump.{h,cpp}``, branch
``nx-warp-capture``) taps the encoder's *input*: the pixels the encoder saw,
plus the pose the frame was rendered for.  It writes, per eye stream:

    <dir>/stream<i>.yuv        appended raw frames, NV12 or P010, no container
    <dir>/stream<i>.jsonl      one JSON object per frame in that .yuv
    <dir>/stream<i>-info.json  geometry and pixel format, written once

That is not what the quality harness eats.  The harness wants one *coded
picture* per frame -- both eyes side by side, the layout every corpus entry
uses (`corpus/MANIFEST.json`, `vr-mixed-1024-v2`: 2048x1024 for 1024-per-eye)
-- as headerless **planar** 8-bit YUV, plus a version 2 `.poses.json` sidecar
(`docs/WARP.md` 2.1) and a `nxq.sequence.Sequence` descriptor.

This script is that conversion, and it is deliberately the only place where the
two formats meet::

    python3 tools/quality/capture/ingest_wivrn.py --dump $NXQ_SCRATCH/rawdump \\
        --name wivrn-vrchat-1440 --pix yuv420p

It then runs `tools/quality/capture/run_gates.sh` on the sidecar it printed.

What it actually converts
-------------------------

**Pixels.**  NV12 is planar Y followed by an *interleaved* CbCr plane; the
harness wants three separate planes, so the CbCr plane is deinterleaved.  No
resampling and no colour conversion happens: the chroma really is 4:2:0 at the
encoder's input, and `yuv444p` output (`--pix yuv444p`) upsamples by pixel
replication, which invents no detail and says so in the sidecar.

**Bit depth.**  P010 carries 10-bit values in the top 10 bits of 16-bit
little-endian samples.  The harness is 8-bit throughout (`nxq/yuv.py`
``PIX_FMTS``), so a 10-bit capture is down-converted -- video-range, so the
map is ``v8 = v10 / 4`` (10-bit black 64 -> 8-bit black 16) -- with **ordered
(Bayer 8x8) dithering** by default, because truncating two bits of a smooth
gradient makes banding that no codec produced and that every metric then
attributes to the codec.  ``--dither none`` truncates instead.  Either way the
choice is written into the pose log's ``capture`` block, because a quality
number measured on dithered material and one measured on truncated material are
not the same number.

**Poses.**  This is the part that is silently wrong-able, so it is done
explicitly rather than by copying bytes across.  WiVRn hands `raw_dump` an
``XrPosef`` straight out of ``xrLocateViews``, which is OpenXR's own
convention:

===========================  ==========================  =================
quantity                     OpenXR / WiVRn              `nxv-openxr-1`
===========================  ==========================  =================
quaternion component order   ``x, y, z, w``              ``x, y, z, w``
handedness                   right, active rotation      right, active
what the quaternion rotates  view space -> reference     camera -> world
camera axes                  x right, y up, -z forward   same
field of view                ``XrFovf`` half-angles,     ``fov_sign``
                             left and down negative      ``xrfovf``
which pose                   the render pose (the        ``pose_kind``
                             frame was drawn with it)    ``render``
===========================  ==========================  =================

They agree on every row, so the **quaternion conversion is the identity** --
but that is a fact that had to be established, not assumed, and it is asserted
end to end by ``test_ingest_wivrn.py``, which drives a pure-rotation pair
through `nxv-enc` and requires the warped frame to clear 35 dB.  Get the
convention wrong in either direction and that measurement collapses by 20 dB
while nothing else in the pipeline complains (`docs/WARP-AUDIT.md`).

The one place they do **not** agree is the *shape* of the FOV.  `nxv-enc`'s
sidecar reader takes ``fov_deg`` as a symmetric ``{h, v}`` pair and builds a
centred ``K``; a real headset's ``XrFovf`` is asymmetric, often by several
degrees.  This script writes the symmetric equivalent into ``fov_deg`` (which
is what the encoder consumes), the *measured* asymmetric half-angles into
``fov_rad``, and the asymmetry itself into ``capture.fov`` -- and prints a
warning above ``--fov-asymmetry-warn`` degrees.  A capture whose FOV is
strongly asymmetric is measurably harder for the warp than the sidecar admits,
and pretending otherwise is how a codec gets blamed for a projection error.

**Foveation.**  The tap runs after the foveation resample, so the runs in the
``.jsonl`` are the map from encoded pixels back to render pixels.  They are
preserved verbatim (deduplicated; they are usually constant for a session).
A capture whose foveation is *not* degenerate is **refused** unless
``--allow-foveation`` is passed, because its pixels are not on a uniform
angular grid and the warp's homography assumes they are.  See `CAPTURE.md`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from nxq import yuv  # noqa: E402
from nxq.sequence import Sequence  # noqa: E402

#: .../<repo>/tools/quality/capture/ingest_wivrn.py -> <repo>
REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))
MANIFEST = os.path.join(REPO, "corpus", "MANIFEST.json")

#: Written verbatim into every pose log this script produces.  It is the same
#: block `gen_synthetic.py` writes, and it is correct for a WiVRn capture for
#: the reasons in this module's docstring: OpenXR's pose convention and
#: `nxv-openxr-1` agree row for row.
POSE_CONVENTION = {
    "id": "nxv-openxr-1",
    "quaternion": "xyzw",
    "handedness": "right",
    "rotation": "camera_to_world",
    "axes": "x_right_y_up_z_back",
    "image_origin": "top_left",
    "pixel_centre": 0.5,
    "fov_sign": "xrfovf",
    "pose_kind": "render",
    "pairing": "n_minus_1_to_n",
    "position_units": "m",
}

#: Standard 8x8 ordered-dither (Bayer) matrix, values 0..63.
BAYER8 = np.array([
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
], dtype=np.uint16)


class IngestError(RuntimeError):
    """A capture that cannot be honestly converted."""


# ------------------------------------------------------------------ info json


def load_info(dump: str, stream: int) -> dict:
    """Read and *validate* ``stream<i>-info.json``.

    Every field `raw_dump.cpp` writes is checked, because this file is the only
    description of the ``.yuv`` next to it and a wrong one produces a sequence
    that decodes into garbage rather than an error.
    """
    path = os.path.join(dump, f"stream{stream}-info.json")
    if not os.path.exists(path):
        raise IngestError(f"{path} is missing: is this a WIVRN_RAW_DUMP directory?")
    with open(path) as fh:
        try:
            info = json.load(fh)
        except json.JSONDecodeError as e:
            raise IngestError(f"{path}: not JSON ({e}). A dump interrupted mid-write?")

    def need(key):
        if key not in info:
            raise IngestError(f"{path}: no {key!r}")
        return info[key]

    idx = int(need("stream"))
    if idx != stream:
        raise IngestError(f"{path}: says stream {idx}, but it is stream{stream}-info.json")
    w, h = int(need("width")), int(need("height"))
    depth = int(need("bit_depth"))
    pixfmt = str(need("pixel_format"))
    chroma = str(need("chroma"))
    fb = int(need("frame_bytes"))
    eye = str(info.get("eye", "left" if stream == 0 else "right"))

    if chroma != "4:2:0":
        raise IngestError(f"{path}: chroma {chroma!r}, this reader only knows 4:2:0")
    if (depth, pixfmt) not in ((8, "nv12"), (10, "p010le")):
        raise IngestError(
            f"{path}: bit_depth {depth} with pixel_format {pixfmt!r} is not a "
            "combination raw_dump.cpp writes (8/nv12 or 10/p010le)")
    if w <= 0 or h <= 0 or w % 2 or h % 2:
        raise IngestError(f"{path}: {w}x{h} is not a positive even resolution")
    if not (16 <= w <= 4096 and 16 <= h <= 4096):
        raise IngestError(
            f"{path}: {w}x{h} is outside the codec's 16..4096 per-eye range "
            "(include/nxvc/nxvc.h)")
    want = w * h * 3 // 2 * (2 if depth == 10 else 1)
    if fb != want:
        raise IngestError(
            f"{path}: frame_bytes {fb} but {w}x{h} {pixfmt} is {want} bytes. "
            "The info file and the .yuv disagree; nothing downstream can "
            "recover from that.")
    planes = info.get("planes")
    if isinstance(planes, list) and len(planes) == 2:
        y_p, c_p = planes
        if (int(y_p.get("width", w)), int(y_p.get("height", h))) != (w, h):
            raise IngestError(f"{path}: Y plane is not {w}x{h}")
        if (int(c_p.get("width", w // 2)), int(c_p.get("height", h // 2))) != (w // 2, h // 2):
            raise IngestError(f"{path}: CbCr plane is not {w // 2}x{h // 2}")

    info["_width"], info["_height"] = w, h
    info["_depth"], info["_pixfmt"], info["_frame_bytes"] = depth, pixfmt, fb
    info["_eye"] = eye
    return info


# -------------------------------------------------------------------- jsonl


def load_jsonl(dump: str, stream: int) -> list[dict]:
    """Read ``stream<i>.jsonl``, tolerating a truncated final line.

    A dump ends when the session ends, which is not a line boundary: the last
    record can be half written.  Dropping it is correct and dropping it
    silently is not, so the count is reported by the caller against the frame
    count of the ``.yuv``.
    """
    path = os.path.join(dump, f"stream{stream}.jsonl")
    if not os.path.exists(path):
        raise IngestError(f"{path} is missing")
    out, dropped = [], 0
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                dropped += 1
    if dropped:
        print(f"[ingest] {os.path.basename(path)}: dropped {dropped} unparseable "
              f"line(s) (a dump cut off mid-write)")
    if not out:
        raise IngestError(f"{path}: no usable records")
    return out


def _fov_tuple(f: dict) -> tuple[float, float, float, float]:
    return (float(f["tan_left"]), float(f["tan_right"]),
            float(f["tan_up"]), float(f["tan_down"]))


def _fov_key(fovs: list) -> tuple:
    return tuple(_fov_tuple(f) for f in fovs)


def _fov_deg(fovs: list) -> tuple[float, float, float]:
    """(horizontal deg, vertical deg, worst asymmetry deg) over the eyes given.

    ``fov_deg`` in the sidecar is the *total* angle, which is what `nxv-enc`
    halves to build ``K``.  The asymmetry is the residual the symmetric model
    throws away: ``|angleRight + angleLeft|``, which is zero for a symmetric
    frustum and is several degrees on a real headset.
    """
    hs, vs, asym = [], [], 0.0
    for f in fovs:
        tl, tr, tu, td = _fov_tuple(f)
        al, ar = math.atan(tl), math.atan(tr)
        au, ad = math.atan(tu), math.atan(td)
        hs.append(math.degrees(ar - al))
        vs.append(math.degrees(au - ad))
        asym = max(asym, abs(math.degrees(ar + al)), abs(math.degrees(au + ad)))
    return float(np.mean(hs)), float(np.mean(vs)), asym


def _foveation_key(fov_runs: list) -> tuple:
    return tuple((tuple(int(v) for v in e.get("x", [])),
                  tuple(int(v) for v in e.get("y", []))) for e in fov_runs)


def _runs_are_identity(runs: tuple, width: int, height: int) -> bool:
    """A `foveation_parameter` degenerates to a no-op when only the 1:1 run is used.

    The runs are lengths per ratio with the **middle** entry at 1:1
    (``common/wivrn_packets.h``), so a map that resamples nothing has every
    non-middle entry at zero -- and the middle entry then spans the axis.
    """
    for axis, extent in zip(runs, (width, height)):
        if len(axis) == 0:
            continue
        mid = (len(axis) - 1) // 2
        if any(v for i, v in enumerate(axis) if i != mid):
            return False
        if axis[mid] not in (0, extent):
            return False
    return True


# ------------------------------------------------------------------ pixels


def nv12_to_planar(buf: bytes, w: int, h: int) -> yuv.Frame:
    """One NV12 frame -> three planar 8-bit planes.  Pure deinterleave."""
    y = np.frombuffer(buf, np.uint8, count=w * h).reshape(h, w)
    cbcr = np.frombuffer(buf, np.uint8, count=w * h // 2, offset=w * h).reshape(h // 2, w // 2, 2)
    return yuv.Frame(np.ascontiguousarray(y),
                     np.ascontiguousarray(cbcr[..., 0]),
                     np.ascontiguousarray(cbcr[..., 1]))


def _to8(v10: np.ndarray, dither: str) -> np.ndarray:
    """10-bit video-range -> 8-bit video-range (``v8 = v10 / 4``).

    ``ordered`` adds an 8x8 Bayer threshold in 0..3 before the shift, which is
    the exact quantisation step being removed.  Truncation alone puts a visible
    contour on every smooth gradient in the capture, and a contour that the
    capture pipeline invented is a contour the codec gets charged for.
    """
    if dither == "none":
        return np.clip(v10 >> 2, 0, 255).astype(np.uint8)
    h, w = v10.shape
    t = np.tile(BAYER8 >> 4, ((h + 7) // 8, (w + 7) // 8))[:h, :w]
    return np.clip((v10 + t) >> 2, 0, 255).astype(np.uint8)


def p010_to_planar(buf: bytes, w: int, h: int, dither: str) -> yuv.Frame:
    """One P010 frame -> three planar 8-bit planes, down-converted."""
    y16 = np.frombuffer(buf, "<u2", count=w * h).reshape(h, w)
    c16 = np.frombuffer(buf, "<u2", count=w * h // 2, offset=w * h * 2).reshape(h // 2, w // 2, 2)
    y10 = (y16 >> 6).astype(np.uint16)
    cb10 = (np.ascontiguousarray(c16[..., 0]) >> 6).astype(np.uint16)
    cr10 = (np.ascontiguousarray(c16[..., 1]) >> 6).astype(np.uint16)
    return yuv.Frame(_to8(y10, dither), _to8(cb10, dither), _to8(cr10, dither))


def upsample_chroma(frame: yuv.Frame) -> yuv.Frame:
    """4:2:0 -> 4:4:4 by pixel replication.  Invents no detail."""
    def up(p):
        return np.ascontiguousarray(np.repeat(np.repeat(p, 2, axis=0), 2, axis=1))
    return yuv.Frame(frame.y, up(frame.u), up(frame.v))


def compose(frames: list[yuv.Frame]) -> yuv.Frame:
    """One or two eye pictures -> the coded picture the harness indexes.

    Side by side, eye 0 first, which is the layout every stereo corpus entry
    uses and the layout `nxv-enc --eyes 2` expects (`--w` is the FULL width).
    """
    if len(frames) == 1:
        return frames[0]
    return yuv.Frame(np.hstack([f.y for f in frames]),
                     np.hstack([f.u for f in frames]),
                     np.hstack([f.v for f in frames]))


# -------------------------------------------------------------------- poses


def quat_angle_deg(a, b) -> float:
    """Angle of the relative rotation between two unit quaternions, degrees."""
    dot = min(1.0, abs(sum(x * y for x, y in zip(a, b))))
    return math.degrees(2.0 * math.acos(dot))


def build_pose_log(records: list[list[dict]], eye_w: int, eye_h: int, fps: float,
                   capture: dict, warn_asym: float, quiet: bool) -> dict:
    """The version 2 `.poses.json` (docs/WARP.md 2.1) for the joined records.

    ``records[i]`` is the list of per-stream jsonl objects for output frame
    ``i``.  Both eyes carry the same ``view_info_t``, so the pose is read from
    stream 0 and stream 1 is used as a cross-check.

    Key order matters here and is not cosmetic: `nxv-enc`'s sidecar reader is a
    scraper that takes the FIRST ``"id"`` and the FIRST ``"fov_deg"`` in the
    file and EVERY ``"orientation_xyzw"``.  ``convention`` and ``fov_deg`` are
    therefore written first, the bulky ``capture`` block after them, and no key
    anywhere else in the document is spelled ``orientation_xyzw``.
    """
    fov_variants: list[tuple] = []
    fov_index: list[int] = []
    fovea_variants: list[tuple] = []
    fovea_index: list[int] = []
    frames: list[dict] = []
    eye_mismatch_deg = 0.0
    prev_q = None
    prev_t = None

    for i, per_stream in enumerate(records):
        r = per_stream[0]
        poses = r["pose"]
        q = [float(v) for v in poses[0]["orientation"]]
        p = [float(v) for v in poses[0]["position"]]
        # Both eye views share an orientation in every runtime that reports a
        # rigid head; a runtime that canted the eyes would break the single
        # homography per frame that `warp_ext()` carries, so measure it.
        if len(poses) > 1:
            q_r = [float(v) for v in poses[1]["orientation"]]
            eye_mismatch_deg = max(eye_mismatch_deg, quat_angle_deg(q, q_r))
        else:
            q_r = q
        p_r = ([float(v) for v in poses[1]["position"]] if len(poses) > 1 else p)

        key = _fov_key(r["fov"])
        if key not in fov_variants:
            fov_variants.append(key)
        fov_index.append(fov_variants.index(key))

        fkey = _foveation_key(r.get("foveation", []))
        if fkey not in fovea_variants:
            fovea_variants.append(fkey)
        fovea_index.append(fovea_variants.index(fkey))

        t_ns = int(r.get("display_time_ns", 0))
        if prev_t is not None and t_ns > prev_t:
            dt = (t_ns - prev_t) / 1e9
        else:
            dt = 1.0 / fps
        av = 0.0 if prev_q is None else quat_angle_deg(prev_q, q) / max(dt, 1e-9)
        prev_q, prev_t = q, t_ns

        frames.append({
            "frame": i,
            "time_s": (t_ns - int(records[0][0].get("display_time_ns", 0))) / 1e9,
            "orientation_xyzw": q,
            "position_xyz": p,
            "angular_velocity_deg_s": av,
            "display_time_ns": t_ns,
            "src_frame": int(r.get("frame", i)),
            "right_eye_quat_xyzw": q_r,
            "right_eye_pos_xyz": p_r,
            "fov_variant": fov_index[-1],
            "foveation_variant": fovea_index[-1],
        })

    h_deg, v_deg, asym = _fov_deg(records[0][0]["fov"])
    tl, tr, tu, td = _fov_tuple(records[0][0]["fov"][0])

    identity_foveation = all(
        _runs_are_identity(runs, eye_w, eye_h)
        for variant in fovea_variants for runs in variant)

    if not quiet:
        if asym > warn_asym:
            print(f"[ingest] WARNING: the frustum is asymmetric by {asym:.2f} deg, "
                  f"above --fov-asymmetry-warn {warn_asym:.2f}.")
            print("[ingest]   nxv-enc builds a CENTRED K from fov_deg (docs/WARP.md 2.1), "
                  "so the warp\n"
                  "[ingest]   is derived for a frustum this capture does not have. The "
                  "measured\n"
                  "[ingest]   half-angles are in fov_rad and capture.fov; read every "
                  "number on this\n"
                  "[ingest]   sequence knowing the projection is approximated.")
        if eye_mismatch_deg > 0.01:
            print(f"[ingest] WARNING: the two eye orientations differ by up to "
                  f"{eye_mismatch_deg:.3f} deg; one homography per frame covers both.")

    doc = {
        "version": 2,
        "convention": POSE_CONVENTION,
        "fov_deg": {"h": h_deg, "v": v_deg},
        "fov_rad": {"left": math.atan(tl), "right": math.atan(tr),
                    "up": math.atan(tu), "down": math.atan(td)},
        "eye": {"width": eye_w, "height": eye_h},
        "fps": fps,
    }
    cap = dict(capture)
    cap["fov"] = {
        "measured_tangents_per_eye": [
            {"tan_left": t[0], "tan_right": t[1], "tan_up": t[2], "tan_down": t[3]}
            for t in fov_variants[0]
        ],
        "variants": len(fov_variants),
        "per_frame_variant": fov_index if len(fov_variants) > 1 else "constant",
        "asymmetry_deg": asym,
        "note": "fov_deg above is the SYMMETRIC total angle nxv-enc consumes; "
                "the frustum this capture was rendered with is the asymmetric "
                "one recorded here. The difference is a projection error the "
                "warp cannot see and the codec gets charged for.",
    }
    cap["eye_orientation_max_delta_deg"] = eye_mismatch_deg
    cap["foveation"] = {
        "identity": identity_foveation,
        "note": "run lengths per ratio, middle entry 1:1 "
                "(common/wivrn_packets.h foveation_parameter). Preserved so an "
                "encoded pixel can be mapped back to a render pixel; nothing in "
                "the harness undoes the resample.",
        "variants": [[{"x": list(runs[0]), "y": list(runs[1])} for runs in variant]
                     for variant in fovea_variants],
        "per_frame_variant": fovea_index if len(fovea_variants) > 1 else "constant",
    }
    doc["capture"] = cap
    doc["frames"] = frames
    return doc


# ------------------------------------------------------------------ manifest


def sha256_file(path: str, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for b in iter(lambda: fh.read(chunk), b""):
            h.update(b)
    return h.hexdigest()


def corpus_root(explicit: str | None) -> str:
    if explicit:
        return os.path.abspath(explicit)
    if os.environ.get("NXW_CORPUS"):
        return os.path.abspath(os.environ["NXW_CORPUS"])
    with open(MANIFEST) as fh:
        return os.path.abspath(json.load(fh)["default_root"])


def register(name: str, out_dir: str, root: str, files: list[str], entry: dict,
             quiet: bool) -> bool:
    """Add or replace ``name`` in `corpus/MANIFEST.json`, with hashes.

    Returns False (and says why) when the output is not under the corpus root,
    because a manifest path is relative to that root and a path that escapes it
    is not reproducible on another machine.
    """
    try:
        rels = [os.path.relpath(f, root) for f in files]
    except ValueError:
        rels = None
    if rels is None or any(r.startswith("..") for r in rels):
        print(f"[ingest] not registering {name}: {out_dir} is outside the corpus "
              f"root {root}. Pass --out inside it, or set $NXW_CORPUS.")
        return False

    with open(MANIFEST) as fh:
        m = json.load(fh)
    entry = dict(entry)
    entry["files"] = [{"path": r, "sha256": sha256_file(f), "bytes": os.path.getsize(f)}
                      for r, f in zip(rels, files)]
    for i, e in enumerate(m["entries"]):
        if e["name"] == name:
            m["entries"][i] = entry
            break
    else:
        m["entries"].append(entry)
    with open(MANIFEST, "w") as fh:
        json.dump(m, fh, indent=2)
        fh.write("\n")
    if not quiet:
        print(f"[ingest] registered {name} in corpus/MANIFEST.json "
              f"({len(entry['files'])} files, hashes pinned)")
    return True


# ---------------------------------------------------------------------- main


def ingest(dump: str, name: str, out_dir: str, pix_fmts: list[str], layout: str,
           eye: str, fps: float | None, max_frames: int, dither: str,
           allow_foveation: bool, warn_asym: float, quiet: bool) -> dict:
    log = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))
    if "." in name:
        raise IngestError(
            f"--name {name!r} contains a dot. The harness derives "
            "<name>.poses.json from the sidecar's basename up to the first dot "
            "(ref/warp_chain.py), so a dotted name silently loses its poses.")

    streams = [0, 1] if layout == "sbs" else [0 if eye == "left" else 1]
    infos = [load_info(dump, s) for s in streams]
    logs = [load_jsonl(dump, s) for s in streams]

    w, h = infos[0]["_width"], infos[0]["_height"]
    depth, pixfmt = infos[0]["_depth"], infos[0]["_pixfmt"]
    fb = infos[0]["_frame_bytes"]
    for info in infos[1:]:
        if (info["_width"], info["_height"], info["_depth"], info["_pixfmt"]) != \
           (w, h, depth, pixfmt):
            raise IngestError(
                "the two eye streams disagree on geometry or format "
                f"({w}x{h} {pixfmt} vs {info['_width']}x{info['_height']} "
                f"{info['_pixfmt']}); they cannot be one side-by-side picture")

    log(f"[ingest] {dump}")
    log(f"[ingest]   {len(streams)} stream(s), {w}x{h} per eye, {pixfmt} "
        f"{depth}-bit, {fb} bytes/frame")

    # How many frames actually exist: the .yuv is appended to and the .jsonl is
    # written after it, so either can be the shorter one.
    counts = []
    for s, info in zip(streams, infos):
        path = os.path.join(dump, f"stream{s}.yuv")
        if not os.path.exists(path):
            raise IngestError(f"{path} is missing")
        size = os.path.getsize(path)
        n_yuv, rem = divmod(size, fb)
        if rem:
            log(f"[ingest]   stream{s}.yuv: {rem} trailing bytes (a partial frame), dropped")
        counts.append(min(n_yuv, len(logs[streams.index(s)])))
        log(f"[ingest]   stream{s}: {n_yuv} frames on disk, {len(logs[streams.index(s)])} "
            f"pose records -> {counts[-1]} usable")
    n = min(counts)
    if n == 0:
        raise IngestError("no complete frame in this dump")

    # Join by the encoder's frame index rather than by position: a stream that
    # dropped a frame would otherwise pair the wrong pose with the wrong eye,
    # which is a warp error nothing downstream can detect.
    by_id = [{int(r.get("frame", i)): r for i, r in enumerate(lg[:counts[k]])}
             for k, lg in enumerate(logs)]
    order = [int(r.get("frame", i)) for i, r in enumerate(logs[0][:counts[0]])]
    common = [fid for fid in order if all(fid in d for d in by_id)]
    if len(common) < n:
        log(f"[ingest]   {n - len(common)} frame(s) present in one eye only, dropped")
    if max_frames:
        common = common[:max_frames]
    if not common:
        raise IngestError("the two streams share no frame index")
    # Position of each kept frame id inside each stream's file.
    pos = [{int(r.get("frame", i)): i for i, r in enumerate(lg[:counts[k]])}
           for k, lg in enumerate(logs)]
    records = [[by_id[k][fid] for k in range(len(streams))] for fid in common]

    if fps is None:
        ts = [int(r[0].get("display_time_ns", 0)) for r in records]
        dts = [b - a for a, b in zip(ts, ts[1:]) if b > a]
        fps = round(1e9 / float(np.median(dts)), 3) if dts else 90.0
        log(f"[ingest]   {fps} fps, from the median display-time delta")

    # Foveation gate. The tap is downstream of the foveation resample, so a
    # foveated capture's pixels are not on a uniform angular grid -- which is
    # exactly what derive_homography() assumes. Refuse rather than measure.
    fovea_ok = all(
        _runs_are_identity(runs, w, h)
        for rec in records
        for entry in rec
        for runs in _foveation_key(entry.get("foveation", [])))
    if not fovea_ok:
        if not allow_foveation:
            raise IngestError(
                "this capture is FOVEATED: the .jsonl carries non-degenerate "
                "foveation runs.\n"
                "  The tap sits after the foveation resample, so its pixels are "
                "not on a uniform\n"
                "  angular grid, and the pose homography (docs/WARP.md 4) "
                "assumes they are. Every\n"
                "  warp number measured on it would be wrong by an unknown "
                "amount.\n"
                "  Turn foveation off on the headset and re-record "
                "(tools/quality/capture/CAPTURE.md),\n"
                "  or pass --allow-foveation to convert it anyway -- the runs "
                "are preserved in the\n"
                "  pose log either way, and the sidecar records that it is not "
                "gate material.")
        log("[ingest]   FOVEATED capture converted under --allow-foveation: "
            "NOT gate material")

    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    out_w = w * len(streams)
    out_h = h

    writers = {}
    for pf in pix_fmts:
        fmt = yuv.Format(out_w, out_h, pf)
        path = os.path.join(out_dir, f"{name}.{pf}.yuv")
        writers[pf] = (fmt, yuv.SequenceWriter(path, fmt), path)

    handles = [open(os.path.join(dump, f"stream{s}.yuv"), "rb") for s in streams]
    try:
        for i, fid in enumerate(common):
            eyes = []
            for k, fh in enumerate(handles):
                fh.seek(pos[k][fid] * fb)
                buf = fh.read(fb)
                if len(buf) != fb:
                    raise IngestError(f"stream{streams[k]}.yuv: short read at frame {fid}")
                eyes.append(nv12_to_planar(buf, w, h) if depth == 8
                            else p010_to_planar(buf, w, h, dither))
            f420 = compose(eyes)
            f444 = upsample_chroma(f420)
            for pf, (fmt, wr, _) in writers.items():
                wr.write(f420 if pf == "yuv420p" else f444)
            if not quiet and (i % 50 == 0 or i == len(common) - 1):
                log(f"[ingest]   frame {i + 1}/{len(common)}")
    finally:
        for fh in handles:
            fh.close()

    capture = {
        "source": "WiVRn NX raw_dump (server/encoder/raw_dump.{h,cpp}, branch "
                  "nx-warp-capture): the encoder's input image plus the "
                  "view_info_t the frame was rendered for",
        "dump_dir": os.path.abspath(dump),
        "streams": len(streams),
        "layout": layout,
        "input_pixel_format": pixfmt,
        "input_bit_depth": depth,
        "bit_depth_conversion": (
            "none (8-bit in, 8-bit out)" if depth == 8 else
            f"P010 10-bit -> 8-bit video range (v8 = v10/4), dither={dither}"),
        "dither": dither if depth == 10 else "n/a",
        "chroma": "4:2:0 as captured; yuv444p output is pixel replication, no "
                  "detail is invented",
        "frames_in_dump": n,
        "frames_ingested": len(common),
        "gate_material": bool(fovea_ok),
        "ingested_by": "tools/quality/capture/ingest_wivrn.py",
    }
    poses_doc = build_pose_log(records, w, h, fps, capture, warn_asym, quiet)
    pose_path = os.path.join(out_dir, f"{name}.poses.json")
    with open(pose_path, "w") as fh:
        json.dump(poses_doc, fh, indent=1)
        fh.write("\n")

    sidecars = []
    for pf, (fmt, wr, path) in writers.items():
        wr.close()
        seq = Sequence(
            name=f"{name}.{pf}", path=path, width=out_w, height=out_h,
            pix_fmt=pf, fps=fps, frames=len(common), pose_log=pose_path,
            source=f"wivrn-raw-dump:{pixfmt}{depth}:{layout}",
            layout=layout if layout == "mono" else "sbs",
        )
        sc = os.path.join(out_dir, f"{name}.{pf}.json")
        seq.save(sc)
        sidecars.append(sc)
        log(f"[ingest] wrote {path} ({os.path.getsize(path) / 1e6:.1f} MB) and {sc}")
    log(f"[ingest] pose log: {pose_path}")

    av = [f["angular_velocity_deg_s"] for f in poses_doc["frames"][1:]]
    if av:
        log(f"[ingest] head motion: {min(av):.1f} to {max(av):.1f} deg/s, "
            f"mean {float(np.mean(av)):.1f}")
        if max(av) < 5.0:
            log("[ingest] WARNING: almost no head motion in this take. The kill "
                "test (PAPER 2.11\n"
                "[ingest]   item 1) splits BD-rate at the fastest 20 percent of "
                "frames; a still take\n"
                "[ingest]   has no motion side to split. Record the 10 seconds of "
                "head rotation.")

    return {
        "name": name, "out_dir": out_dir, "sidecars": sidecars,
        "pose_log": pose_path, "frames": len(common), "fps": fps,
        "width": out_w, "height": out_h, "pix_fmt": pix_fmts,
        "files": [writers[pf][2] for pf in pix_fmts] + [pose_path],
        "gate_material": bool(fovea_ok),
        "capture": capture, "poses": poses_doc,
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", required=True, help="the WIVRN_RAW_DUMP directory")
    ap.add_argument("--name", required=True,
                    help="corpus entry / sequence base name, e.g. wivrn-vrchat-1440 "
                         "(no dots: the harness splits on the first one)")
    ap.add_argument("--out", default=None,
                    help="output directory (default: the corpus root, $NXW_CORPUS)")
    ap.add_argument("--corpus-root", default=None,
                    help="corpus root for manifest paths (default $NXW_CORPUS, "
                         "else MANIFEST.json's default_root)")
    ap.add_argument("--pix", default="yuv420p",
                    help="comma-separated output formats (default yuv420p; "
                         "yuv444p doubles the size and replicates chroma)")
    ap.add_argument("--layout", default="sbs", choices=("sbs", "mono"),
                    help="sbs joins both eye streams into one coded picture "
                         "(what nxv-enc --eyes 2 wants); mono takes one eye")
    ap.add_argument("--eye", default="left", choices=("left", "right"),
                    help="with --layout mono, which stream to take")
    ap.add_argument("--fps", type=float, default=None,
                    help="override the rate (default: median display-time delta)")
    ap.add_argument("--frames", type=int, default=0, help="ingest at most N frames")
    ap.add_argument("--dither", default="ordered", choices=("ordered", "none"),
                    help="10-bit -> 8-bit dithering (default ordered/Bayer 8x8)")
    ap.add_argument("--allow-foveation", action="store_true",
                    help="convert a foveated capture anyway; it is marked as not "
                         "gate material in the sidecar")
    ap.add_argument("--fov-asymmetry-warn", type=float, default=1.0, metavar="DEG",
                    help="warn when the frustum's asymmetry exceeds this (default 1)")
    ap.add_argument("--no-manifest", action="store_true",
                    help="do not touch corpus/MANIFEST.json")
    ap.add_argument("--class", dest="klass", default="wivrn-capture",
                    help="manifest class (default wivrn-capture)")
    ap.add_argument("--note", default=None, help="manifest note for this entry")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    pix_fmts = [p.strip() for p in args.pix.split(",") if p.strip()]
    for p in pix_fmts:
        if p not in yuv.PIX_FMTS:
            ap.error(f"unsupported pix fmt {p!r}, want {yuv.PIX_FMTS}")

    root = corpus_root(args.corpus_root)
    out_dir = os.path.abspath(args.out) if args.out else root

    try:
        res = ingest(args.dump, args.name, out_dir, pix_fmts, args.layout, args.eye,
                     args.fps, args.frames, args.dither, args.allow_foveation,
                     args.fov_asymmetry_warn, args.quiet)
    except IngestError as e:
        print(f"ingest_wivrn: {e}", file=sys.stderr)
        return 1

    if not args.no_manifest:
        note = args.note or (
            f"Real WiVRn NX session, ingested by tools/quality/capture/"
            f"ingest_wivrn.py from {os.path.abspath(args.dump)} on "
            f"{res['frames']} frames. "
            + ("Foveation degenerate, uniform angular grid: gate material. "
               if res["gate_material"] else
               "FOVEATED capture: the pixels are not on a uniform angular grid, "
               "so the pose homography does not describe them. NOT gate material. ")
            + "PRIVATE RECORDING: never commit the data, never publish it.")
        entry = {
            "name": args.name,
            "kind": "capture",
            "class": args.klass,
            "resolution": f"{res['width']}x{res['height']}",
            "frames": res["frames"],
            "fps": res["fps"],
            "pix_fmt": pix_fmts,
            "pose_log": os.path.basename(res["pose_log"]),
            "source": "WiVRn NX session, encoder input tap "
                      "(server/encoder/raw_dump.cpp, WIVRN_RAW_DUMP)",
            "license": "Private capture. Never commit, never publish: it is a "
                       "recording of someone's session.",
            "note": note,
        }
        register(args.name, out_dir, root, res["files"], entry, args.quiet)

    print()
    print("Next:")
    print(f"  tools/quality/capture/run_gates.sh {res['sidecars'][0]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
