"""The hardware-class base layer: HEVC through ffmpeg/libx265.

The base layer of the hybrid path is an ordinary HEVC stream that MediaCodec
can eat (PAPER.md 1.7, 3.5).  Here it is x265 configured to look as much like
a low-latency hardware encoder as software reasonably can:

* ``-tune zerolatency``, ``bframes=0``, ``rc-lookahead=0``, ``scenecut=0``
  -- P-only, no reordering, no lookahead, one IDR at frame 0
* CBR-ish ABR with a tight VBV (one frame of buffer) so the rate is actually
  spent evenly rather than hoarded, which is what a streaming encoder does
* ``pools=4`` and ``-threads 4`` for the CPU discipline

x265 at these settings is *better* than a Pico-4-class hardware encoder, not
worse, so every hybrid result here is conservative: a real base layer would
be a little softer at the same bitrate.

If libx265 is missing the module falls back to libx264 and marks the result,
so a run on a machine without x265 is still self-consistent and honestly
labelled.
"""

from __future__ import annotations

import functools
import os
import shutil
import subprocess
from dataclasses import dataclass

from . import cpu
from .yuv import YuvSequence


class FfmpegMissing(RuntimeError):
    pass


def ffmpeg_path() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise FfmpegMissing("ffmpeg is not on PATH")
    return p


@functools.lru_cache(maxsize=1)
def available_encoders() -> frozenset[str]:
    try:
        out = subprocess.run(
            [ffmpeg_path(), "-hide_banner", "-encoders"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout
    except (FfmpegMissing, subprocess.CalledProcessError):
        return frozenset()
    return frozenset(
        name for name in ("libx265", "libx264") if f" {name} " in out
    )


def pick_encoder(prefer: str = "libx265") -> str:
    enc = available_encoders()
    if prefer in enc:
        return prefer
    if "libx265" in enc:
        return "libx265"
    if "libx264" in enc:
        return "libx264"
    raise FfmpegMissing("neither libx265 nor libx264 is available in this ffmpeg")


@dataclass
class BaseResult:
    path: str  # decoded yuv420p at base resolution
    width: int
    height: int
    bitstream_bits: int  # actual coded size
    encoder: str
    frames: int
    fps: float

    @property
    def bitrate_bps(self) -> float:
        return self.bitstream_bits * self.fps / self.frames

    def sequence(self) -> YuvSequence:
        return YuvSequence(self.path, self.width, self.height)


def _x265_params(bitrate_kbit: int, threads: int, keyint: int) -> str:
    vbv = max(1, bitrate_kbit)
    return ":".join(
        [
            f"pools={threads}",
            "frame-threads=1",
            "bframes=0",
            "b-adapt=0",
            "rc-lookahead=0",
            "scenecut=0",
            "ref=1",
            "aq-mode=2",
            "no-open-gop=1",
            f"keyint={keyint}",
            f"min-keyint={keyint}",
            f"bitrate={bitrate_kbit}",
            f"vbv-maxrate={vbv}",
            f"vbv-bufsize={max(1, vbv // 45)}",  # ~2 frames at 90 Hz
            "strict-cbr=1",
        ]
    )


def _x264_params(bitrate_kbit: int, threads: int, keyint: int) -> str:
    vbv = max(1, bitrate_kbit)
    return ":".join(
        [
            f"threads={threads}",
            "bframes=0",
            "rc-lookahead=0",
            "scenecut=0",
            "ref=1",
            f"keyint={keyint}",
            f"min-keyint={keyint}",
            f"vbv-maxrate={vbv}",
            f"vbv-bufsize={max(1, vbv // 45)}",
            "nal-hrd=cbr",
        ]
    )


def encode_decode_base(
    src_yuv: str,
    src_w: int,
    src_h: int,
    frames: int,
    base_w: int,
    base_h: int,
    bitrate_bps: float,
    workdir: str,
    fps: float = 90.0,
    tag: str = "base",
    encoder: str | None = None,
) -> BaseResult:
    """Downscale, encode, and decode back.  Returns the decoded base sequence.

    The stream is written to disk so its true coded size is measured rather
    than estimated: ``bitstream_bits`` is ``8 * os.path.getsize(...)``.
    """
    enc = encoder or pick_encoder()
    os.makedirs(workdir, exist_ok=True)
    ext = "hevc" if enc == "libx265" else "h264"
    bs = os.path.join(workdir, f"{tag}.{ext}")
    dec = os.path.join(workdir, f"{tag}.dec.yuv")
    kbit = max(8, int(round(bitrate_bps / 1000.0)))
    th = cpu.threads()
    keyint = frames * 100  # single IDR at frame 0, as in a live stream

    vf = []
    if (base_w, base_h) != (src_w, src_h):
        # Lanczos down, matching what a server-side scaler would do.
        vf.append(f"scale={base_w}:{base_h}:flags=lanczos")
    params = _x265_params(kbit, th, keyint) if enc == "libx265" else _x264_params(kbit, th, keyint)
    pkey = "-x265-params" if enc == "libx265" else "-x264-params"

    cmd = [
        ffmpeg_path(), "-hide_banner", "-loglevel", "error", "-y",
        "-threads", str(th),
        "-f", "rawvideo", "-pix_fmt", "yuv420p",
        "-s", f"{src_w}x{src_h}", "-r", str(fps), "-i", src_yuv,
    ]
    if vf:
        cmd += ["-vf", ",".join(vf)]
    cmd += [
        "-c:v", enc, "-pix_fmt", "yuv420p",
        "-tune", "zerolatency", "-preset", "veryfast",
        pkey, params,
        "-f", ext, bs,
    ]
    cpu.run(cmd)

    cpu.run([
        ffmpeg_path(), "-hide_banner", "-loglevel", "error", "-y",
        "-threads", str(th), "-i", bs,
        "-f", "rawvideo", "-pix_fmt", "yuv420p", dec,
    ])

    bits = 8 * os.path.getsize(bs)
    got = os.path.getsize(dec) // (base_w * base_h * 3 // 2)
    if got != frames:
        raise RuntimeError(f"base decode produced {got} frames, expected {frames}")
    return BaseResult(dec, base_w, base_h, bits, enc, frames, fps)
