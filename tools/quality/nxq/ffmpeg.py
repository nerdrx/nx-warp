"""ffmpeg driving: capability probe, anchor encoders, and libvmaf.

Everything is launched through :mod:`nxq.cpu`, so ffmpeg runs on the harness's
core slice at idle priority with ``-threads`` capped.

The anchors are the ones the paper's Phase 1 and Phase 2 exit criteria name:

``x264-intra``
    ``--keyint 1 --tune zerolatency``: every frame an IDR.  This is the
    Phase 1 gate ("within 1.0 dB PSNR of x264 intra at 100 to 400 Mbit").

``x264-p`` / ``x265-p``
    ``--tune zerolatency``, no B-frames, a single reference, and one IDR at the
    start: the low-latency P-only configuration the Phase 2 BD-rate test
    compares against.

``x264-p-refresh`` / ``x265-p-refresh``
    The same P-only configuration but with **periodic intra refresh instead of
    IDRs** and a **foveated per-block delta-QP map**, which together are the
    software stand-in for ``VK_KHR_video_encode_intra_refresh`` and
    ``VK_KHR_video_encode_quantization_map`` (docs/RESEARCH-INDUSTRY.md 2.2).
    This is the honest opponent for NX Warp's foveation and IDR-free claims:
    a competitor can build exactly this on portable Vulkan today.

``hevc-vulkan`` / ``h264-vulkan``
    Real Vulkan video encode through the local driver, ultra-low-latency tuning.
    4:2:0 8-bit only, because Vulkan video H.264/H.265 has no 4:4:4 profile.

``av1-svt-p``
    SVT-AV1 in low-delay-P mode: the "what Virtual Desktop ships" reference.

If an encoder is missing, or is compiled in but will not open on the local
driver, the probe says so and the harness skips that anchor with a clear
message rather than failing.
"""

from __future__ import annotations

import functools
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field

from . import cpu, qpmap
from .yuv import Format


#: Every encoder any anchor can use, in report order.
ANCHOR_ENCODERS = ("libx264", "libx265", "hevc_vulkan", "h264_vulkan", "libsvtav1")


@dataclass
class Caps:
    """What this machine's ffmpeg can do."""

    ffmpeg: str | None = None
    version: str = ""
    encoders: set[str] = field(default_factory=set)
    filters: set[str] = field(default_factory=set)
    pix_fmts: set[str] = field(default_factory=set)

    @property
    def available(self) -> bool:
        return self.ffmpeg is not None

    def has_encoder(self, name: str) -> bool:
        return name in self.encoders

    @property
    def has_vmaf(self) -> bool:
        return "libvmaf" in self.filters

    def describe(self) -> str:
        if not self.available:
            return "ffmpeg: NOT FOUND (anchors and video import unavailable)"
        enc = ", ".join(n for n in ANCHOR_ENCODERS if n in self.encoders) or "none"
        missing = ", ".join(n for n in ANCHOR_ENCODERS if n not in self.encoders)
        return (
            f"ffmpeg: {self.version}\n"
            f"  anchor encoders: {enc}\n"
            + (f"  missing        : {missing}\n" if missing else "")
            + f"  libvmaf filter : {'yes' if self.has_vmaf else 'no'}"
        )


@functools.lru_cache(maxsize=1)
def probe() -> Caps:
    """Probe ffmpeg once per process."""
    exe = shutil.which("ffmpeg")
    if not exe:
        return Caps()
    caps = Caps(ffmpeg=exe)

    def _run(args: list[str]) -> str:
        try:
            p = subprocess.run(
                [exe, "-hide_banner", *args], capture_output=True, text=True, timeout=60
            )
            return p.stdout + p.stderr
        except (OSError, subprocess.SubprocessError):
            return ""

    ver = _run(["-version"]).splitlines()
    caps.version = ver[0].replace("ffmpeg version ", "").split(" Copyright")[0] if ver else "unknown"
    for line in _run(["-encoders"]).splitlines():
        m = re.match(r"^\s*[A-Z.]{6}\s+(\S+)", line)
        if m:
            caps.encoders.add(m.group(1))
    for line in _run(["-filters"]).splitlines():
        m = re.match(r"^\s*[A-Z.]{2,3}\s+(\S+)\s+\S+->\S+", line)
        if m:
            caps.filters.add(m.group(1))
    for line in _run(["-pix_fmts"]).splitlines():
        m = re.match(r"^\s*[A-Z.]{5}\s+(\S+)", line)
        if m:
            caps.pix_fmts.add(m.group(1))
    return caps


class FFmpegError(RuntimeError):
    pass


def _run_ffmpeg(args: list[str], what: str, want_cmd: bool = False):
    """Run ffmpeg under the CPU discipline.

    Returns ffmpeg's stderr, or the full argv when *want_cmd* is set, so the
    caller can record the exact command line it ran.
    """
    caps = probe()
    if not caps.available:
        raise FFmpegError("ffmpeg is not installed")
    cmd = [caps.ffmpeg, "-hide_banner", "-nostdin", "-y", "-threads", str(cpu.threads()), *args]
    p = cpu.run(cmd, check=False)
    if p.returncode != 0:
        tail = "\n".join((p.stderr or "").strip().splitlines()[-15:])
        raise FFmpegError(f"{what} failed (exit {p.returncode}):\n{tail}")
    return cpu.wrap(cmd) if want_cmd else (p.stderr or "")


def _raw_input(path: str, fmt: Format, fps: float) -> list[str]:
    return [
        "-f", "rawvideo",
        "-pix_fmt", fmt.pix_fmt,
        "-s", f"{fmt.width}x{fmt.height}",
        "-r", str(fps),
        "-i", str(path),
    ]


# --- anchor definitions --------------------------------------------------

#: Vulkan physical device index for the Vulkan-video anchors.
VULKAN_DEVICE = os.environ.get("NXQ_VULKAN_DEVICE", "0")
#: SVT-AV1 preset for the AV1 anchor (higher is faster; 10 is the low-latency tier).
SVTAV1_PRESET = int(os.environ.get("NXQ_SVTAV1_PRESET", "10"))


@dataclass(frozen=True)
class Anchor:
    """One anchor encoder configuration.

    ``backend`` selects how the command line is built:

    ``sw``
        libx264 / libx265 through their ``-x26?-params`` string.
    ``vulkan``
        ``hevc_vulkan`` / ``h264_vulkan``: Vulkan video encode on the local
        driver, via ``-init_hw_device vulkan`` and a ``hwupload``.  Constant-QP
        only, and 4:2:0 8-bit only -- Vulkan video H.264/H.265 has no 4:4:4
        profile any driver here exposes, which is itself a result.
    ``svtav1``
        ``libsvtav1`` in its low-delay-P configuration: the "what Virtual
        Desktop ships" reference point (docs/RESEARCH-INDUSTRY.md 1.6).
    """

    name: str
    encoder: str            # libx264 / libx265 / hevc_vulkan / h264_vulkan / libsvtav1
    raw_fmt: str            # muxer for the elementary stream: h264 / hevc / obu
    intra: bool
    preset: str = "medium"
    backend: str = "sw"
    #: Periodic intra refresh instead of IDR frames -- the software stand-in for
    #: ``VK_KHR_video_encode_intra_refresh``.
    intra_refresh: bool = False
    #: Refresh sweep length in frames; 0 means "one sweep across the clip".
    refresh_period: int = 0
    #: Drive a foveated delta-QP map through ``addroi`` (see :mod:`nxq.qpmap`).
    foveated: bool = False
    #: Rate-control modes this anchor can actually honour, best first.
    rc_modes: tuple[str, ...] = ("qp", "crf")
    #: Pixel formats the encoder accepts; empty means "whatever the source is".
    pix_fmts: tuple[str, ...] = ()
    note: str = ""

    @property
    def param_flag(self) -> str:
        return "-x264-params" if self.encoder == "libx264" else "-x265-params"

    def period(self, nframes: int) -> int:
        return self.refresh_period if self.refresh_period > 0 else max(1, nframes)

    def params(self, nframes: int) -> str:
        """Encoder parameter string (software backends only)."""
        common_264 = "scenecut=0:bframes=0:ref=1:rc-lookahead=0:sliced-threads=0"
        common_265 = "scenecut=0:bframes=0:ref=1:b-adapt=0:rc-lookahead=0:frame-threads=1:log-level=error"
        big = max(nframes * 10, 1000)

        if self.encoder == "libx264":
            if self.intra:
                # Every frame an IDR: --keyint 1.
                return f"keyint=1:min-keyint=1:{common_264}"
            if self.intra_refresh:
                # Periodic intra refresh instead of IDRs: after the first frame
                # there is never another IDR, the refresh column sweeps instead.
                k = self.period(nframes)
                return f"keyint={k}:min-keyint={k}:intra-refresh=1:{common_264}"
            # P-only, single reference, one IDR at the start.
            return f"keyint={big}:min-keyint={big}:{common_264}"

        # libx265
        if self.intra:
            return f"keyint=1:min-keyint=1:{common_265}"
        if self.intra_refresh:
            k = self.period(nframes)
            return f"keyint={k}:min-keyint={k}:intra-refresh=1:{common_265}"
        return f"keyint={big}:min-keyint={big}:{common_265}"


ANCHORS: dict[str, Anchor] = {
    # --- flat software anchors (the original set) ---
    "x264-intra": Anchor("x264-intra", "libx264", "h264", intra=True,
                         note="every frame an IDR; the Phase 1 gate anchor"),
    "x264-p": Anchor("x264-p", "libx264", "h264", intra=False,
                     note="P-only, single reference, one IDR at the start"),
    "x265-intra": Anchor("x265-intra", "libx265", "hevc", intra=True),
    "x265-p": Anchor("x265-p", "libx265", "hevc", intra=False,
                     note="P-only, single reference; the Phase 2 anchor"),

    # --- the honest hardware-class baselines (docs/RESEARCH-INDUSTRY.md 2.2) ---
    "x264-p-refresh": Anchor(
        "x264-p-refresh", "libx264", "h264", intra=False,
        intra_refresh=True, foveated=True, rc_modes=("crf",),
        note="P-only with periodic intra refresh (no IDRs after the first) and a "
             "foveated delta-QP map; emulates VK_KHR_video_encode_intra_refresh "
             "plus VK_KHR_video_encode_quantization_map on H.264",
    ),
    "x265-p-refresh": Anchor(
        "x265-p-refresh", "libx265", "hevc", intra=False,
        intra_refresh=True, foveated=True, rc_modes=("crf",),
        note="P-only with periodic intra refresh (no IDRs after the first) and a "
             "foveated delta-QP map; emulates VK_KHR_video_encode_intra_refresh "
             "plus VK_KHR_video_encode_quantization_map on H.265",
    ),

    # --- real Vulkan video encode through the local driver ---
    "hevc-vulkan": Anchor(
        "hevc-vulkan", "hevc_vulkan", "hevc", intra=False, backend="vulkan",
        rc_modes=("qp",), pix_fmts=("yuv420p",),
        note="Vulkan video H.265 encode, ultra-low-latency tuning, P-only",
    ),
    "h264-vulkan": Anchor(
        "h264-vulkan", "h264_vulkan", "h264", intra=False, backend="vulkan",
        rc_modes=("qp",), pix_fmts=("yuv420p",),
        note="Vulkan video H.264 encode, ultra-low-latency tuning, P-only",
    ),

    # --- what Virtual Desktop ships ---
    "av1-svt-p": Anchor(
        "av1-svt-p", "libsvtav1", "obu", intra=False, backend="svtav1",
        rc_modes=("crf",), pix_fmts=("yuv420p",),
        note="SVT-AV1 low-delay P (pred-struct=1, no lookahead, no temporal "
             "filtering); the Virtual Desktop / ALVR AV1 reference point",
    ),
}

#: The Phase 1 gate anchor, per PAPER.md 3.11.
PHASE1_ANCHOR = "x264-intra"
#: The Phase 2 BD-rate anchor, per PAPER.md 3.11 and 2.11 item 1.
PHASE2_ANCHOR = "x265-p"
#: The anchor the foveation and loss-recovery claims must actually beat.
HARDWARE_CLASS_ANCHOR = "x265-p-refresh"


@functools.lru_cache(maxsize=8)
def vulkan_ready(encoder: str, device: str = "") -> tuple[bool, str]:
    """Can this ffmpeg actually open *encoder* on the local Vulkan driver?

    The encoder being compiled in says nothing about the driver supporting
    video encode, so this runs a two-frame throwaway encode and reports what
    went wrong if it fails.  Cached, because it costs a process launch.
    """
    caps = probe()
    dev = device or VULKAN_DEVICE
    if not caps.available:
        return False, "ffmpeg not installed"
    if encoder not in caps.encoders:
        return False, f"{encoder} not compiled into this ffmpeg"
    cmd = [
        caps.ffmpeg, "-hide_banner", "-nostdin", "-y",
        "-init_hw_device", f"vulkan=vk:{dev}",
        "-f", "lavfi", "-i", "color=c=black:s=256x256:r=30",
        "-frames:v", "2", "-vf", "format=nv12,hwupload",
        "-c:v", encoder, "-qp", "30", "-f", encoder.split("_")[0], os.devnull,
    ]
    try:
        p = cpu.run(cmd, check=False, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        return False, f"{encoder} probe could not run: {exc}"
    if p.returncode == 0:
        return True, ""
    lines = [ln.strip() for ln in (p.stderr or "").strip().splitlines()]
    # The first complaint names the real cause; the last is usually ffmpeg's
    # generic "Conversion failed!", which explains nothing.
    hits = [ln for ln in lines
            if ("rror" in ln or "upport" in ln or "ailed" in ln)
            and not ln.startswith("Conversion failed")]
    why = hits[0] if hits else f"exit {p.returncode}"
    return False, (
        f"{encoder} is compiled in but would not open on Vulkan device {dev} "
        f"({why}); the driver most likely lacks video encode support"
    )


def anchor_available(anchor: Anchor, fmt: Format | None = None) -> tuple[bool, str]:
    """Whether *anchor* can run here, and on *fmt* if one is given."""
    caps = probe()
    if not caps.available:
        return False, "ffmpeg not installed"
    if not caps.has_encoder(anchor.encoder):
        return False, f"{anchor.encoder} not compiled into this ffmpeg"
    if anchor.backend == "vulkan":
        ok, why = vulkan_ready(anchor.encoder)
        if not ok:
            return False, why
    if fmt is not None and anchor.pix_fmts and fmt.pix_fmt not in anchor.pix_fmts:
        return False, (
            f"{anchor.encoder} accepts {', '.join(anchor.pix_fmts)} only, and this "
            f"sequence is {fmt.pix_fmt}"
            + (" -- Vulkan video H.264/H.265 has no 4:4:4 profile"
               if anchor.backend == "vulkan" else "")
        )
    return True, ""


def anchor_rc(anchor: Anchor, requested: str) -> str:
    """The rate-control mode *anchor* will actually be driven with.

    The harness asks for one mode for the whole run; an anchor that cannot
    honour it falls back to its own first supported mode and the caller records
    which was used.  The foveated anchors are CRF-only on purpose: x264 and x265
    both switch adaptive quantization off in constant-QP mode, and the per-block
    offsets ride on the AQ path, so a constant-QP foveated encode is silently
    identical to a flat one.
    """
    return requested if requested in anchor.rc_modes else anchor.rc_modes[0]


@dataclass(frozen=True)
class Encoded:
    """The outcome of one anchor encode."""

    size: int
    cmd: list[str]

    @property
    def cmdline(self) -> str:
        return " ".join(shlex.quote(a) for a in self.cmd)


def encode_anchor(
    anchor: Anchor,
    src_yuv: str,
    fmt: Format,
    out_bitstream: str,
    *,
    qp: int | None = None,
    crf: float | None = None,
    fps: float = 90.0,
    nframes: int | None = None,
    fovea: "qpmap.FoveaMap | None" = None,
    layout: str = "mono",
) -> Encoded:
    """Encode *src_yuv* with an anchor at one operating point.

    Exactly one of *qp* or *crf* must be given.  Returns the bitstream size in
    bytes (an elementary stream, so there is no container overhead in the rate)
    along with the exact argv, which the harness records in its results JSON.
    """
    ok, why = anchor_available(anchor, fmt)
    if not ok:
        raise FFmpegError(f"anchor {anchor.name} unavailable: {why}")
    if (qp is None) == (crf is None):
        raise FFmpegError("give exactly one of qp= or crf=")
    if nframes is None:
        nframes = fmt.frame_count(src_yuv)

    rc = ["-qp", str(qp)] if qp is not None else ["-crf", str(crf)]
    pre: list[str] = []
    vf: list[str] = []
    enc: list[str] = []

    if anchor.foveated:
        m = fovea or qpmap.FoveaMap.default()
        vf.append(m.addroi_chain(fmt.width, fmt.height, layout))

    if anchor.backend == "vulkan":
        pre += ["-init_hw_device", f"vulkan=vk:{VULKAN_DEVICE}"]
        vf.append("format=nv12,hwupload")
        big = max(nframes * 10, 1000)
        enc = [
            "-tune", "ull", "-usage", "stream", "-content", "rendered",
            "-async_depth", "1", "-rc_mode", "cqp",
            "-idr_interval", str(big), "-g", str(big), "-bf", "0",
            *rc,
        ]
    elif anchor.backend == "svtav1":
        big = max(nframes * 10, 1000)
        enc = [
            "-preset", str(SVTAV1_PRESET),
            *rc,
            "-g", str(big),
            "-svtav1-params",
            f"pred-struct=1:lookahead=0:enable-tf=0:keyint={big}:scd=0",
        ]
    else:
        enc = [
            "-preset", anchor.preset,
            "-tune", "zerolatency",
            *rc,
            anchor.param_flag, anchor.params(nframes),
        ]

    args = [
        *pre,
        *_raw_input(src_yuv, fmt, fps),
        *(["-vf", ",".join(vf)] if vf else []),
        "-c:v", anchor.encoder,
        *enc,
        "-f", anchor.raw_fmt,
        str(out_bitstream),
    ]
    cmd = _run_ffmpeg(args, f"{anchor.name} encode", want_cmd=True)
    return Encoded(os.path.getsize(out_bitstream), cmd)


def decode_bitstream(bitstream: str, fmt: Format, out_yuv: str) -> None:
    """Decode an elementary stream back to raw planar YUV."""
    _run_ffmpeg(
        ["-i", str(bitstream), "-pix_fmt", fmt.pix_fmt, "-f", "rawvideo", str(out_yuv)],
        "anchor decode",
    )


# --- VMAF ----------------------------------------------------------------


def vmaf(ref_yuv: str, dis_yuv: str, fmt: Format, fps: float = 90.0) -> float | None:
    """Mean VMAF of *dis* against *ref*, or ``None`` if libvmaf is unavailable.

    libvmaf's models are trained on 4:2:0; 4:4:4 input is converted to 4:2:0
    first so the score means what the model says it means.
    """
    caps = probe()
    if not caps.available or not caps.has_vmaf:
        return None
    with tempfile.TemporaryDirectory(prefix="nxq-vmaf-") as td:
        log = os.path.join(td, "vmaf.json")
        conv = "format=yuv420p"
        lavfi = (
            f"[0:v]{conv}[dis];[1:v]{conv}[ref];"
            f"[dis][ref]libvmaf=log_path={log}:log_fmt=json:n_threads={cpu.threads()}"
        )
        args = [
            *_raw_input(dis_yuv, fmt, fps),
            *_raw_input(ref_yuv, fmt, fps),
            "-lavfi", lavfi,
            "-f", "null", "-",
        ]
        try:
            _run_ffmpeg(args, "libvmaf")
        except FFmpegError:
            return None
        try:
            with open(log) as fh:
                doc = json.load(fh)
        except (OSError, json.JSONDecodeError):
            return None
    pooled = doc.get("pooled_metrics", {}).get("vmaf", {})
    if "mean" in pooled:
        return float(pooled["mean"])
    frames = doc.get("frames", [])
    vals = [f["metrics"]["vmaf"] for f in frames if "vmaf" in f.get("metrics", {})]
    return float(sum(vals) / len(vals)) if vals else None
