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

If libx264 or libx265 is missing, the probe says so and the harness skips that
anchor with a clear message rather than failing.
"""

from __future__ import annotations

import functools
import json
import os
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field

from . import cpu
from .yuv import Format


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
        enc = ", ".join(sorted(n for n in ("libx264", "libx265") if n in self.encoders)) or "none"
        return (
            f"ffmpeg: {self.version}\n"
            f"  anchor encoders: {enc}\n"
            f"  libvmaf filter : {'yes' if self.has_vmaf else 'no'}"
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


def _run_ffmpeg(args: list[str], what: str) -> str:
    caps = probe()
    if not caps.available:
        raise FFmpegError("ffmpeg is not installed")
    cmd = [caps.ffmpeg, "-hide_banner", "-nostdin", "-y", "-threads", str(cpu.threads()), *args]
    p = cpu.run(cmd, check=False)
    if p.returncode != 0:
        tail = "\n".join((p.stderr or "").strip().splitlines()[-15:])
        raise FFmpegError(f"{what} failed (exit {p.returncode}):\n{tail}")
    return p.stderr or ""


def _raw_input(path: str, fmt: Format, fps: float) -> list[str]:
    return [
        "-f", "rawvideo",
        "-pix_fmt", fmt.pix_fmt,
        "-s", f"{fmt.width}x{fmt.height}",
        "-r", str(fps),
        "-i", str(path),
    ]


# --- anchor definitions --------------------------------------------------


@dataclass(frozen=True)
class Anchor:
    """One anchor encoder configuration."""

    name: str
    encoder: str            # libx264 / libx265
    raw_fmt: str            # muxer for the elementary stream: h264 / hevc
    intra: bool
    preset: str = "medium"

    @property
    def param_flag(self) -> str:
        return "-x264-params" if self.encoder == "libx264" else "-x265-params"

    def params(self, nframes: int) -> str:
        """Encoder parameter string."""
        if self.encoder == "libx264":
            if self.intra:
                # Every frame an IDR: --keyint 1.
                return "keyint=1:min-keyint=1:scenecut=0:bframes=0:ref=1:rc-lookahead=0:sliced-threads=0"
            # P-only, single reference, one IDR at the start.
            big = max(nframes * 10, 1000)
            return (
                f"keyint={big}:min-keyint={big}:scenecut=0:bframes=0:ref=1:"
                "rc-lookahead=0:sliced-threads=0"
            )
        # libx265
        if self.intra:
            return "keyint=1:min-keyint=1:scenecut=0:bframes=0:ref=1:rc-lookahead=0:log-level=error"
        big = max(nframes * 10, 1000)
        return (
            f"keyint={big}:min-keyint={big}:scenecut=0:bframes=0:ref=1:b-adapt=0:"
            "rc-lookahead=0:frame-threads=1:log-level=error"
        )


ANCHORS: dict[str, Anchor] = {
    "x264-intra": Anchor("x264-intra", "libx264", "h264", intra=True),
    "x264-p": Anchor("x264-p", "libx264", "h264", intra=False),
    "x265-intra": Anchor("x265-intra", "libx265", "hevc", intra=True),
    "x265-p": Anchor("x265-p", "libx265", "hevc", intra=False),
}

#: The Phase 1 gate anchor, per PAPER.md 3.11.
PHASE1_ANCHOR = "x264-intra"
#: The Phase 2 BD-rate anchor, per PAPER.md 3.11 and 2.11 item 1.
PHASE2_ANCHOR = "x265-p"


def anchor_available(anchor: Anchor) -> tuple[bool, str]:
    caps = probe()
    if not caps.available:
        return False, "ffmpeg not installed"
    if not caps.has_encoder(anchor.encoder):
        return False, f"{anchor.encoder} not compiled into this ffmpeg"
    return True, ""


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
) -> int:
    """Encode *src_yuv* with an anchor at one operating point.

    Exactly one of *qp* or *crf* must be given.  Returns the bitstream size in
    bytes (an elementary stream, so there is no container overhead in the rate).
    """
    ok, why = anchor_available(anchor)
    if not ok:
        raise FFmpegError(f"anchor {anchor.name} unavailable: {why}")
    if (qp is None) == (crf is None):
        raise FFmpegError("give exactly one of qp= or crf=")
    if nframes is None:
        nframes = fmt.frame_count(src_yuv)

    rc = ["-qp", str(qp)] if qp is not None else ["-crf", str(crf)]
    args = [
        *_raw_input(src_yuv, fmt, fps),
        "-c:v", anchor.encoder,
        "-preset", anchor.preset,
        "-tune", "zerolatency",
        *rc,
        anchor.param_flag, anchor.params(nframes),
        "-f", anchor.raw_fmt,
        str(out_bitstream),
    ]
    _run_ffmpeg(args, f"{anchor.name} encode")
    return os.path.getsize(out_bitstream)


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
