"""FovVideoVDP through the reference implementation, in a headset display model.

BORROWED, UNMODIFIED, from branch `tourney/metric` (commit 25bc7a4, "quality:
the PAPER.md 5.3 perceptual metric set").  It is copied rather than
reimplemented so that the perceptual numbers in ref/RESULTS-percept.md are
produced by the same code the metric package will land; `nxq.yuv.yuv_to_rgb`
came with it.  If the two branches are merged this file is a duplicate and the
`tourney/metric` copy is the one to keep.


PAPER.md 5.3 names FovVideoVDP (Mantiuk, Denes, Chapiro, Kaplanyan, Rufo,
Bachy, Lian, Patney, *FovVideoVDP: A visible difference predictor for wide
field-of-view video*, ACM TOG 40(4), SIGGRAPH 2021) as the **primary objective
metric**, run in display space with gaze, display geometry, luminance and
temporal content, reporting a JOD score.

This module is the plumbing for that: it turns one of the harness's raw YUV
sequence pairs into the tensors the authors' `pyfvvdp` package wants, under a
display model for the headset rather than for a desk monitor, and returns JOD
per eye and pooled.  Nothing here re-implements the metric; if `pyfvvdp` is not
installed :func:`available` says so and the caller reports the metric as
unavailable rather than inventing a number.

Install it into the harness venv with::

    $NXQ_SCRATCH/venv/bin/pip install --index-url \\
        https://download.pytorch.org/whl/cpu torch
    $NXQ_SCRATCH/venv/bin/pip install pyfvvdp

CPU PyTorch is enough (about 0.6 s per 512x512 frame on four cores); a CUDA or
ROCm build is used automatically when `torch.cuda.is_available()`.

Three things about this measurement that must be said every time a number from
it is quoted
-------------------------------------------------------------------------------

1. **It is not yet the paper's display space.**  5.3 asks for the metric to be
   run on the output of the *client reprojection shader*, so that warped
   reference concealment is charged for what the eye finally sees.  This
   harness has no reprojection simulator, so what is compared here is the
   decoded frame against the source frame: display *geometry* and display
   *photometry*, but the codec's own output rather than the reprojected one.
   The gap is named in `tools/quality/README.md` and closing it is a change to
   `ref/`, not to this file.
2. **The ppd follows the sequence, not the panel.**  A Pico 4 is 2160 px per
   eye across about 100 degrees, so 15.8 pixels per degree on axis.  A 1024 px
   test clip across the same 100 degrees is 7.5 ppd, and that is the number
   this module uses, because it is what the pixels being compared actually
   subtend.  Scoring a 1024 px clip at the panel's 15.8 ppd would be scoring a
   picture nobody is looking at.  :meth:`HeadsetDisplay.geometry_json` reports
   both, and ``--fvvdp-ppd`` overrides.
3. **JOD is not a ratio scale of rate.**  BD-rate on JOD is computed with the
   same Bjontegaard integral as BD-rate on PSNR, which assumes only that the
   quality axis is monotone in rate; the *units* of a BD-JOD figure are JOD,
   and a JOD is calibrated so that 1 unit is a 75 percent preference in a
   pairwise comparison.
"""

from __future__ import annotations

import dataclasses
import json
import math

import numpy as np

from . import cpu
from .yuv import Format, read_sequence, yuv_to_rgb

#: Content brighter than this many nits is not reachable by these panels; the
#: Pico 4 is a ~100 nit LCD, which is also the luminance `nxrc::tvm` assumes
#: (docs/RATECONTROL.md 8.2 approximation 5), so the two models agree.
_PICO4_PEAK_NITS = 100.0


@dataclasses.dataclass(frozen=True)
class HeadsetDisplay:
    """A headset as FovVideoVDP needs to see it: geometry plus photometry.

    ``panel_width_px``/``panel_height_px`` are the real panel, per eye, and are
    used only for reporting: the geometry actually handed to the metric is
    built from the *sequence's* per-eye resolution at the same field of view.

    ``distance_m`` is the virtual-screen distance of the equivalent flat
    display.  Only the angular subtense matters to the metric, and the field of
    view fixes that, so this number cancels; it is the reference
    implementation's own VR convention (`fvvdp_display_geometry` defaults to
    3 m when a FOV is given) and is carried explicitly so the reported
    ``display_size_m`` means something.
    """

    name: str
    panel_width_px: int
    panel_height_px: int
    fov_horizontal_deg: float
    fps: float
    peak_nits: float
    contrast: float = 1000.0
    gamma: float = 2.2
    eotf: str = "sRGB"
    ambient_lux: float = 0.0
    reflectivity: float = 0.005
    distance_m: float = 3.0

    def ppd_center(self, width_px: int) -> float:
        """On-axis pixels per degree for *width_px* pixels across the FOV.

        This is the tan-projection centre density, the same quantity and the
        same formula as ``foveated_metrics.ppd_from_fov(..., mode="center")``
        and as the reference implementation's ``ppd_centre``; the three agree
        to the last digit and ``tests/test_fvvdp.py`` checks that they do.
        """
        f = (width_px / 2.0) / math.tan(math.radians(self.fov_horizontal_deg) / 2.0)
        return f * math.pi / 180.0

    def display_size_m(self, width_px: int, height_px: int) -> tuple[float, float]:
        w = 2.0 * math.tan(math.radians(self.fov_horizontal_deg) / 2.0) * self.distance_m
        return (w, w * height_px / width_px)

    def geometry_json(self, width_px: int, height_px: int) -> dict:
        sw, sh = self.display_size_m(width_px, height_px)
        return {
            "headset": self.name,
            "panel_px_per_eye": [self.panel_width_px, self.panel_height_px],
            "panel_ppd_center": self.ppd_center(self.panel_width_px),
            "scored_px_per_eye": [width_px, height_px],
            "scored_ppd_center": self.ppd_center(width_px),
            "fov_horizontal_deg": self.fov_horizontal_deg,
            "fps": self.fps,
            "viewing_distance_m": self.distance_m,
            "display_size_m": [sw, sh],
            "peak_nits": self.peak_nits,
            "contrast": self.contrast,
            "eotf": self.eotf,
            "gamma": self.gamma,
            "ambient_lux": self.ambient_lux,
        }

    def describe(self, width_px: int, height_px: int) -> str:
        g = self.geometry_json(width_px, height_px)
        return (
            f"{self.name}: panel {self.panel_width_px}x{self.panel_height_px} per eye "
            f"({g['panel_ppd_center']:.2f} ppd on axis), scoring "
            f"{width_px}x{height_px} at {g['scored_ppd_center']:.2f} ppd, "
            f"{self.fov_horizontal_deg:g} deg FOV, {self.fps:g} Hz, "
            f"{self.peak_nits:g} nit {self.eotf}, viewing distance "
            f"{self.distance_m:g} m ({g['display_size_m'][0]:.2f} m wide)"
        )

    # The pyfvvdp objects.  Built here rather than in the runner so that the
    # display model is one thing with one definition.
    def photometry(self):
        import pyfvvdp

        return pyfvvdp.fvvdp_display_photo_eotf(
            self.peak_nits, contrast=self.contrast, gamma=self.gamma, EOTF=self.eotf,
            E_ambient=self.ambient_lux, k_refl=self.reflectivity, name=self.name,
        )

    def geometry(self, width_px: int, height_px: int):
        import pyfvvdp

        return pyfvvdp.fvvdp_display_geometry(
            (width_px, height_px), distance_m=self.distance_m,
            fov_horizontal=self.fov_horizontal_deg,
        )


#: The headsets the harness knows.  `pico4` is the paper's target device
#: (docs/RATECONTROL.md 6.3 sizes the fixed foveation for it).
HEADSETS: dict[str, HeadsetDisplay] = {
    "pico4": HeadsetDisplay(
        name="pico4", panel_width_px=2160, panel_height_px=2160,
        fov_horizontal_deg=100.0, fps=90.0, peak_nits=_PICO4_PEAK_NITS,
    ),
    "quest3": HeadsetDisplay(
        name="quest3", panel_width_px=2064, panel_height_px=2208,
        fov_horizontal_deg=110.0, fps=90.0, peak_nits=100.0,
    ),
}
DEFAULT_HEADSET = "pico4"


def available() -> tuple[bool, str]:
    """Whether the reference implementation can be imported."""
    try:
        import torch  # noqa: F401
    except ImportError as exc:
        return False, f"PyTorch is not installed ({exc}); pyfvvdp needs it"
    try:
        import pyfvvdp  # noqa: F401
    except ImportError as exc:
        return False, (
            f"pyfvvdp is not installed ({exc}); "
            "pip install pyfvvdp into the harness venv"
        )
    return True, ""


def device_name(prefer: str | None = None) -> str:
    """``cuda`` when a GPU torch build sees a device, else ``cpu``."""
    import torch

    if prefer:
        return prefer
    try:
        if torch.cuda.is_available():
            return "cuda"
    except (RuntimeError, AssertionError):
        pass
    return "cpu"


# --- gaze ----------------------------------------------------------------


def read_gaze_log(path: str) -> list[tuple[float, float]]:
    """Per-frame fixation points in one view's pixels.

    Accepts ``[{"x":..,"y":..}, ...]`` or ``{"frames": [...]}``, the same shape
    ``foveated_metrics.py --gaze-log`` takes.
    """
    with open(path) as fh:
        doc = json.load(fh)
    rows = doc["frames"] if isinstance(doc, dict) else doc
    return [(float(r["x"]), float(r["y"])) for r in rows]


def gaze_segments(
    gaze: list[tuple[float, float]], n_frames: int, ppd: float, tol_deg: float
) -> list[tuple[int, int, tuple[float, float] | None]]:
    """Split a clip into contiguous runs of near-constant fixation.

    The reference implementation takes **one** fixation point per call, so a
    moving gaze has to be handled by segmentation.  A new segment is started
    whenever the gaze leaves a *tol_deg* disc around the segment's first
    sample; each segment is scored on its own and the JODs are averaged.

    The cost of this is real and is reported: FovVideoVDP's temporal filter is
    re-primed (replicate padding) at every segment boundary, so a clip cut into
    many segments is scored slightly optimistically on transients near the
    cuts.  With a centre fixation there is one segment and no cost at all.
    """
    if not gaze:
        return [(0, n_frames, None)]
    tol_px = tol_deg * ppd
    out: list[tuple[int, int, tuple[float, float] | None]] = []
    start = 0
    anchor = gaze[0]
    for i in range(1, min(n_frames, len(gaze))):
        dx, dy = gaze[i][0] - anchor[0], gaze[i][1] - anchor[1]
        if math.hypot(dx, dy) > tol_px:
            out.append((start, i, anchor))
            start, anchor = i, gaze[i]
    out.append((start, n_frames, anchor))
    return out


# --- the runner ----------------------------------------------------------


def _views(clip: np.ndarray, layout: str) -> list[tuple[str, np.ndarray]]:
    """Split a ``(F, H, W, 3)`` clip into eyes along the width axis.

    Same split as ``foveated_metrics._views``, one axis further in because
    these arrays carry the whole clip rather than one plane.
    """
    if layout == "sbs":
        half = clip.shape[2] // 2
        return [("left", clip[:, :, :half]), ("right", clip[:, :, half:])]
    return [("mono", clip)]


@dataclasses.dataclass
class FvvdpScoring:
    """Everything the FovVideoVDP score of one run depends on."""

    display: HeadsetDisplay
    layout: str = "mono"
    fixation: tuple[float, float] | None = None   # in one view's pixels
    gaze_log: str | None = None
    gaze_tol_deg: float = 2.0
    device: str | None = None
    ppd_override: float | None = None
    foveated: bool = True

    def to_json(self, width_px: int, height_px: int) -> dict:
        d = self.display.geometry_json(width_px, height_px)
        d.update({
            "layout": self.layout,
            "fixation": list(self.fixation) if self.fixation else "view centre",
            "gaze_log": self.gaze_log,
            "gaze_tol_deg": self.gaze_tol_deg,
            "foveated": self.foveated,
            "ppd_override": self.ppd_override,
            "display_space": "decoded frame vs source frame (no reprojection "
                             "simulator; PAPER.md 5.3 asks for the reprojected pair)",
        })
        return d


class FvvdpRunner:
    """Scores raw YUV sequence pairs with FovVideoVDP, one eye at a time.

    Constructed once per run so the metric object, its CSF cache and the
    Laplacian pyramid are built once and reused across every operating point,
    which is most of the wall time on CPU.
    """

    def __init__(self, scoring: FvvdpScoring, fps: float):
        ok, why = available()
        if not ok:
            raise RuntimeError(why)
        import torch

        import pyfvvdp

        self.scoring = scoring
        self.fps = fps
        self.device_name = device_name(scoring.device)
        self.torch = torch
        self.pyfvvdp = pyfvvdp
        self.device = torch.device(self.device_name)
        # The CPU discipline of nxq/cpu.py applies to the in-process work too.
        torch.set_num_threads(cpu.threads())
        self._metric = None
        self._metric_shape: tuple[int, int] | None = None
        self.version = getattr(pyfvvdp, "__version__", None) or _dist_version()

    def _metric_for(self, width: int, height: int):
        if self._metric is not None and self._metric_shape == (width, height):
            return self._metric
        disp = self.scoring.display
        geo = disp.geometry(width, height)
        if self.scoring.ppd_override:
            geo.ppd_centre = float(self.scoring.ppd_override)
        self._metric = self.pyfvvdp.fvvdp(
            display_photometry=disp.photometry(), display_geometry=geo,
            foveated=self.scoring.foveated, heatmap=None, quiet=True,
            device=self.device,
        )
        self._metric_shape = (width, height)
        return self._metric

    def ppd(self, view_width: int) -> float:
        if self.scoring.ppd_override:
            return float(self.scoring.ppd_override)
        return self.scoring.display.ppd_center(view_width)

    def describe(self, view_width: int, view_height: int) -> str:
        return (f"{self.scoring.display.describe(view_width, view_height)}; "
                f"pyfvvdp {self.version} on {self.device_name}, "
                f"{'foveated' if self.scoring.foveated else 'non-foveated'}")

    def score(
        self, ref_path: str, dis_path: str, fmt: Format, *, limit: int | None = None
    ) -> dict:
        """JOD per eye and pooled, for one decoded sequence.

        Pooling over the eyes is the **mean** JOD, and the worse eye is
        reported next to it as ``jod_min``: a JOD is an interval scale of
        perceived quality, so a mean is meaningful and a decibel-style
        MSE-domain combination is not.
        """
        ref = _load_rgb(ref_path, fmt, limit)
        dis = _load_rgb(dis_path, fmt, limit)
        if ref.shape != dis.shape:
            raise ValueError(f"shape mismatch {ref.shape} vs {dis.shape}")
        n_frames = ref.shape[0]
        gaze = read_gaze_log(self.scoring.gaze_log) if self.scoring.gaze_log else []

        out: dict = {"views": {}, "frames": n_frames}
        jods: list[float] = []
        for (vname, rv), (_, dv) in zip(_views(ref, self.scoring.layout),
                                        _views(dis, self.scoring.layout)):
            h, w = rv.shape[1:3]
            metric = self._metric_for(w, h)
            ppd = self.ppd(w)
            fixation = self.scoring.fixation
            segs = (gaze_segments(gaze, n_frames, ppd, self.scoring.gaze_tol_deg)
                    if gaze else [(0, n_frames, fixation)])
            seg_jods = []
            for lo, hi, fix in segs:
                fp = None
                if fix:
                    fp = np.asarray([float(fix[0]), float(fix[1])])
                q, _stats = metric.predict(
                    np.ascontiguousarray(dv[lo:hi]), np.ascontiguousarray(rv[lo:hi]),
                    dim_order="FHWC", frames_per_second=self.fps, fixation_point=fp,
                )
                seg_jods.append(float(q))
            jod = float(np.mean(seg_jods))
            out["views"][vname] = {"jod": jod, "segments": len(segs), "ppd_center": ppd}
            jods.append(jod)
        out["jod"] = float(np.mean(jods))
        out["jod_min"] = float(np.min(jods))
        return out


def _dist_version() -> str:
    try:
        from importlib import metadata

        return metadata.version("pyfvvdp")
    except Exception:  # pragma: no cover - only when the dist metadata is odd
        return "unknown"


def _load_rgb(path: str, fmt: Format, limit: int | None) -> np.ndarray:
    """Whole sequence as uint8 RGB, shape (F, H, W, 3).

    FovVideoVDP's temporal filter needs a 250 ms window, so the clip is held in
    memory rather than streamed: 36 frames of 2048x1024 is 226 MB, which is the
    size the harness already accepts for a decoded YUV file.
    """
    frames = [yuv_to_rgb(f) for f in read_sequence(path, fmt, limit)]
    if not frames:
        raise RuntimeError(f"{path}: no frames")
    return np.stack(frames, axis=0)


def scoring_from_args(args, seq_width: int, layout: str) -> FvvdpScoring:
    """Build an :class:`FvvdpScoring` from ``compare.py``'s parsed arguments."""
    disp = HEADSETS[args.fvvdp_headset]
    if args.fvvdp_fov:
        disp = dataclasses.replace(disp, fov_horizontal_deg=args.fvvdp_fov)
    if args.fvvdp_nits:
        disp = dataclasses.replace(disp, peak_nits=args.fvvdp_nits)
    fixation = None
    if args.fvvdp_fixation:
        fx, fy = (float(v) for v in args.fvvdp_fixation.split(","))
        fixation = (fx, fy)
    return FvvdpScoring(
        display=disp, layout=layout, fixation=fixation,
        gaze_log=args.fvvdp_gaze, gaze_tol_deg=args.fvvdp_gaze_tol,
        device=args.fvvdp_device, ppd_override=args.fvvdp_ppd,
        foveated=not args.fvvdp_no_fovea,
    )


def add_arguments(group) -> None:
    """The ``--fvvdp-*`` options, so compare.py and any other driver agree."""
    group.add_argument("--fvvdp-headset", default=DEFAULT_HEADSET,
                       choices=sorted(HEADSETS),
                       help="display model for the FovVideoVDP run (default: pico4)")
    group.add_argument("--fvvdp-fov", type=float, default=None,
                       help="override the headset's horizontal FOV in degrees")
    group.add_argument("--fvvdp-nits", type=float, default=None,
                       help="override the headset's peak luminance in cd/m^2")
    group.add_argument("--fvvdp-ppd", type=float, default=None,
                       help="override the computed on-axis pixels per degree")
    group.add_argument("--fvvdp-fixation", default=None, metavar="X,Y",
                       help="fixation point in one view's pixels (default: view centre)")
    group.add_argument("--fvvdp-gaze", default=None, metavar="FILE",
                       help="per-frame gaze log, JSON [{'x':..,'y':..}, ...]; the clip "
                            "is scored in runs of near-constant fixation")
    group.add_argument("--fvvdp-gaze-tol", type=float, default=2.0,
                       help="degrees of gaze motion that start a new segment (default 2)")
    group.add_argument("--fvvdp-device", default=None,
                       help="torch device (default: cuda when available, else cpu)")
    group.add_argument("--fvvdp-no-fovea", action="store_true",
                       help="run the non-foveated FovVideoVDP model")
