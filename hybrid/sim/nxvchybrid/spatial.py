"""The *spatial* hybrid: hardware HEVC periphery plus an NX Warp fovea inset.

ADR 0022 rejected the *layered* hybrid (a full-frame HEVC base with a
pose-warped enhancement layer on top of every pixel).  Its argument is a
rate-distortion one -- marginal bits are worth more to x265 than to our
enhancement layer, so the optimiser drives the base share to 100% -- and it
does not reach the arrangement measured here, which is disjoint in space
rather than layered in quality:

* the **periphery** is an ordinary full-resolution HEVC stream, decoded by
  MediaCodec exactly as WiVRn does today, carrying ``base_frac`` of the total
  bitrate;
* the **inset** is a square crop of each eye, centred on the lens axis, coded
  by the real NX Warp codec (``nxv-enc``/``nxv-dec`` from ``build-ref``,
  intra + inter, its own rate control) with the remaining bits;
* the client composites the inset over the periphery in the reprojection pass
  with a feathered boundary.

No pixel is coded twice, so there is no "which layer gets the marginal bit"
question at all.  The trade is different: the inset buys a much higher bit
*density* over a small area, and the whole thing is bounded by what the
Adreno 650 can decode -- which is why the inset is measured in tiles.

Unlike :mod:`nxvchybrid.hybrid`, this module does **not** model the codec.  It
runs it.  The only modelled component left is the base layer, and that is x265
standing in for a hardware HEVC encoder, exactly as in :mod:`nxvchybrid.base`.

Bitrates follow the same convention as the rest of the simulator: every
``mbit`` figure is the **2 x 2048^2 x 90 Hz equivalent**, and the simulator's
own 2 x 1024^2 frame is charged a quarter of it (:func:`sim_bps`).
"""

from __future__ import annotations

import json
import math
import os
import sys
import time
from dataclasses import dataclass, asdict

import numpy as np

from . import base as basemod, cpu

# The device operating point every bitrate in this file is quoted at.
DEVICE_PIXELS_PER_FRAME = 2 * 2048 * 2048

#: Tile side, PAPER.md 6.2.  Inset sizes and offsets are multiples of it so
#: that the inset's tile grid is a sub-grid of the full frame's.
TILE = 64

#: Where the real codec lives.  The default is the ``build-ref`` tree of the
#: checkout this file belongs to; ``NXVCH_CODEC_DIR`` overrides it, which is
#: what a *worktree* needs, since ``build-ref`` is untracked and lives in the
#: main checkout.
DEFAULT_CODEC_DIR = os.environ.get("NXVCH_CODEC_DIR") or os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "build-ref", "bin")
)


class SpatialError(RuntimeError):
    pass


# --- geometry ------------------------------------------------------------


def focal_px(width_px: int, hfov_deg: float) -> float:
    """Pixels per radian on axis for a rectilinear (tan) projection."""
    return (width_px / 2.0) / math.tan(math.radians(hfov_deg) / 2.0)


def crop_fov_deg(eye_px: int, hfov_deg: float, crop_px: int) -> float:
    """The field of view a centred *crop_px* crop of an *eye_px* view spans.

    The principal point does not move under a centred crop and the focal
    length in pixels does not change, so the crop is a valid pinhole camera of
    its own and the codec's rotation-only homography stays exact for it.  A
    wrong FOV here is a silently wrong warp (docs/WARP.md 2.1), which is why
    this is computed rather than assumed.
    """
    f = focal_px(eye_px, hfov_deg)
    return 2.0 * math.degrees(math.atan((crop_px / 2.0) / f))


def half_angle_to_px(half_deg: float, ppd_center: float) -> float:
    """Half-width in pixels of a box subtending *half_deg* off axis."""
    f = ppd_center * 180.0 / math.pi
    return f * math.tan(math.radians(half_deg))


def tiles_for(inset_px: int, eyes: int = 2) -> int:
    per_eye = (inset_px // TILE) ** 2
    return per_eye * eyes


# --- rate bookkeeping ----------------------------------------------------


def sim_bps(mbit: float, width: int, height: int) -> float:
    """Device-equivalent Mbit/s -> bits per second at the simulator's size."""
    return mbit * 1e6 * (width * height) / DEVICE_PIXELS_PER_FRAME


def mbit_from_bits(bits: float, frames: int, width: int, height: int,
                   fps: float = 90.0) -> float:
    """Total coded bits -> the device-equivalent Mbit/s they represent."""
    per_frame = bits / max(1, frames)
    return per_frame * fps * DEVICE_PIXELS_PER_FRAME / (width * height) / 1e6


# --- YUV plumbing --------------------------------------------------------


@dataclass(frozen=True)
class Geometry:
    """One sequence's raw layout, and the eye split of a side-by-side frame."""

    width: int
    height: int
    pix_fmt: str
    frames: int
    fps: float = 90.0
    layout: str = "sbs"

    @property
    def chroma_shift(self) -> int:
        return 1 if self.pix_fmt == "yuv420p" else 0

    @property
    def cw(self) -> int:
        return self.width >> self.chroma_shift

    @property
    def ch(self) -> int:
        return self.height >> self.chroma_shift

    @property
    def frame_bytes(self) -> int:
        return self.width * self.height + 2 * self.cw * self.ch

    @property
    def eye_width(self) -> int:
        return self.width // 2 if self.layout == "sbs" else self.width

    def read(self, path: str, i: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        with open(path, "rb") as fh:
            fh.seek(i * self.frame_bytes)
            buf = np.frombuffer(fh.read(self.frame_bytes), dtype=np.uint8)
        if buf.size != self.frame_bytes:
            raise SpatialError(f"{path}: short read at frame {i}")
        n = self.width * self.height
        c = self.cw * self.ch
        return (buf[:n].reshape(self.height, self.width),
                buf[n:n + c].reshape(self.ch, self.cw),
                buf[n + c:].reshape(self.ch, self.cw))


def load_sequence(sidecar: str) -> tuple[str, Geometry, str]:
    """Read a ``tools/quality`` sequence sidecar.  Returns (yuv, geometry, poses)."""
    with open(sidecar) as fh:
        d = json.load(fh)
    base = os.path.dirname(os.path.abspath(sidecar))
    yuv = os.path.join(base, d["path"])
    poses = os.path.join(base, d["pose_log"]) if d.get("pose_log") else ""
    geo = Geometry(int(d["width"]), int(d["height"]), d.get("pix_fmt", "yuv420p"),
                   int(d["frames"]), float(d.get("fps", 90.0)),
                   d.get("layout", "mono"))
    return yuv, geo, poses


def extract_inset(src: str, geo: Geometry, inset: int, out: str) -> Geometry:
    """Write the per-eye centred inset crops of *src* as one side-by-side clip.

    The output is ``2 * inset`` wide so that ``nxv-enc --eyes 2`` sees two
    square pictures, which is the same shape the full frame has.
    """
    if geo.layout != "sbs":
        raise SpatialError("the spatial hybrid is defined on a side-by-side stereo clip")
    eye = geo.eye_width
    if inset > eye:
        raise SpatialError(f"inset {inset} is larger than the eye ({eye})")
    off = (eye - inset) // 2
    if off % TILE or inset % TILE:
        raise SpatialError(
            f"inset {inset} at offset {off} is not on the {TILE}-px tile grid; "
            "the inset must cover whole tiles of the full frame"
        )
    cs = geo.chroma_shift
    ci, co = inset >> cs, off >> cs
    ce = eye >> cs
    with open(out, "wb") as fh:
        for i in range(geo.frames):
            y, u, v = geo.read(src, i)
            fh.write(np.hstack([y[off:off + inset, e * eye + off: e * eye + off + inset]
                                for e in (0, 1)]).tobytes())
            for p in (u, v):
                fh.write(np.hstack([p[co:co + ci, e * ce + co: e * ce + co + ci]
                                    for e in (0, 1)]).tobytes())
    return Geometry(2 * inset, inset, geo.pix_fmt, geo.frames, geo.fps, "sbs")


def feather_alpha(inset: int, feather: int) -> np.ndarray:
    """Smoothstep blend weight for one inset: 0 at its border, 1 *feather* in."""
    if feather <= 0:
        return np.ones((inset, inset), dtype=np.float32)
    idx = np.arange(inset, dtype=np.float32)
    d = np.minimum(idx + 0.5, inset - idx - 0.5)          # distance to the border
    t = np.clip(d / float(feather), 0.0, 1.0)
    s = t * t * (3.0 - 2.0 * t)
    return np.minimum(s[:, None], s[None, :]).astype(np.float32)


def composite(periphery: str, inset_yuv: str, geo: Geometry, inset: int,
              feather: int, out: str) -> None:
    """Paste the decoded inset over the decoded periphery with a feathered edge.

    This is what the reprojection pass would do: one extra tap inside the inset
    rectangle, cross-faded over *feather* pixels so that the seam is a gradient
    rather than a step.  Both inputs are in the same 4:2:0 (or 4:4:4) domain,
    which is the arrangement docs/INTEGRATION.md 1.3 option 1 already gives the
    client.
    """
    eye = geo.eye_width
    off = (eye - inset) // 2
    cs = geo.chroma_shift
    ig = Geometry(2 * inset, inset, geo.pix_fmt, geo.frames, geo.fps, "sbs")
    a = feather_alpha(inset, feather)
    ac = a[::(1 << cs), ::(1 << cs)] if cs else a
    ci, co, ce = inset >> cs, off >> cs, eye >> cs
    with open(out, "wb") as fh:
        for i in range(geo.frames):
            planes = [p.copy() for p in geo.read(periphery, i)]
            ins = ig.read(inset_yuv, i)
            for pi, (dst, srcp) in enumerate(zip(planes, ins)):
                w = a if pi == 0 else ac
                sz = inset if pi == 0 else ci
                o = off if pi == 0 else co
                st = eye if pi == 0 else ce
                for e in (0, 1):
                    tgt = dst[o:o + sz, e * st + o: e * st + o + sz].astype(np.float32)
                    src = srcp[:, e * sz:(e + 1) * sz].astype(np.float32)
                    dst[o:o + sz, e * st + o: e * st + o + sz] = np.rint(
                        w * src + (1.0 - w) * tgt).astype(np.uint8)
            for p in planes:
                fh.write(p.tobytes())


# --- the codec -----------------------------------------------------------


@dataclass
class CodecPaths:
    enc: str
    dec: str

    @classmethod
    def at(cls, d: str) -> "CodecPaths":
        c = cls(os.path.join(d, "nxv-enc"), os.path.join(d, "nxv-dec"))
        for p in (c.enc, c.dec):
            if not os.path.isfile(p):
                raise SpatialError(
                    f"{p} is missing; build the reference codec first "
                    "(cmake --build build-ref)"
                )
        return c


def write_crop_poses(poses: str, eye: int, hfov: float, inset: int, out: str) -> float:
    """Copy a pose sidecar with the geometry of the crop, and return its FOV."""
    with open(poses) as fh:
        d = json.load(fh)
    fov = crop_fov_deg(eye, hfov, inset)
    d["fov_deg"] = {"h": fov, "v": fov}
    d["eye"] = {"width": inset, "height": inset}
    d["crop"] = {"of_eye_px": eye, "inset_px": inset, "parent_fov_deg": hfov,
                 "note": "centred crop; principal point and focal length unchanged"}
    with open(out, "w") as fh:
        json.dump(d, fh)
    return fov


def encode_inset(codec: CodecPaths, src: str, geo: Geometry, qp: int, poses: str,
                 fov: float, out: str, extra: list[str] | None = None) -> int:
    cmd = [codec.enc, "--in", src, "--w", str(geo.width), "--h", str(geo.height),
           "--pix", geo.pix_fmt, "--qp", str(qp), "--eyes", "2",
           "--inter", "on", "--poses", poses, "--fov", f"{fov:.4f},{fov:.4f}",
           "--quiet", "--out", out]
    if extra:
        cmd += extra
    p = cpu.run(cmd, check=False, timeout=1800.0)
    if p.returncode != 0:
        tail = "\n".join((p.stderr or "").strip().splitlines()[-10:])
        raise SpatialError(f"nxv-enc failed (exit {p.returncode}) at qp {qp}:\n{tail}")
    return 8 * os.path.getsize(out)


def decode_inset(codec: CodecPaths, nxv: str, out: str) -> None:
    p = cpu.run([codec.dec, "--in", nxv, "--out", out], check=False, timeout=1800.0)
    if p.returncode != 0:
        tail = "\n".join((p.stderr or "").strip().splitlines()[-10:])
        raise SpatialError(f"nxv-dec failed (exit {p.returncode}):\n{tail}")


def bisect_qp(codec: CodecPaths, src: str, geo: Geometry, poses: str, fov: float,
              target_bits: float, work: str, extra: list[str] | None = None,
              qp_lo: int = 0, qp_hi: int = 51) -> tuple[int, int, str, list[dict]]:
    """Smallest QP whose stream fits *target_bits*.

    ``nxv-enc`` has no rate control -- it takes a QP -- so the harness supplies
    one, by bisection on the integer QP ladder.  The chosen point is the
    *highest quality* encode that fits the budget; if even QP 51 overshoots,
    QP 51 is returned and the overshoot is reported rather than hidden.
    """
    trials: list[dict] = []
    best: tuple[int, int, str] | None = None
    lo, hi = qp_lo, qp_hi
    while lo <= hi:
        mid = (lo + hi) // 2
        out = os.path.join(work, f"inset-qp{mid}.nxv")
        bits = encode_inset(codec, src, geo, mid, poses, fov, out, extra)
        trials.append({"qp": mid, "bits": bits})
        if bits <= target_bits:
            if best is None or mid < best[0]:
                if best is not None and os.path.exists(best[2]) and best[2] != out:
                    os.remove(best[2])
                best = (mid, bits, out)
            hi = mid - 1
        else:
            os.remove(out)
            lo = mid + 1
    if best is None:
        out = os.path.join(work, "inset-qp51.nxv")
        bits = encode_inset(codec, src, geo, qp_hi, poses, fov, out, extra)
        trials.append({"qp": qp_hi, "bits": bits, "overshoot": True})
        best = (qp_hi, bits, out)
    return best[0], best[1], best[2], trials


# --- the anchors ---------------------------------------------------------


def _nxq():
    """Import ``tools/quality`` lazily; it is the metric and anchor library."""
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    tq = os.path.join(root, "tools", "quality")
    if tq not in sys.path:
        sys.path.insert(0, tq)
    import foveated_metrics  # noqa: F401
    from nxq import ffmpeg as nxffmpeg, qpmap  # noqa: F401
    return tq


def bisect_crf(anchor_name: str, src: str, geo: Geometry, target_bits: float,
               work: str, fovea_spec: str | None,
               lo: float = 0.0, hi: float = 51.0,
               iters: int = 8) -> tuple[float, int, str, list[dict]]:
    """CRF that lands just under *target_bits* for a CRF-only anchor.

    ``x265-p-refresh`` carries its foveation as an ``addroi`` hint, which both
    x264 and x265 discard under constant QP; the harness therefore forces it
    onto CRF (tools/quality/README.md), and a CRF is not a bitrate.  This is
    the bisection that turns one into the other so the foveated anchor can be
    compared at a matched budget like everything else.
    """
    _nxq()
    from nxq import ffmpeg as nxffmpeg, qpmap
    from nxq.yuv import Format

    anchor = nxffmpeg.ANCHORS[anchor_name]
    fmt = Format(geo.width, geo.height, geo.pix_fmt)
    fovea = qpmap.FoveaMap.parse(fovea_spec) if anchor.foveated else None
    trials: list[dict] = []
    best: tuple[float, int, str] | None = None
    for _ in range(iters):
        mid = 0.5 * (lo + hi)
        out = os.path.join(work, f"anchor-crf{mid:.2f}.{anchor.raw_fmt}")
        enc = nxffmpeg.encode_anchor(anchor, src, fmt, out, crf=round(mid, 2),
                                     fps=geo.fps, nframes=geo.frames,
                                     fovea=fovea, layout=geo.layout)
        bits = 8 * enc.size
        trials.append({"crf": round(mid, 2), "bits": bits})
        if bits <= target_bits:
            if best is None or mid < best[0]:
                if best is not None and os.path.exists(best[2]):
                    os.remove(best[2])
                best = (mid, bits, out)
            else:
                os.remove(out)
            hi = mid
        else:
            os.remove(out)
            lo = mid
    if best is None:
        out = os.path.join(work, f"anchor-crf{hi:.2f}.{anchor.raw_fmt}")
        enc = nxffmpeg.encode_anchor(anchor, src, fmt, out, crf=round(hi, 2),
                                     fps=geo.fps, nframes=geo.frames,
                                     fovea=fovea, layout=geo.layout)
        best = (hi, 8 * enc.size, out)
        trials.append({"crf": round(hi, 2), "bits": best[1], "overshoot": True})
    dec = os.path.join(work, "anchor.dec.yuv")
    nxffmpeg.decode_bitstream(best[2], fmt, dec)
    return best[0], best[1], dec, trials


# --- metrics -------------------------------------------------------------


@dataclass
class Scores:
    psnr_y: float = 0.0
    fov_psnr_y: float = 0.0
    psnr_fovea: float = 0.0
    psnr_periphery: float = 0.0
    fov_ssim_y: float = 0.0
    jod: float | None = None
    jod_min: float | None = None
    ppd_center: float = 0.0


def score(ref: str, dis: str, geo: Geometry, *, hfov: float, fvvdp_runner=None,
          weighting: str = "acuity", ssim_stride: int = 3) -> Scores:
    """Eccentricity-weighted PSNR/SSIM (and FovVideoVDP, if a runner is given).

    Both metrics use a **centre fixation**: this is a fixed-foveation headset,
    the inset is centred on the lens axis, and the foveated anchor puts its
    low-QP box in the same place, so scoring anywhere else would grade one arm
    on a region no arm optimised.
    """
    _nxq()
    import foveated_metrics as fm

    eye = geo.eye_width
    ppd = fm.ppd_from_fov(eye, hfov)
    ecc = fm.eccentricity_map(geo.height, eye, ppd)
    w = fm.acuity_weights(ecc, weighting)
    inside = ecc <= fm.FOVEA_RADIUS_DEG

    acc = {k: 0.0 for k in ("wse", "wsum", "se", "n", "fse", "fn", "pse", "pn")}
    ssims: list[float] = []
    for i in range(geo.frames):
        ry = geo.read(ref, i)[0]
        dy = geo.read(dis, i)[0]
        for e in (0, 1):
            a = ry[:, e * eye:(e + 1) * eye].astype(np.float64)
            b = dy[:, e * eye:(e + 1) * eye].astype(np.float64)
            d2 = (a - b) ** 2
            acc["wse"] += float((w * d2).sum())
            acc["wsum"] += float(w.sum())
            acc["se"] += float(d2.sum())
            acc["n"] += d2.size
            acc["fse"] += float(d2[inside].sum())
            acc["fn"] += int(inside.sum())
            acc["pse"] += float(d2[~inside].sum())
            acc["pn"] += int((~inside).sum())
            if i % ssim_stride == 0:
                ssims.append(fm.foveated_ssim(a, b, w))

    def db(se: float, n: float) -> float:
        return fm._m.psnr_from_mse(se / max(n, 1e-9))

    s = Scores(
        psnr_y=db(acc["se"], acc["n"]),
        fov_psnr_y=db(acc["wse"], acc["wsum"]),
        psnr_fovea=db(acc["fse"], acc["fn"]),
        psnr_periphery=db(acc["pse"], acc["pn"]),
        fov_ssim_y=float(np.mean(ssims)),
        ppd_center=ppd,
    )
    if fvvdp_runner is not None:
        from nxq.yuv import Format
        r = fvvdp_runner.score(ref, dis, Format(geo.width, geo.height, geo.pix_fmt))
        s.jod = r["jod"]
        s.jod_min = r["jod_min"]
    return s


def make_fvvdp_runner(geo: Geometry, hfov: float, device: str | None = None):
    """A FovVideoVDP runner in the Pico 4 display model, or ``None``.

    The metric is optional: if ``pyfvvdp`` is not installed the sweep still
    produces every PSNR-shaped number and says the JOD column is unavailable,
    rather than inventing one.
    """
    _nxq()
    from nxq import fvvdp as fv

    ok, why = fv.available()
    if not ok:
        return None, why
    disp = fv.HeadsetDisplay("pico4", 2160, 2160, hfov, geo.fps, 100.0)
    sc = fv.FvvdpScoring(display=disp, layout=geo.layout, device=device)
    return fv.FvvdpRunner(sc, geo.fps), ""


# --- jobs ----------------------------------------------------------------


@dataclass
class SpatialJob:
    kind: str                 # "spatial" | "hevc" | "hevc-fov"
    total_mbit: float
    inset: int
    base_frac: float
    feather: int
    seq: str                  # sequence sidecar
    workroot: str
    codec_dir: str = DEFAULT_CODEC_DIR
    hfov: float = 95.0
    hole_dq: int = 0          # +QP inside the inset box on the periphery encode
    fovea_spec: str | None = None
    tag: str = ""

    @property
    def label(self) -> str:
        if self.kind == "hevc":
            return f"x265-{self.total_mbit:g}"
        if self.kind == "hevc-fov":
            return f"x265fov-{self.total_mbit:g}"
        s = f"sp-i{self.inset}-f{self.base_frac:g}-{self.total_mbit:g}"
        if self.feather != 32:
            s += f"-w{self.feather}"
        if self.hole_dq:
            s += f"-hole{self.hole_dq}"
        if self.tag:
            s += f"-{self.tag}"
        return s


def _hole_roi(geo: Geometry, inset: int, dq: int) -> str:
    """An ``addroi`` chain that raises QP inside the inset box of both eyes.

    The periphery does not have to code what the inset covers well.  Spending
    those bits elsewhere is free on the encoder side and costs nothing on the
    client -- until the inset stream is lost, which is exactly why this is a
    switch and not the default.
    """
    eye = geo.eye_width
    off = (eye - inset) // 2
    parts = [f"addroi=x={e * eye + off}:y={off}:w={inset}:h={inset}:qoffset={dq}/51"
             for e in (0, 1)]
    return ",".join(parts)


def produce(job: SpatialJob) -> dict:
    """Everything up to (but not including) the metrics: returns paths + rates."""
    t0 = time.time()
    src, geo, poses = load_sequence(job.seq)
    work = os.path.join(job.workroot, job.label)
    os.makedirs(work, exist_ok=True)
    total_bps = sim_bps(job.total_mbit, geo.width, geo.height)
    total_bits = total_bps * geo.frames / geo.fps

    out: dict = {"label": job.label, "kind": job.kind, "total_mbit": job.total_mbit,
                 "inset": job.inset, "base_frac": job.base_frac, "tag": job.tag,
                 "feather": job.feather, "hole_dq": job.hole_dq,
                 "size": [geo.width, geo.height], "frames": geo.frames,
                 "pix_fmt": geo.pix_fmt, "ref": src}

    if job.kind == "hevc":
        r = basemod.encode_decode_base(src, geo.width, geo.height, geo.frames,
                                       geo.width, geo.height, total_bps, work,
                                       geo.fps, tag="x265")
        out.update(bits=r.bitstream_bits, dis=r.path, encoder=r.encoder,
                   base_bits=r.bitstream_bits, inset_bits=0)
    elif job.kind == "hevc-fov":
        crf, bits, dec, trials = bisect_crf("x265-p-refresh", src, geo, total_bits,
                                            work, job.fovea_spec)
        out.update(bits=bits, dis=dec, crf=crf, crf_trials=trials,
                   base_bits=bits, inset_bits=0, encoder="libx265")
    else:
        codec = CodecPaths.at(job.codec_dir)
        # --- periphery: full resolution, ADR 0022 item 3 ---
        per_bps = total_bps * job.base_frac
        extra_vf = _hole_roi(geo, job.inset, job.hole_dq) if job.hole_dq else None
        per = _encode_periphery(src, geo, per_bps, work, extra_vf)
        # --- inset: the real codec ---
        crop = os.path.join(work, "inset.src.yuv")
        cgeo = extract_inset(src, geo, job.inset, crop)
        cposes = os.path.join(work, "inset.poses.json")
        fov = write_crop_poses(poses, geo.eye_width, job.hfov, job.inset, cposes)
        inset_target = max(0.0, total_bits - per.bitstream_bits)
        qp, ibits, nxv, trials = bisect_qp(codec, crop, cgeo, cposes, fov,
                                           inset_target, work)
        idec = os.path.join(work, "inset.dec.yuv")
        decode_inset(codec, nxv, idec)
        comp = os.path.join(work, "composite.yuv")
        composite(per.path, idec, geo, job.inset, job.feather, comp)
        out.update(bits=per.bitstream_bits + ibits, dis=comp,
                   base_bits=per.bitstream_bits, inset_bits=ibits,
                   inset_qp=qp, inset_fov_deg=fov, qp_trials=trials,
                   encoder=per.encoder,
                   inset_tiles=tiles_for(job.inset),
                   periphery_only=per.path)
        for p in (crop, idec):
            if os.path.exists(p):
                os.remove(p)

    out["measured_mbit"] = mbit_from_bits(out["bits"], geo.frames, geo.width,
                                          geo.height, geo.fps)
    out["base_mbit"] = mbit_from_bits(out["base_bits"], geo.frames, geo.width,
                                      geo.height, geo.fps)
    out["inset_mbit"] = mbit_from_bits(out["inset_bits"], geo.frames, geo.width,
                                       geo.height, geo.fps)
    out["produce_seconds"] = time.time() - t0
    return out


def _encode_periphery(src: str, geo: Geometry, bps: float, work: str,
                      extra_vf: str | None):
    """x265 over the whole frame, at *bps*, optionally with an inset QP hole."""
    if not extra_vf:
        return basemod.encode_decode_base(src, geo.width, geo.height, geo.frames,
                                          geo.width, geo.height, bps, work,
                                          geo.fps, tag="periphery")
    # The hole needs a filter chain, which encode_decode_base does not expose;
    # run the same command with one added.
    enc = basemod.pick_encoder()
    ext = "hevc" if enc == "libx265" else "h264"
    bs = os.path.join(work, f"periphery.{ext}")
    dec = os.path.join(work, "periphery.dec.yuv")
    kbit = max(8, int(round(bps / 1000.0)))
    th = cpu.threads()
    keyint = geo.frames * 100
    params = (basemod._x265_params(kbit, th, keyint) if enc == "libx265"
              else basemod._x264_params(kbit, th, keyint))
    pkey = "-x265-params" if enc == "libx265" else "-x264-params"
    cpu.run([basemod.ffmpeg_path(), "-hide_banner", "-loglevel", "error", "-y",
             "-threads", str(th), "-f", "rawvideo", "-pix_fmt", geo.pix_fmt,
             "-s", f"{geo.width}x{geo.height}", "-r", str(geo.fps), "-i", src,
             "-vf", extra_vf, "-c:v", enc, "-pix_fmt", geo.pix_fmt,
             "-tune", "zerolatency", "-preset", "veryfast", pkey, params,
             "-f", ext, bs])
    cpu.run([basemod.ffmpeg_path(), "-hide_banner", "-loglevel", "error", "-y",
             "-threads", str(th), "-i", bs, "-f", "rawvideo",
             "-pix_fmt", geo.pix_fmt, dec])
    return basemod.BaseResult(dec, geo.width, geo.height,
                              8 * os.path.getsize(bs), enc, geo.frames, geo.fps)


# --- decoder cost model --------------------------------------------------

#: bench/README.md: K5 (Pass A + Pass B, all 2048 tiles of a 2 x 2048^2 frame)
#: measured 28.0 ms p50 on the Pico 4 with the GPU pinned at 441.6 MHz.
K5_MS_FULL_FRAME = 28.0
K5_TILES = 2048
BENCH_CLOCK_MHZ = 441.6
PART_CLOCK_MHZ = 587.0
#: bench/README.md "One transform plane, no chroma": a real 4:2:0 decoder adds
#: 32 chroma blocks per tile, about +50% on the transform half of K5.
CHROMA_FACTOR = 1.5


def decode_ms(tiles: int, *, clock_mhz: float = BENCH_CLOCK_MHZ,
              chroma: bool = False) -> float:
    """Pass A + Pass B milliseconds for *tiles*, linear in the tile count."""
    per_tile = K5_MS_FULL_FRAME / K5_TILES * (BENCH_CLOCK_MHZ / clock_mhz)
    if chroma:
        per_tile *= CHROMA_FACTOR
    return per_tile * tiles


def inset_fitting(budget_ms: float, *, clock_mhz: float = BENCH_CLOCK_MHZ,
                  chroma: bool = False, eye_px: int = 2048) -> int:
    """Largest tile-aligned square inset per eye whose two copies fit *budget_ms*."""
    best = 0
    for n in range(1, eye_px // TILE + 1):
        if decode_ms(2 * n * n, clock_mhz=clock_mhz, chroma=chroma) <= budget_ms:
            best = n * TILE
    return best


# --- sweep driver --------------------------------------------------------


def build_jobs(seq: str, workroot: str, totals, insets, fracs, *, feather: int,
               codec_dir: str, hfov: float, fovea_spec: str | None,
               anchors: bool = True) -> list[SpatialJob]:
    jobs: list[SpatialJob] = []
    common = dict(seq=seq, workroot=workroot, codec_dir=codec_dir, hfov=hfov,
                  fovea_spec=fovea_spec)
    for t in totals:
        if anchors:
            jobs.append(SpatialJob("hevc", t, 0, 1.0, 0, **common))
            jobs.append(SpatialJob("hevc-fov", t, 0, 1.0, 0, **common))
        for i in insets:
            for f in fracs:
                jobs.append(SpatialJob("spatial", t, i, f, feather, **common))
    return jobs


def _produce_one(job: SpatialJob) -> dict:
    os.environ.setdefault("OMP_NUM_THREADS", "1")
    try:
        return produce(job)
    except Exception as exc:  # noqa: BLE001 - one bad point must not kill a sweep
        return {"label": job.label, "kind": job.kind, "total_mbit": job.total_mbit,
                "inset": job.inset, "base_frac": job.base_frac, "tag": job.tag,
                "error": repr(exc)}


def score_all(results: list[dict], hfov: float, *, fvvdp: bool = True,
              device: str | None = None, keep: bool = False,
              progress=None) -> dict:
    """Second stage: metrics, serially, so the GPU is used by one process.

    Composites are large (113 MB each at 2 x 1024^2 x 36) and there is no
    reason to keep them once they are scored, so each is deleted after its
    metrics unless *keep* is set.
    """
    runner, why = (None, "disabled")
    meta = {"fvvdp": None}
    first = next((r for r in results if "error" not in r), None)
    if first is None:
        return meta
    _, geo, _ = load_sequence_geometry(first)
    if fvvdp:
        runner, why = make_fvvdp_runner(geo, hfov, device)
        meta["fvvdp"] = runner.describe(geo.eye_width, geo.height) if runner else why
    for n, r in enumerate(results, 1):
        if "error" in r:
            continue
        s = score(r["ref"], r["dis"], geo, hfov=hfov, fvvdp_runner=runner)
        r.update(asdict(s))
        if progress:
            progress(n, len(results), r)
        if not keep:
            for k in ("dis", "periphery_only"):
                p = r.get(k)
                if p and os.path.exists(p) and "seq/" not in p:
                    os.remove(p)
                    r[k + "_removed"] = True
    return meta


def load_sequence_geometry(result: dict) -> tuple[str, Geometry, str]:
    w, h = result["size"]
    geo = Geometry(w, h, result["pix_fmt"], result["frames"], 90.0, "sbs")
    return result["ref"], geo, ""


# --- translating the simulator's inset to the headset ---------------------

#: docs/RATECONTROL.md 6.3 `pico4_eye()`: the render FOV WiVRn asks for at 1.0x
#: scale, +/-0.8568 tangent (+/-40.6 degrees) over 2160 px, ppd_center 22.0.
PICO4_PPD_CENTER = 22.0
PICO4_EYE_PX = 2160
#: PAPER.md 5.1.3: the fixed-foveation eye box, elliptical half-angles.
EYE_BOX_H_DEG = 20.0
EYE_BOX_V_DEG = 15.0
#: PAPER.md 5.1.4: the s=1 radius is 5 degrees of fovea plus the latency pad.
FOVEA_DEG = 5.0


def half_angle_of_inset(inset_px: int, eye_px: int, hfov_deg: float) -> float:
    """Half-angle subtended by a centred *inset_px* crop of an *eye_px* view."""
    return crop_fov_deg(eye_px, hfov_deg, inset_px) / 2.0


def device_inset_px(inset_px: int, eye_px: int, hfov_deg: float,
                    ppd_center: float = PICO4_PPD_CENTER, align: int = TILE) -> int:
    """The Pico 4 inset that subtends the same angle as a simulator inset.

    The simulator's eye is 1024 px over 95 degrees; the headset's is 2160 px
    over 81.2.  Those are different angular densities, so the translation
    between them is angular and not a pixel scale -- and it is the step where
    a spatial-hybrid result stops being about the test clip and starts being
    about the panel.
    """
    half = half_angle_of_inset(inset_px, eye_px, hfov_deg)
    px = 2.0 * half_angle_to_px(half, ppd_center)
    return int(math.ceil(min(px, eye_px if align == 1 else 2048) / align) * align)


def eye_box_inset(pad_deg: float = 3.0, ppd_center: float = PICO4_PPD_CENTER,
                  align: int = TILE) -> tuple[int, int]:
    """Inset (w, h) in device pixels covering the eye box plus a foveal radius.

    On a fixed-foveation headset the gaze is not measured, so the ``s = 1``
    region has to hold the fovea *wherever inside the eye box the eye happens
    to be*: the half-angle is the box half-angle plus the foveal radius, not
    the box half-angle alone.  ``pad_deg`` is PAPER.md 5.1.4's smooth-pursuit
    and tracker-error allowance (0.05 deg/ms of gaze-to-photon plus 1 degree),
    3 degrees at a 40 ms budget and 5.85 at the 57 ms worst case.
    """
    r = FOVEA_DEG + pad_deg
    w = 2.0 * half_angle_to_px(EYE_BOX_H_DEG + r, ppd_center)
    h = 2.0 * half_angle_to_px(EYE_BOX_V_DEG + r, ppd_center)
    up = lambda x: int(math.ceil(x / align) * align)  # noqa: E731
    return up(w), up(h)
