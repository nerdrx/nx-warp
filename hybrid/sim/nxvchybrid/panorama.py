"""Synthetic VR test material: a procedural panorama plus a head-pose log.

Why synthetic.  The hybrid experiment needs material whose inter-frame motion
is *exactly* the rotation-only homography of PAPER.md 2.2, otherwise a
measurement of "how often does the pose-warped hypothesis beat the upsampled
base" measures the renderer's shortcomings rather than the codec's.  A pinhole
camera panning inside a static equirectangular environment gives exactly that,
by construction, to within the sampling filter.

The panorama is deliberately mixed so the four tile classes of
:mod:`nxvchybrid.codec` all occur in quantity:

``flat``     sky gradient, large smooth areas
``texture``  multi-octave value noise at several scales
``edge``     hard-edged geometry: bars, rings, checkerboards
``text``     procedurally rendered 5x7 bitmap text at three sizes

On top of the background, a small number of screen-space *sprites* move
independently of the head.  They are the content the pose warp cannot predict,
and they are what makes the BASE hypothesis earn its place; without them the
experiment would be rigged in favour of the temporal hypothesis.

Everything is seeded, so the sequence is reproducible byte for byte.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from .yuv import Frame, rgb_to_ycbcr

# --- a 5x7 bitmap font ---------------------------------------------------
#
# Enough glyphs for the strings drawn below.  Each glyph is 7 rows of 5 bits.

_FONT = {
    "A": (0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11),
    "B": (0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E),
    "C": (0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E),
    "D": (0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E),
    "E": (0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F),
    "F": (0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10),
    "G": (0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F),
    "H": (0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11),
    "I": (0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E),
    "J": (0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C),
    "K": (0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11),
    "L": (0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F),
    "M": (0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11),
    "N": (0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11),
    "O": (0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E),
    "P": (0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10),
    "Q": (0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D),
    "R": (0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11),
    "S": (0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E),
    "T": (0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04),
    "U": (0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E),
    "V": (0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04),
    "W": (0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11),
    "X": (0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11),
    "Y": (0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04),
    "Z": (0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F),
    "0": (0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E),
    "1": (0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E),
    "2": (0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F),
    "3": (0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E),
    "4": (0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02),
    "5": (0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E),
    "6": (0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E),
    "7": (0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08),
    "8": (0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E),
    "9": (0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C),
    ".": (0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C),
    "-": (0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00),
    ":": (0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00),
    " ": (0, 0, 0, 0, 0, 0, 0),
}


def draw_text(dst: np.ndarray, x: int, y: int, text: str, scale: int, colour) -> None:
    """Stamp *text* into the HxWx3 array *dst* with the 5x7 font."""
    h, w = dst.shape[:2]
    col = np.asarray(colour, dtype=np.float32)
    for ci, ch in enumerate(text.upper()):
        glyph = _FONT.get(ch)
        if glyph is None:
            continue
        gx = x + ci * 6 * scale
        for row in range(7):
            bits = glyph[row]
            for bit in range(5):
                if not (bits >> (4 - bit)) & 1:
                    continue
                px = gx + bit * scale
                py = y + row * scale
                if 0 <= px < w - scale and 0 <= py < h - scale:
                    dst[py : py + scale, px : px + scale] = col


# --- value noise ---------------------------------------------------------


def _value_noise(rng: np.random.Generator, h: int, w: int, cells: int) -> np.ndarray:
    """Bilinearly interpolated value noise on a `cells`-wide lattice."""
    gy = max(2, cells // 2)
    lat = rng.random((gy + 1, cells + 1), dtype=np.float32)
    lat[:, -1] = lat[:, 0]  # wrap in longitude
    fy = np.linspace(0, gy, h, endpoint=False, dtype=np.float32)
    fx = np.linspace(0, cells, w, endpoint=False, dtype=np.float32)
    y0 = np.floor(fy).astype(np.int32)
    x0 = np.floor(fx).astype(np.int32)
    ty = (fy - y0)[:, None]
    tx = (fx - x0)[None, :]
    ty = ty * ty * (3 - 2 * ty)
    tx = tx * tx * (3 - 2 * tx)
    a = lat[y0][:, x0]
    b = lat[y0][:, x0 + 1]
    c = lat[y0 + 1][:, x0]
    d = lat[y0 + 1][:, x0 + 1]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def build_panorama(width: int = 4096, height: int = 2048, seed: int = 7) -> np.ndarray:
    """Return an equirectangular HxWx3 float32 RGB panorama in 0..255.

    The detail level is the single most important property of this function.
    An earlier version produced a mostly-flat scene (80% of tiles classified
    ``flat``, mean absolute gradient 1.4) on which x265 reached 48 dB at
    0.066 bpp -- an operating point where no layer-split question is even
    interesting.  Real VR content has roughly 1/f spatial statistics, so the
    background is now full-sphere fractional Brownian noise down to the pixel
    scale, and the geometry on top of it is textured rather than flat.
    """
    rng = np.random.default_rng(seed)
    h, w = height, width
    # Feature sizes are relative to the reference 4096-wide panorama so a small
    # panorama (the 256^2 self-test) keeps the same content mix rather than
    # degenerating into one giant checkerboard.
    u = w / 4096.0
    yy = np.linspace(0.0, 1.0, h, dtype=np.float32)[:, None]
    xx = np.linspace(0.0, 1.0, w, dtype=np.float32)[None, :]

    # 1. flat: a smooth sky/ground gradient, the low-frequency floor.
    img = np.empty((h, w, 3), dtype=np.float32)
    sky = np.clip(1.0 - yy * 1.35, 0.0, 1.0)
    img[..., 0] = 45 + 85 * sky + 30 * (1 - sky)
    img[..., 1] = 58 + 100 * sky + 25 * (1 - sky)
    img[..., 2] = 88 + 120 * sky + 12 * (1 - sky)

    # 2. texture: fBm down to the pixel scale, on all three channels so the
    #    chroma planes are not trivially codable either.
    octaves = [max(4, int(c * u)) for c in (16, 32, 64, 128, 256, 512, 1024, 2048)]
    # Falloff 0.75 per octave rather than a steeper 1/f-in-energy roll-off:
    # after the 2x supersample box filter, an octave at 4 panorama pixels per
    # cell contributes ~0.12 of gradient per unit amplitude and one at 32 px
    # contributes ~0.009, so a steep roll-off leaves the rendered frames with
    # almost no gradient at all.  These constants land the sequence at a mean
    # absolute luma gradient near 9 and roughly a quarter flat tiles.
    for ch, gain in enumerate((1.0, 0.86, 0.72)):
        acc = np.zeros((h, w), dtype=np.float32)
        amp = 1.0
        for cells in octaves:
            acc += amp * (_value_noise(rng, h, w, cells) - 0.5)
            amp *= 0.75
        img[..., ch] += acc * (55.0 * gain)

    # A high-frequency "detail wall": the region where the base layer's
    # resolution loss is most visible and the temporal hypothesis is worth most.
    y0, y1 = int(0.28 * h), int(0.64 * h)
    x0, x1 = int(0.02 * w), int(0.24 * w)
    fine = np.zeros((y1 - y0, x1 - x0), dtype=np.float32)
    amp = 1.0
    for cells in (max(8, int(240 * u)), max(16, int(700 * u)), max(24, int(1600 * u))):
        fine += amp * _value_noise(rng, y1 - y0, x1 - x0, cells)
        amp *= 0.7
    fine /= fine.max()
    img[y0:y1, x0:x1] = (40 + 195 * fine)[..., None] * np.array(
        [1.0, 0.94, 0.82], dtype=np.float32
    )

    # 3. edge: textured bars, rings and checkerboards at several scales.
    for _ in range(26):
        cy = int(rng.integers(int(0.12 * h), int(0.88 * h)))
        cx = int(rng.integers(0, w))
        bh = max(3, int(rng.integers(24, 150) * u))
        bw = max(4, int(rng.integers(36, 300) * u))
        col = rng.random(3, dtype=np.float32) * 190 + 35
        grain = _value_noise(rng, 2 * bh, 2 * bw, max(6, int(90 * u)))
        ys = np.arange(cy - bh, cy + bh)
        keep = (ys >= 0) & (ys < h)
        xs = np.arange(cx - bw, cx + bw) % w
        patch = col[None, None, :] * (0.65 + 0.7 * grain[..., None])
        img[ys[keep][:, None], xs[None, :], :] = patch[keep]

    for _ in range(9):
        cy = int(rng.integers(int(0.20 * h), int(0.80 * h)))
        cx = int(rng.integers(0, w))
        rad = 240 * u * float(rng.uniform(0.4, 1.2))
        r = np.hypot(
            (np.arange(h) - cy)[:, None].astype(np.float32),
            (((np.arange(w) - cx + w // 2) % w) - w // 2)[None, :].astype(np.float32),
        )
        ring = max(2, int(rng.integers(6, 22) * u))
        mask = (r < rad) & ((r.astype(np.int32) // ring) % 2 == 0)
        col = rng.random(3, dtype=np.float32) * 180 + 45
        img[mask] = col

    for k, (fy, fx, cell_ref) in enumerate(
        ((0.12, 0.55, 16), (0.66, 0.18, 8), (0.40, 0.78, 4))
    ):
        cy0, cx0 = int(fy * h), int(fx * w)
        ch_, cw_ = min(int(280 * u), h - cy0), min(int(520 * u), w - cx0)
        if ch_ < 4 or cw_ < 4:
            continue
        cell = max(2, int(cell_ref * u))
        ys, xs = np.mgrid[0:ch_, 0:cw_]
        chk = (((ys // cell) + (xs // cell)) % 2).astype(np.float32) * 205 + 28
        img[cy0 : cy0 + ch_, cx0 : cx0 + cw_] = chk[..., None]

    # 4. text: the class the paper cares most about, at three sizes.
    strings = [
        ("NX WARP HYBRID MODE", 4),
        ("BASE LAYER HEVC 0.5X", 3),
        ("ENHANCEMENT TILE 64X64", 2),
        ("POSE WARPED TEMPORAL HYPOTHESIS", 2),
        ("QUANTIZER STEP 24 QP 30", 3),
        ("PICO 4 ADRENO 650 90 HZ", 4),
        ("LCEVC RESIDUAL ON RESIDUAL", 2),
        ("TILE CLASS TEXT EDGE TEXTURE FLAT", 2),
        ("WEIGHTS 0 ONE-QUARTER ONE-HALF 1", 2),
        ("MEDIACODEC AHARDWAREBUFFER IMPORT", 2),
        ("RANS EIGHT LANES PER TILE", 3),
        ("FOVEATION LEVEL 0 1 2", 3),
    ]
    for i, (s, sc) in enumerate(strings):
        sc = max(1, int(round(sc * u)))
        ty = int(h * (0.14 + 0.061 * i))
        tx = int(w * ((0.17 + 0.137 * i) % 0.88))
        pw_ = len(s) * 6 * sc + 8
        ph_ = 7 * sc + 8
        if ty + ph_ < h and tx + pw_ < w:
            img[ty - 4 : ty + ph_, tx - 4 : tx + pw_] = 18.0
            draw_text(img, tx, ty, s, sc, (238, 238, 238))

    return np.clip(img, 0.0, 255.0)


# --- pose log ------------------------------------------------------------


def _rot(yaw: float, pitch: float, roll: float) -> np.ndarray:
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cr, sr = math.cos(roll), math.sin(roll)
    ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], dtype=np.float64)
    rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]], dtype=np.float64)
    rz = np.array([[cr, -sr, 0], [sr, cr, 0], [0, 0, 1]], dtype=np.float64)
    return ry @ rx @ rz


@dataclass
class Pose:
    yaw: float
    pitch: float
    roll: float

    def matrix(self) -> np.ndarray:
        return _rot(self.yaw, self.pitch, self.roll)


def pose_log(n: int, fps: float = 90.0) -> list[Pose]:
    """A head-motion profile covering the regimes PAPER.md 2.2 calls out.

    Four phases over the sequence: a slow drift, a fast sweep peaking at
    300 deg/s (the paper's worst case, ~70 px per frame at 2048 px / 95 deg),
    a deceleration with roll (which is where block-translation codecs lose),
    and a near-static settle with micro-jitter.
    """
    poses: list[Pose] = []
    yaw = pitch = roll = 0.0
    for i in range(n):
        t = i / fps
        frac = i / max(1, n - 1)
        if frac < 0.17:  # slow drift
            vyaw, vpitch, vroll = 20.0, 4.0, 0.0
        elif frac < 0.50:  # fast sweep, ramping to 300 deg/s
            r = (frac - 0.17) / 0.33
            vyaw = 40.0 + 260.0 * math.sin(math.pi * r)
            vpitch = 25.0 * math.sin(2 * math.pi * r)
            vroll = 0.0
        elif frac < 0.72:  # decelerate, with roll
            r = (frac - 0.50) / 0.22
            vyaw = 90.0 * (1 - r)
            vpitch = -18.0 * (1 - r)
            vroll = 35.0 * math.sin(math.pi * r)
        else:  # settle: micro-jitter only
            vyaw = 6.0 * math.sin(9.0 * t)
            vpitch = 4.0 * math.cos(7.0 * t)
            vroll = 2.0 * math.sin(5.0 * t)
        yaw += math.radians(vyaw) / fps
        pitch += math.radians(vpitch) / fps
        roll += math.radians(vroll) / fps
        poses.append(Pose(yaw, pitch, roll))
    return poses


def intrinsics(size: int, fov_deg: float = 95.0) -> np.ndarray:
    """Pinhole K for a square eye buffer of *size* px and horizontal *fov_deg*."""
    f = (size / 2.0) / math.tan(math.radians(fov_deg) / 2.0)
    c = (size - 1) / 2.0
    return np.array([[f, 0, c], [0, f, c], [0, 0, 1]], dtype=np.float64)


# --- sprites -------------------------------------------------------------


@dataclass
class Sprite:
    tex: np.ndarray  # hxwx3
    x0: float
    y0: float
    vx: float  # px per frame, screen space
    vy: float


def build_sprites(size: int, seed: int = 11) -> list[Sprite]:
    """Screen-space movers: the content the pose warp genuinely cannot predict."""
    rng = np.random.default_rng(seed)
    sprites: list[Sprite] = []
    # Total sprite area is ~3.3% of the frame: a HUD panel, a small fast
    # mover, and a slower textured object.  The exact figure matters: at 9%
    # (an earlier version) the movers alone cap frame PSNR near 23 dB and
    # every hypothesis-choice statistic collapses onto them, which measures
    # the test material rather than the codec.
    specs = [
        (int(size * 0.10), 3.1, 1.3, "text"),
        (int(size * 0.065), -4.4, 3.6, "edge"),
        (int(size * 0.13), 0.9, -0.7, "texture"),
    ]
    for s, vx, vy, kind in specs:
        s = max(32, s)
        tex = np.zeros((s, s, 3), dtype=np.float32)
        if kind == "text":
            tex[:] = 25.0
            draw_text(tex, 6, s // 3, "HUD 42", max(1, s // 48), (240, 200, 60))
            tex[:3] = tex[-3:] = tex[:, :3] = tex[:, -3:] = 240.0
        elif kind == "edge":
            ys, xs = np.mgrid[0:s, 0:s]
            tex[..., 0] = ((ys // 8 + xs // 8) % 2) * 200 + 30
            tex[..., 1] = tex[..., 0] * 0.4
            tex[..., 2] = 220 - tex[..., 0] * 0.5
        else:
            n = _value_noise(rng, s, s, 40)
            tex[..., 0] = 40 + 190 * n
            tex[..., 1] = 60 + 150 * (1 - n)
            tex[..., 2] = 100 + 120 * n
        sprites.append(
            Sprite(
                tex,
                float(rng.integers(0, size - s)),
                float(rng.integers(0, size - s)),
                vx * size / 1024.0,
                vy * size / 1024.0,
            )
        )
    return sprites


def _composite(dst: np.ndarray, sprites: list[Sprite], frame: int, size: int) -> None:
    for sp in sprites:
        s = sp.tex.shape[0]
        span = size - s
        if span <= 0:
            continue
        # triangle-wave bounce, so the motion is smooth and bounded
        def bounce(p0: float, v: float) -> int:
            p = p0 + v * frame
            period = 2.0 * span
            p = p % period
            return int(round(p if p <= span else period - p))

        y = bounce(sp.y0, sp.vy)
        x = bounce(sp.x0, sp.vx)
        dst[y : y + s, x : x + s] = sp.tex


# --- rendering -----------------------------------------------------------


def render_frame(
    pano: np.ndarray,
    pose: Pose,
    size: int,
    K_inv: np.ndarray,
    sprites: list[Sprite] | None = None,
    frame_index: int = 0,
    ss: int = 2,
) -> Frame:
    """Render one pinhole view of *pano* at *pose* into a yuv420 :class:`Frame`.

    *ss* is the supersampling factor: the view is rendered at ``size * ss`` and
    box-filtered down.  This matters more than it looks.  With ss = 1 the
    renderer point-samples the panorama, so each frame aliases with a different
    phase and consecutive frames are *not* related by the rotation homography
    at high spatial frequencies -- which would silently cripple the temporal
    hypothesis this whole experiment is measuring, and would do so in a way
    that looks like a codec result rather than a renderer artefact.  Real
    engine output is antialiased; so is this.
    """
    ph, pw = pano.shape[:2]
    big = size * ss
    xs = np.arange(big, dtype=np.float64)
    gx, gy = np.meshgrid(xs, xs)
    ones = np.ones_like(gx)
    if ss != 1:
        # sample at sub-pixel centres of the target grid
        gx = (gx + 0.5) / ss - 0.5
        gy = (gy + 0.5) / ss - 0.5
    rays = np.stack([gx, gy, ones], axis=-1) @ K_inv.T  # camera-space directions
    rays = rays @ pose.matrix().T  # world-space
    d = rays / np.linalg.norm(rays, axis=-1, keepdims=True)
    lat = np.arcsin(np.clip(d[..., 1], -1.0, 1.0))
    lon = np.arctan2(d[..., 0], d[..., 2])
    u = (lon / (2 * math.pi) + 0.5) * pw
    v = (lat / math.pi + 0.5) * ph

    u0 = np.floor(u).astype(np.int64)
    v0 = np.floor(v).astype(np.int64)
    fu = (u - u0)[..., None].astype(np.float32)
    fv = (v - v0)[..., None].astype(np.float32)
    u0m = u0 % pw
    u1m = (u0 + 1) % pw
    v0c = np.clip(v0, 0, ph - 1)
    v1c = np.clip(v0 + 1, 0, ph - 1)
    a = pano[v0c, u0m]
    b = pano[v0c, u1m]
    c = pano[v1c, u0m]
    e = pano[v1c, u1m]
    rgb = (a.astype(np.float32) * (1 - fu) + b.astype(np.float32) * fu) * (1 - fv) + (
        c.astype(np.float32) * (1 - fu) + e.astype(np.float32) * fu
    ) * fv

    if ss != 1:
        rgb = rgb.reshape(size, ss, size, ss, 3).mean(axis=(1, 3))

    if sprites:
        _composite(rgb, sprites, frame_index, size)

    ycc = rgb_to_ycbcr(np.clip(rgb, 0, 255))
    y = ycc[..., 0].astype(np.float32)
    # 2x2 box subsample for chroma (the standard 4:2:0 siting approximation)
    cb = ycc[..., 1].astype(np.float32).reshape(size // 2, 2, size // 2, 2).mean(axis=(1, 3))
    cr = ycc[..., 2].astype(np.float32).reshape(size // 2, 2, size // 2, 2).mean(axis=(1, 3))
    return Frame(y, cb, cr)


def render_sequence(
    size: int,
    frames: int,
    fov_deg: float = 95.0,
    pano_width: int = 4096,
    seed: int = 7,
    sprites: bool = True,
    ss: int = 2,
):
    """Yield (Frame, Pose) for a whole synthetic sequence."""
    # uint8 storage: an 8192x4096 panorama is 100 MB this way and 400 MB as
    # float32, and the renderer promotes only the four taps it needs.
    pano = build_panorama(pano_width, pano_width // 2, seed=seed).astype(np.uint8)
    poses = pose_log(frames)
    K_inv = np.linalg.inv(intrinsics(size, fov_deg))
    spr = build_sprites(size, seed=seed + 4) if sprites else None
    for i, p in enumerate(poses):
        yield render_frame(pano, p, size, K_inv, spr, i, ss), p
