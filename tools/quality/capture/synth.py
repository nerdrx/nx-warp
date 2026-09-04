"""Synthetic VR-like stereo test material.

This exists so that Phase 1 has test sequences before the codec, and before a
headset capture pipeline, exists.  It is not a substitute for real VR captures
(see the WiVRn section of the README) but it is deterministic, cheap, and it
deliberately contains the content classes that the paper says matter:

* **textured geometry** -- multi-octave value noise terrain, so the entropy
  coder has something real to chew on;
* **text panels** -- the 4:2:0 chroma-fringe torture case, and the content the
  paper locks to lossless (5.2, ``dQ_class``);
* **high-frequency detail** -- checkerboards and a zone plate, which alias
  under any downsampling and expose resampling blur (2.11 item 2);
* **dark regions** -- a low-luminance gradient where banding lives (5.2,
  ``dQ_lum = -2`` below 16/255);
* **moving objects** -- near-field discs with real stereo disparity and
  frame-to-frame motion, i.e. exactly the residual motion the pose warp cannot
  predict (2.3, and the "hands at 40 px parallax" risk in 2.11 item 5);
* **head-locked UI** -- a fixed HUD panel, the ``STATIC_MV`` case (2.11 item 6);
* **head rotation** -- views are rendered by sampling a large equirectangular
  panorama with a per-frame pose, so a rotation sequence is geometrically
  exactly what the codec's rotation-only reprojection (2.2) is meant to predict.

Everything is seeded; the same arguments produce byte-identical output on any
machine.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

from . import font5x7

# --- noise ---------------------------------------------------------------


def _bilinear_upsample(lat: np.ndarray, h: int, w: int) -> np.ndarray:
    """Upsample a small lattice to (h, w) with bilinear interpolation, wrapping in x."""
    lh, lw = lat.shape
    yi = (np.arange(h, dtype=np.float32) + 0.5) * lh / h - 0.5
    xi = (np.arange(w, dtype=np.float32) + 0.5) * lw / w - 0.5
    y0 = np.floor(yi).astype(np.int32)
    x0 = np.floor(xi).astype(np.int32)
    fy = (yi - y0)[:, None]
    fx = (xi - x0)[None, :]
    y0c = np.clip(y0, 0, lh - 1)
    y1c = np.clip(y0 + 1, 0, lh - 1)
    x0c = x0 % lw
    x1c = (x0 + 1) % lw
    a = lat[np.ix_(y0c, x0c)]
    b = lat[np.ix_(y0c, x1c)]
    c = lat[np.ix_(y1c, x0c)]
    d = lat[np.ix_(y1c, x1c)]
    top = a + (b - a) * fx
    bot = c + (d - c) * fx
    return top + (bot - top) * fy


def value_noise(h: int, w: int, octaves: int, rng: np.random.Generator, base: int = 4) -> np.ndarray:
    """Multi-octave value noise in [0, 1], wrapping horizontally."""
    out = np.zeros((h, w), np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        lh = max(2, base * (2**o))
        lw = max(2, base * (2**o) * 2)
        lat = rng.random((lh, lw), dtype=np.float32)
        out += amp * _bilinear_upsample(lat, h, w)
        total += amp
        amp *= 0.5
    return out / total


# --- panorama ------------------------------------------------------------

_PANEL_LINES = (
    "NX WARP QUALITY HARNESS",
    "TILE 64X64  RANS 8 LANES",
    "PHASE 1 GATE: PSNR VS X264 INTRA",
    "WITHIN 1.0 DB AT 100-400 MBIT",
    "0123456789 ()/%+=-*#",
    "THE QUICK BROWN FOX JUMPS OVER",
    "SMALL TEXT STRESSES 4:2:0 CHROMA",
)


def make_panorama(width: int = 4096, height: int = 2048, seed: int = 1,
                  feature_scale: float = 1.0) -> np.ndarray:
    """Build an equirectangular RGB panorama full of codec-hostile content.

    Returns an (height, width, 3) uint8 array.  ``width`` spans 360 degrees of
    longitude, ``height`` spans 180 degrees of latitude (top = +90).

    ``feature_scale`` multiplies the size, in panorama pixels, of the features
    that are defined in pixels rather than in degrees: the three checkerboard
    periods, the zone plate's chirp rate and the star size.  It exists because
    those are the *angular* content classes the paper cares about, and a
    panorama rendered at four times the resolution with the same pixel periods
    is not a finer picture of the same world, it is a different world with four
    times the angular frequency -- one that sits above the eye's Nyquist and can
    only ever be aliasing.  ``1.0`` reproduces version 1 exactly; the v2
    generator passes the panorama's oversampling ratio (panorama px/deg over
    the eye's on-axis px/deg, about 5.6), which sizes these features in **eye**
    pixels: checkerboard periods of 4, 8 and 16 output pixels, and a star about
    one output pixel across.

    The random draw order does not depend on ``feature_scale``, so the noise,
    the terrain and the star *positions* are the same for any value of it.
    """
    rng = np.random.default_rng(seed)
    fs = float(feature_scale)
    img = np.zeros((height, width, 3), np.float32)

    lat = (0.5 - (np.arange(height, dtype=np.float32) + 0.5) / height) * 180.0  # +90 .. -90
    lon_frac = (np.arange(width, dtype=np.float32) + 0.5) / width

    # 1. Sky: a smooth vertical gradient. Smooth gradients band at 8 bit; this
    #    is the content the paper's dither decision (5.2) exists for.
    t = np.clip((lat[:, None] + 10.0) / 100.0, 0.0, 1.0)  # (height, 1), broadcast over columns
    img[..., 0] = 20 + 45 * t
    img[..., 1] = 35 + 80 * t
    img[..., 2] = 60 + 150 * t

    # 2. Ground: textured terrain from value noise, below the horizon.
    ground = value_noise(height, width, 6, rng, base=6)
    fine = value_noise(height, width, 3, rng, base=64)
    terrain = np.clip(0.7 * ground + 0.3 * fine, 0, 1)
    # fade across the horizon so it is not a hard edge at every longitude
    below = np.clip((-lat[:, None]) / 6.0, 0.0, 1.0)
    gcol = np.stack(
        [40 + 150 * terrain, 55 + 130 * terrain**1.3, 35 + 90 * terrain**1.8], axis=-1
    )
    img = img * (1 - below[..., None]) + gcol * below[..., None]

    # 3. Star field high in the sky (isolated impulses: worst case for a
    #    transform codec, and they must not be smeared by the warp).
    star_px = max(1, int(round(fs)))
    stars = rng.random((height, width)) < 0.00035 / (fs * fs)
    stars &= lat[:, None] > 25.0
    if star_px == 1:
        img[stars] = 245.0
    else:
        # Keep a star about one eye-pixel across rather than one panorama
        # pixel, so it survives band-limiting as the isolated impulse it is
        # meant to be.  A max-dilation, so overlapping stars stay one star.
        sy, sx = np.nonzero(stars)
        for dy in range(star_px):
            yy = np.clip(sy + dy, 0, height - 1)
            for dx in range(star_px):
                img[yy, (sx + dx) % width] = 245.0

    def band(lat_lo, lat_hi, lon_lo, lon_hi):
        """Index slice for a lat/lon rectangle."""
        rows = np.where((lat >= lat_lo) & (lat < lat_hi))[0]
        cols = np.where((lon_frac >= lon_lo) & (lon_frac < lon_hi))[0]
        if rows.size == 0 or cols.size == 0:
            return None
        return np.ix_(rows, cols)

    # 4. Checkerboards at three frequencies, on the equator.
    for i, (lo, hi, period) in enumerate(
        ((0.02, 0.10, 4), (0.11, 0.19, 8), (0.20, 0.28, 16))
    ):
        idx = band(-14, 14, lo, hi)
        if idx is None:
            continue
        rr, cc = np.meshgrid(idx[0].ravel(), idx[1].ravel(), indexing="ij")
        period = max(1, int(round(period * fs)))
        chk = (((rr // period) + (cc // period)) % 2).astype(np.float32)
        img[idx] = np.stack([chk * 235 + 10] * 3, axis=-1)

    # 5. Zone plate: a radial chirp, the classic resolution/aliasing torture.
    idx = band(-16, 16, 0.30, 0.44)
    if idx is not None:
        rr, cc = np.meshgrid(idx[0].ravel(), idx[1].ravel(), indexing="ij")
        cy, cx = rr.mean(), cc.mean()
        r2 = ((rr - cy) ** 2 + (cc - cx) ** 2).astype(np.float32)
        z = 0.5 + 0.5 * np.cos(r2 * (0.0016 / (fs * fs)))
        img[idx] = np.stack([z * 230 + 12] * 3, axis=-1)

    # 6. Dark region: near-black with faint structure. Banding and the shadow
    #    protection rule (dQ_lum = -2 below 16/255) live here.
    idx = band(-18, 18, 0.46, 0.58)
    if idx is not None:
        rr, cc = np.meshgrid(idx[0].ravel(), idx[1].ravel(), indexing="ij")
        g = (cc - cc.min()) / max(1, cc.max() - cc.min())
        faint = value_noise(rr.shape[0], rr.shape[1], 4, rng, base=8)
        d = np.clip(g * 22.0 + faint * 6.0, 0, 255)
        img[idx] = np.stack([d, d * 1.05, d * 1.25], axis=-1)

    # 7. Saturated colour bars: chroma fidelity and 4:2:0 behaviour.
    idx = band(-16, 16, 0.60, 0.70)
    if idx is not None:
        rr, cc = np.meshgrid(idx[0].ravel(), idx[1].ravel(), indexing="ij")
        bars = np.array(
            [
                [235, 235, 235], [235, 235, 16], [16, 235, 235], [16, 235, 16],
                [235, 16, 235], [235, 16, 16], [16, 16, 235], [16, 16, 16],
            ],
            np.float32,
        )
        n = (((cc - cc.min()) * len(bars)) // max(1, cc.max() - cc.min() + 1)).astype(np.int32)
        img[idx] = bars[np.clip(n, 0, len(bars) - 1)]

    # 8. Text panels at two scales, on a dark UI background.
    idx = band(-20, 20, 0.72, 0.98)
    if idx is not None:
        rows, cols = idx[0].ravel(), idx[1].ravel()
        panel = np.zeros((rows.size, cols.size, 3), np.float32)
        panel[:] = (18, 20, 26)
        panel[2:-2, 2:-2] = (28, 32, 42)
        buf = np.zeros_like(panel, dtype=np.uint8)
        buf[:] = panel.astype(np.uint8)
        scale = max(1, rows.size // 90)
        y = 6
        for i, line in enumerate(_PANEL_LINES):
            s = scale * (2 if i == 0 else 1)
            font5x7.draw_text(buf, line, y, 8, (232, 236, 244), scale=s)
            y += font5x7.GLYPH_H * s + max(2, s * 2)
            if y > rows.size - font5x7.GLYPH_H:
                break
        img[idx] = buf.astype(np.float32)

    return np.clip(img, 0, 255).astype(np.uint8)


# --- poses ---------------------------------------------------------------


def _quat_from_ypr(yaw: float, pitch: float, roll: float) -> tuple[float, float, float, float]:
    """(x, y, z, w) quaternion for the intrinsic Y-X-Z rotation, radians."""
    cy, sy = math.cos(yaw * 0.5), math.sin(yaw * 0.5)
    cp, sp = math.cos(pitch * 0.5), math.sin(pitch * 0.5)
    cr, sr = math.cos(roll * 0.5), math.sin(roll * 0.5)
    # q = qy * qx * qz
    qy = (0.0, sy, 0.0, cy)
    qx = (sp, 0.0, 0.0, cp)
    qz = (0.0, 0.0, sr, cr)

    def mul(a, b):
        ax, ay, az, aw = a
        bx, by, bz, bw = b
        return (
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz,
        )

    return mul(mul(qy, qx), qz)


def rot_matrix(yaw: float, pitch: float, roll: float) -> np.ndarray:
    """World-from-head rotation, Y up, -Z forward, intrinsic Y-X-Z, radians."""
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cr, sr = math.cos(roll), math.sin(roll)
    ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], np.float64)
    rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]], np.float64)
    rz = np.array([[cr, -sr, 0], [sr, cr, 0], [0, 0, 1]], np.float64)
    return ry @ rx @ rz


MOTIONS = ("static", "pan", "turn", "mixed")


#: Peak yaw rate in deg/s for each profile. These are *rates*, not amplitudes,
#: so a 10-frame clip and a 600-frame clip exercise the same angular velocities
#: -- which is the property the codec is actually sensitive to. 150 deg/s is a
#: brisk but ordinary VR head turn; sustained comfortable panning is 30 deg/s.
PEAK_YAW_RATE = {"static": 0.0, "pan": 30.0, "turn": 150.0, "mixed": 120.0}


def make_poses(
    n: int, motion: str = "mixed", fps: float = 90.0, seed: int = 7, peak_rate: float | None = None
) -> list[dict]:
    """Generate a head-motion pose log of *n* frames.

    Profiles (all defined by angular *rate*, so they are clip-length
    independent):

    ``static``  a nearly still head (real users always drift a little)
    ``pan``     a steady 30 deg/s yaw, the "at rest but moving" case
    ``turn``    a raised-cosine velocity ramp peaking at 150 deg/s
    ``mixed``   rest, then a fast turn, then rest -- the profile the Phase 2
                kill test wants, because it contains both low and high angular
                velocity frames in one sequence (2.11 item 1)

    Each entry carries ``angular_velocity_deg_s``, which is what the kill test
    uses to select "the 20 percent of frames with the highest angular
    velocity".
    """
    if motion not in MOTIONS:
        raise ValueError(f"unknown motion {motion!r}, want one of {MOTIONS}")
    rng = np.random.default_rng(seed)
    dt = 1.0 / fps
    drift = np.cumsum(rng.normal(0, 0.06, size=(n, 3)), axis=0)  # gentle human sway, degrees

    peak = PEAK_YAW_RATE[motion] if peak_rate is None else float(peak_rate)
    t = np.arange(n) * dt
    total = max(dt, (n - 1) * dt)
    pitch = np.zeros(n)
    roll = np.zeros(n)

    # Build a yaw *rate* profile, then integrate it. u in [0,1] over the clip.
    u = np.clip(t / total, 0.0, 1.0)
    if motion == "static":
        rate = np.zeros(n)
    elif motion == "pan":
        rate = np.full(n, peak)
    elif motion == "turn":
        # raised cosine: 0 -> peak -> 0
        rate = peak * 0.5 * (1.0 - np.cos(2.0 * np.pi * u))
    else:  # mixed: rest / turn / rest
        rate = np.zeros(n)
        seg = (u >= 0.3) & (u <= 0.7)
        w = np.clip((u - 0.3) / 0.4, 0.0, 1.0)
        rate[seg] = peak * 0.5 * (1.0 - np.cos(2.0 * np.pi * w[seg]))
    yaw = np.concatenate([[0.0], np.cumsum(0.5 * (rate[1:] + rate[:-1]) * dt)])

    # Pitch rides along with the turn rather than being an independent
    # amplitude: tying it to the yaw excursion keeps its rate bounded too, so
    # short clips do not develop absurd angular velocities.
    if motion in ("turn", "mixed"):
        pitch = np.clip(0.12 * (yaw - yaw[0]), -25.0, 25.0)

    yaw = yaw + drift[:, 0]
    pitch = np.clip(pitch + drift[:, 1] * 0.5, -70, 70)
    roll = roll + drift[:, 2] * 0.3

    # Small positional motion: without it the near-field objects show no
    # translational parallax, which is the part rotation-only warp cannot fix.
    pos = np.stack(
        [0.03 * np.sin(2 * np.pi * 0.25 * t), 0.02 * np.sin(2 * np.pi * 0.4 * t), np.zeros(n)],
        axis=1,
    )

    poses = []
    for i in range(n):
        q = _quat_from_ypr(math.radians(yaw[i]), math.radians(pitch[i]), math.radians(roll[i]))
        if i == 0:
            av = 0.0
        else:
            prev = poses[-1]["orientation_xyzw"]
            dot = abs(sum(a * b for a, b in zip(prev, q)))
            av = math.degrees(2.0 * math.acos(min(1.0, dot))) / dt
        poses.append(
            {
                "frame": i,
                "time_s": float(t[i]),
                "position_xyz": [float(v) for v in pos[i]],
                "orientation_xyzw": [float(v) for v in q],
                "yaw_deg": float(yaw[i]),
                "pitch_deg": float(pitch[i]),
                "roll_deg": float(roll[i]),
                "angular_velocity_deg_s": float(av),
            }
        )
    return poses


# --- near-field objects --------------------------------------------------


@dataclass
class Objects:
    """Moving near-field discs: the residual-motion content."""

    count: int = 7
    seed: int = 11
    _p0: np.ndarray = field(init=False)
    _vel: np.ndarray = field(init=False)
    _rad: np.ndarray = field(init=False)
    _tex: list = field(init=False)

    def __post_init__(self) -> None:
        rng = np.random.default_rng(self.seed)
        # In front of the viewer, 0.4 m (hands) to 3 m (props).
        self._p0 = np.stack(
            [
                rng.uniform(-1.2, 1.2, self.count),
                rng.uniform(-0.7, 0.7, self.count),
                -rng.uniform(0.4, 3.0, self.count),
            ],
            axis=1,
        )
        self._vel = rng.uniform(-0.55, 0.55, (self.count, 3))
        self._vel[:, 2] *= 0.25
        self._rad = rng.uniform(0.05, 0.22, self.count)
        self._tex = []
        for i in range(self.count):
            n = 48
            tex = (value_noise(n, n, 4, np.random.default_rng(self.seed + i), base=4) * 255)
            chk = (((np.arange(n)[:, None] // 6) + (np.arange(n)[None, :] // 6)) % 2) * 60.0
            hue = np.array(
                [[[200, 120, 90], [110, 190, 220], [220, 200, 120]][i % 3]], np.float32
            ).reshape(1, 1, 3)
            self._tex.append(np.clip((tex + chk)[..., None] / 255.0 * hue * 1.3, 0, 255))

    def positions(self, t: float) -> np.ndarray:
        """World positions at time *t*, bouncing inside a box."""
        p = self._p0 + self._vel * t
        # triangle-wave fold keeps them in view without a discontinuity
        for ax, (lo, hi) in enumerate(((-1.5, 1.5), (-0.9, 0.9), (-3.2, -0.35))):
            span = hi - lo
            u = (p[:, ax] - lo) % (2 * span)
            p[:, ax] = lo + np.where(u > span, 2 * span - u, u)
        return p


# --- view rendering ------------------------------------------------------


@dataclass(frozen=True)
class Camera:
    width: int
    height: int
    hfov_deg: float = 95.0
    vfov_deg: float = 95.0
    ipd_m: float = 0.063


def _ray_grid(cam: Camera) -> np.ndarray:
    """Unit ray directions in head space for every pixel, (H, W, 3)."""
    tx = math.tan(math.radians(cam.hfov_deg) * 0.5)
    ty = math.tan(math.radians(cam.vfov_deg) * 0.5)
    xs = ((np.arange(cam.width, dtype=np.float32) + 0.5) / cam.width * 2.0 - 1.0) * tx
    ys = -((np.arange(cam.height, dtype=np.float32) + 0.5) / cam.height * 2.0 - 1.0) * ty
    d = np.empty((cam.height, cam.width, 3), np.float32)
    d[..., 0] = xs[None, :]
    d[..., 1] = ys[:, None]
    d[..., 2] = -1.0
    d /= np.linalg.norm(d, axis=-1, keepdims=True)
    return d


def sample_equirect(pano: np.ndarray, dirs: np.ndarray) -> np.ndarray:
    """Bilinear equirectangular lookup for unit direction vectors."""
    ph, pw = pano.shape[:2]
    lon = np.arctan2(dirs[..., 0], -dirs[..., 2])
    lat = np.arcsin(np.clip(dirs[..., 1], -1.0, 1.0))
    u = (lon / (2 * math.pi) + 0.5) * pw - 0.5
    v = (0.5 - lat / math.pi) * ph - 0.5
    u0 = np.floor(u).astype(np.int32)
    v0 = np.floor(v).astype(np.int32)
    fu = (u - u0).astype(np.float32)[..., None]
    fv = (v - v0).astype(np.float32)[..., None]
    u0m, u1m = u0 % pw, (u0 + 1) % pw
    v0c = np.clip(v0, 0, ph - 1)
    v1c = np.clip(v0 + 1, 0, ph - 1)
    p = pano.astype(np.float32)
    a = p[v0c, u0m]
    b = p[v0c, u1m]
    c = p[v1c, u0m]
    d = p[v1c, u1m]
    top = a + (b - a) * fu
    bot = c + (d - c) * fu
    return top + (bot - top) * fv


def _draw_objects(img: np.ndarray, cam: Camera, objs: Objects, R: np.ndarray, eye_pos: np.ndarray,
                  t: float) -> None:
    """Composite the near-field discs into an already-rendered background."""
    tx = math.tan(math.radians(cam.hfov_deg) * 0.5)
    ty = math.tan(math.radians(cam.vfov_deg) * 0.5)
    world = objs.positions(t)
    rel = (world - eye_pos[None, :]) @ R  # head-space = R^T (p - eye)
    order = np.argsort(rel[:, 2])  # far (most negative z) first
    for i in order:
        x, y, z = rel[i]
        if z > -0.15:
            continue
        depth = -z
        px = (x / depth / tx * 0.5 + 0.5) * cam.width
        py = (0.5 - y / depth / ty * 0.5) * cam.height
        rad = objs._rad[i] / depth / tx * 0.5 * cam.width
        if rad < 1.0:
            continue
        x0, x1 = int(px - rad), int(math.ceil(px + rad))
        y0, y1 = int(py - rad), int(math.ceil(py + rad))
        cx0, cy0 = max(0, x0), max(0, y0)
        cx1, cy1 = min(cam.width, x1), min(cam.height, y1)
        if cx1 <= cx0 or cy1 <= cy0:
            continue
        yy = (np.arange(cy0, cy1) - py)[:, None] / rad
        xx = (np.arange(cx0, cx1) - px)[None, :] / rad
        r2 = yy * yy + xx * xx
        mask = r2 <= 1.0
        if not mask.any():
            continue
        tex = objs._tex[i]
        tn = tex.shape[0]
        ti = np.clip(((yy * 0.5 + 0.5) * tn).astype(np.int32), 0, tn - 1)
        tj = np.clip(((xx * 0.5 + 0.5) * tn).astype(np.int32), 0, tn - 1)
        patch = tex[ti, tj]
        # cheap shading so the disc reads as a sphere
        shade = np.clip(1.15 - 0.55 * r2, 0.3, 1.2)[..., None]
        sub = img[cy0:cy1, cx0:cx1]
        sub[mask] = np.clip(patch * shade, 0, 255)[mask]


def _draw_hud(img: np.ndarray, frame: int, eye: str, av: float, ss: int = 1) -> None:
    """A head-locked UI panel: the STATIC_MV content class.

    ``ss`` is the supersampling factor of *img*: the panel's geometry is
    computed at the output resolution and then magnified by ``ss``, so a
    supersampled HUD is an exact ``ss``-times enlargement of the ``ss = 1``
    one and box-filters back down to the same layout, antialiased.
    """
    h, w = img.shape[0] // ss, img.shape[1] // ss
    ph = max(22, h // 12)
    pw = max(90, w // 3)
    y0, x0 = h - ph - max(4, h // 60), max(4, w // 60)
    img[y0 * ss : (y0 + ph) * ss, x0 * ss : (x0 + pw) * ss] = (14, 16, 22)
    img[(y0 + 1) * ss : (y0 + ph - 1) * ss, (x0 + 1) * ss : (x0 + pw - 1) * ss] = (26, 30, 40)
    s = max(1, ph // 18)
    font5x7.draw_text(img, f"FRAME {frame:04d} {eye}", (y0 + 3) * ss, (x0 + 4) * ss,
                      (230, 235, 245), scale=s * ss)
    font5x7.draw_text(img, f"AV {av:6.1f} DEG/S", (y0 + 5 + font5x7.GLYPH_H * s) * ss,
                      (x0 + 4) * ss, (150, 220, 160), scale=s * ss)


def render_view(
    pano: np.ndarray,
    cam: Camera,
    pose: dict,
    objs: Objects | None,
    eye: int,
    dirs: np.ndarray | None = None,
    hud: bool = True,
) -> np.ndarray:
    """Render one eye's RGB view for one pose."""
    if dirs is None:
        dirs = _ray_grid(cam)
    R = rot_matrix(
        math.radians(pose["yaw_deg"]), math.radians(pose["pitch_deg"]), math.radians(pose["roll_deg"])
    )
    world_dirs = dirs.reshape(-1, 3) @ R.T.astype(np.float32)
    img = sample_equirect(pano, world_dirs.reshape(dirs.shape))
    if objs is not None:
        head = np.asarray(pose["position_xyz"], np.float64)
        offset = R @ np.array([(-0.5 if eye == 0 else 0.5) * cam.ipd_m, 0.0, 0.0])
        _draw_objects(img, cam, objs, R, head + offset, pose["time_s"])
    out = np.clip(img, 0, 255).astype(np.uint8)
    if hud:
        _draw_hud(out, pose["frame"], "L" if eye == 0 else "R", pose["angular_velocity_deg_s"])
    return out


# --- band-limited (v2) rendering ----------------------------------------
#
# Version 1 took a single bilinear tap per output sample from a panorama at 2.1x
# the eye's angular resolution.  That is not a picture of the world, it is a
# point sample of it: the frames carry energy above the eye's Nyquist, that
# energy is not a geometric function of the pose, and no warp of any precision
# can predict it.  docs/WARP-AUDIT.md section 4 prices it at 7.2 dB full-frame
# and 14.4 dB centre on the ideal-warp ceiling -- more than any predictor change
# on offer is worth.  Everything below exists to remove it.
#
# Three things together band-limit the render:
#
#  1. a panorama at 16x the eye width (4.2x its angular resolution, the ratio
#     `nxvc-warpsim` uses), with the *angular* content of the 4096-wide one
#     (`feature_scale` above);
#  2. a latitude-aware longitudinal prefilter, because an equirectangular map's
#     longitudinal texel spacing shrinks as 1/cos(lat) and a view pitched 25
#     degrees up reaches latitude 72 at its corner, where the panorama is 3x
#     finer than anything the renderer samples it at;
#  3. 4x4 box supersampling of the render itself, which is what turns the
#     remaining detail into an average rather than a sample.


def equirect_ppd(width: int) -> float:
    """Panorama angular resolution at the equator, pixels per degree."""
    return width / 360.0


def eye_ppd(cam: Camera) -> float:
    """The eye's angular resolution **at the optical centre**, pixels/degree.

    A rectilinear projection's pixel density is lowest on axis and rises as
    1/cos^2, so the centre figure is the one to band-limit against: filtering to
    the average or the edge density would alias in the middle of the picture,
    which is where the fovea is.
    """
    return cam.width / (2.0 * math.tan(math.radians(cam.hfov_deg) * 0.5)) * math.pi / 180.0


def prefilter_equirect(pano: np.ndarray, target_ppd: float, max_width: int = 255) -> np.ndarray:
    """Low-pass an equirectangular panorama to *target_ppd* along longitude.

    Row *r* sits at latitude ``lat`` where one panorama pixel spans
    ``cos(lat) * 360 / width`` degrees of arc, so the row's angular resolution is
    ``(width / 360) / cos(lat)`` pixels per degree.  Where that exceeds the rate
    the renderer samples at, the row is averaged over a box of
    ``row_ppd / target_ppd`` pixels (rounded to an odd width, wrapping in
    longitude); where it does not, the row is left alone.

    Latitude needs no filtering: an equirectangular map's *rows* are uniformly
    spaced in angle, so the vertical rate is the same everywhere and the
    supersampled box downsample handles it.
    """
    ph, pw = pano.shape[:2]
    lat = (0.5 - (np.arange(ph, dtype=np.float64) + 0.5) / ph) * math.pi
    cos_lat = np.maximum(np.cos(lat), 1e-9)
    k = (equirect_ppd(pw) / cos_lat) / max(1e-9, target_ppd)
    widths = np.clip(2 * np.floor(k * 0.5).astype(np.int64) + 1, 1, max_width)
    out = pano.copy()
    for w in np.unique(widths):
        if w <= 1:
            continue
        rows = np.nonzero(widths == w)[0]
        r = int(w) // 2
        x = pano[rows].astype(np.float32)
        padded = np.concatenate([x[:, pw - r :], x, x[:, :r]], axis=1)
        cs = np.zeros((padded.shape[0], padded.shape[1] + 1, 3), np.float32)
        np.cumsum(padded, axis=1, out=cs[:, 1:])
        box = (cs[:, int(w) :] - cs[:, : -int(w)]) / float(w)
        out[rows] = np.clip(box + 0.5, 0, 255).astype(np.uint8)
    return out


def sample_equirect_u8(pano: np.ndarray, dirs: np.ndarray) -> np.ndarray:
    """:func:`sample_equirect` without converting the whole panorama to float.

    Identical arithmetic; it gathers uint8 and widens the four gathered corners
    instead of widening the source.  At a 16384x8192 panorama that is the
    difference between a 1.6 GB temporary per call and none.
    """
    ph, pw = pano.shape[:2]
    lon = np.arctan2(dirs[..., 0], -dirs[..., 2])
    lat = np.arcsin(np.clip(dirs[..., 1], -1.0, 1.0))
    u = (lon / (2 * math.pi) + 0.5) * pw - 0.5
    v = (0.5 - lat / math.pi) * ph - 0.5
    u0 = np.floor(u).astype(np.int32)
    v0 = np.floor(v).astype(np.int32)
    fu = (u - u0).astype(np.float32)[..., None]
    fv = (v - v0).astype(np.float32)[..., None]
    u0m, u1m = u0 % pw, (u0 + 1) % pw
    v0c = np.clip(v0, 0, ph - 1)
    v1c = np.clip(v0 + 1, 0, ph - 1)
    a = pano[v0c, u0m].astype(np.float32)
    b = pano[v0c, u1m].astype(np.float32)
    c = pano[v1c, u0m].astype(np.float32)
    d = pano[v1c, u1m].astype(np.float32)
    top = a + (b - a) * fu
    bot = c + (d - c) * fu
    return top + (bot - top) * fv


def box_downsample(img: np.ndarray, ss: int) -> np.ndarray:
    """Average ``ss x ss`` blocks: the reconstruction filter of the render."""
    h, w = img.shape[0] // ss, img.shape[1] // ss
    return img[: h * ss, : w * ss].reshape(h, ss, w, ss, -1).mean(axis=(1, 3))


def render_view_ss(
    pano: np.ndarray,
    cam: Camera,
    pose: dict,
    objs: Objects | None,
    eye: int,
    ss: int = 4,
    hud: bool = True,
    dirs_hi: np.ndarray | None = None,
    block: int = 512,
) -> np.ndarray:
    """Render one eye band-limited: ``ss x ss`` samples per pixel, box filtered.

    Near-field objects and the HUD are drawn at the supersampled resolution too,
    so their edges are antialiased rather than being the one hard-aliased thing
    left in an otherwise band-limited picture.
    """
    hi = Camera(cam.width * ss, cam.height * ss, cam.hfov_deg, cam.vfov_deg, cam.ipd_m)
    if dirs_hi is None:
        dirs_hi = _ray_grid(hi)
    R = rot_matrix(
        math.radians(pose["yaw_deg"]), math.radians(pose["pitch_deg"]), math.radians(pose["roll_deg"])
    )
    Rf = R.T.astype(np.float32)
    img = np.empty((hi.height, hi.width, 3), np.float32)
    for y0 in range(0, hi.height, block):
        y1 = min(hi.height, y0 + block)
        wd = dirs_hi[y0:y1].reshape(-1, 3) @ Rf
        img[y0:y1] = sample_equirect_u8(pano, wd.reshape(y1 - y0, hi.width, 3))
    if objs is not None:
        head = np.asarray(pose["position_xyz"], np.float64)
        offset = R @ np.array([(-0.5 if eye == 0 else 0.5) * cam.ipd_m, 0.0, 0.0])
        _draw_objects(img, hi, objs, R, head + offset, pose["time_s"])
    if hud:
        _draw_hud(img, pose["frame"], "L" if eye == 0 else "R",
                  pose["angular_velocity_deg_s"], ss=ss)
    out = box_downsample(img, ss)
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def render_stereo_ss(
    pano: np.ndarray,
    cam: Camera,
    pose: dict,
    objs: Objects | None,
    layout: str = "sbs",
    ss: int = 4,
    dirs_hi: np.ndarray | None = None,
    hud: bool = True,
) -> np.ndarray:
    """Band-limited :func:`render_stereo`."""
    if dirs_hi is None:
        dirs_hi = _ray_grid(Camera(cam.width * ss, cam.height * ss, cam.hfov_deg, cam.vfov_deg))
    left = render_view_ss(pano, cam, pose, objs, 0, ss, hud, dirs_hi)
    if layout == "mono":
        return left
    right = render_view_ss(pano, cam, pose, objs, 1, ss, hud, dirs_hi)
    return np.concatenate([left, right], axis=1)


# --- the ideal-warp ceiling ----------------------------------------------


def ideal_warp(prev: np.ndarray, pose_prev: dict, pose_cur: dict, cam: Camera) -> np.ndarray:
    """Warp *prev* by the exact float homography from ``pose_prev`` to ``pose_cur``.

    This is the ceiling any integer warp predictor can reach **on this
    material**: the same geometry the codec's `derive_homography()` quantises,
    evaluated per pixel in double precision with a bilinear resample, and
    written from this module's own projection rather than from the codec's, so
    that agreement between the two is evidence rather than a shared assumption.

    ``prev`` is a single-channel (H, W) image of one eye.  Samples that map
    outside the previous frame -- the disocclusion strip on the leading edge --
    clamp to the border, which is what the codec's warp does.
    """
    h, w = prev.shape
    tx = math.tan(math.radians(cam.hfov_deg) * 0.5)
    ty = math.tan(math.radians(cam.vfov_deg) * 0.5)
    xs = ((np.arange(w, dtype=np.float64) + 0.5) / w * 2.0 - 1.0) * tx
    ys = -((np.arange(h, dtype=np.float64) + 0.5) / h * 2.0 - 1.0) * ty
    d = np.empty((h, w, 3), np.float64)
    d[..., 0] = xs[None, :]
    d[..., 1] = ys[:, None]
    d[..., 2] = -1.0
    rp = rot_matrix(math.radians(pose_prev["yaw_deg"]), math.radians(pose_prev["pitch_deg"]),
                    math.radians(pose_prev["roll_deg"]))
    rc = rot_matrix(math.radians(pose_cur["yaw_deg"]), math.radians(pose_cur["pitch_deg"]),
                    math.radians(pose_cur["roll_deg"]))
    rel = rp.T @ rc  # previous-camera-from-current-camera
    p = d.reshape(-1, 3) @ rel.T
    z = np.minimum(p[:, 2], -1e-9)  # forward is -Z
    u = ((p[:, 0] / -z) / tx * 0.5 + 0.5) * w - 0.5
    v = (0.5 - (p[:, 1] / -z) / ty * 0.5) * h - 0.5
    u = np.clip(u.reshape(h, w), 0.0, w - 1.0)
    v = np.clip(v.reshape(h, w), 0.0, h - 1.0)
    u0 = np.floor(u).astype(np.int64)
    v0 = np.floor(v).astype(np.int64)
    u1 = np.minimum(u0 + 1, w - 1)
    v1 = np.minimum(v0 + 1, h - 1)
    fu, fv = u - u0, v - v0
    src = prev.astype(np.float64)
    top = src[v0, u0] + (src[v0, u1] - src[v0, u0]) * fu
    bot = src[v1, u0] + (src[v1, u1] - src[v1, u0]) * fu
    return top + (bot - top) * fv


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    d = np.asarray(a, np.float64) - np.asarray(b, np.float64)
    mse = float(np.mean(d * d))
    return 1000.0 if mse <= 0.0 else 10.0 * math.log10(255.0 * 255.0 / mse)


def centre_crop(a: np.ndarray, frac: float = 0.125) -> np.ndarray:
    """Drop a *frac* border on every side (the warpsim convention is 1/8)."""
    h, w = a.shape[:2]
    by, bx = int(h * frac), int(w * frac)
    return a[by : h - by, bx : w - bx]


def render_stereo(
    pano: np.ndarray,
    cam: Camera,
    pose: dict,
    objs: Objects | None,
    layout: str = "sbs",
    dirs: np.ndarray | None = None,
    hud: bool = True,
) -> np.ndarray:
    """Render a frame in the requested stereo layout ('sbs', 'mono')."""
    if dirs is None:
        dirs = _ray_grid(cam)
    left = render_view(pano, cam, pose, objs, 0, dirs, hud)
    if layout == "mono":
        return left
    right = render_view(pano, cam, pose, objs, 1, dirs, hud)
    return np.concatenate([left, right], axis=1)
