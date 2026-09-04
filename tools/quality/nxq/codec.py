"""Driving the NX Warp codec CLIs.

The contract, as agreed with the ``ref/tools`` side:

.. code-block:: text

    nxv-enc --in file.yuv --w W --h H --pix yuv444p|yuv420p --qp N --out out.nxv
    nxv-dec --in out.nxv --out out.yuv

``nxv-dec`` takes no geometry: the ``.nxv`` container carries it.

Because those binaries do not exist yet, the harness never hard-codes them.
:class:`CodecCLI` is built from ``--codec-cmd`` (a prefix to which ``-enc`` and
``-dec`` are appended) or from explicit ``--codec-enc`` / ``--codec-dec``
command lines, so the same harness drives the real codec, a build-tree binary,
or ``dummy_codec.py``.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
from dataclasses import dataclass

from . import cpu
from .yuv import Format


class CodecError(RuntimeError):
    pass


@dataclass
class CodecCLI:
    """A resolved encoder/decoder command pair."""

    enc: list[str]
    dec: list[str]
    name: str = "nxv"

    @classmethod
    def from_args(
        cls,
        codec_cmd: str | None = None,
        codec_enc: str | None = None,
        codec_dec: str | None = None,
        name: str | None = None,
    ) -> "CodecCLI":
        if codec_enc or codec_dec:
            if not (codec_enc and codec_dec):
                raise CodecError("--codec-enc and --codec-dec must be given together")
            return cls(shlex.split(codec_enc), shlex.split(codec_dec), name or "codec")
        prefix = codec_cmd or "nxv"
        parts = shlex.split(prefix)
        base = parts[-1]
        return cls(
            parts[:-1] + [f"{base}-enc"],
            parts[:-1] + [f"{base}-dec"],
            name or os.path.basename(base),
        )

    def available(self) -> tuple[bool, str]:
        for role, argv in (("encoder", self.enc), ("decoder", self.dec)):
            exe = argv[0]
            if not (shutil.which(exe) or os.path.isfile(exe)):
                return False, f"{role} {exe!r} not found on PATH"
        return True, ""

    def require(self) -> None:
        ok, why = self.available()
        if not ok:
            raise CodecError(
                f"{why}. The codec CLIs are built in ref/tools; until they exist, point the "
                "harness at the mock with "
                "--codec-enc 'python3 tools/quality/dummy_codec.py enc' "
                "--codec-dec 'python3 tools/quality/dummy_codec.py dec'"
            )

    # --- operations ------------------------------------------------------

    def encode(self, src_yuv: str, fmt: Format, qp: int, out_nxv: str, timeout: float = 900.0) -> int:
        """Encode and return the bitstream size in bytes."""
        cmd = [
            *self.enc,
            "--in", str(src_yuv),
            "--w", str(fmt.width),
            "--h", str(fmt.height),
            "--pix", fmt.pix_fmt,
            "--qp", str(qp),
            "--out", str(out_nxv),
        ]
        _check(cmd, "encode", timeout)
        if not os.path.exists(out_nxv):
            raise CodecError(f"encoder produced no output at {out_nxv}")
        return os.path.getsize(out_nxv)

    def decode(self, nxv: str, out_yuv: str, timeout: float = 900.0) -> None:
        cmd = [*self.dec, "--in", str(nxv), "--out", str(out_yuv)]
        _check(cmd, "decode", timeout)
        if not os.path.exists(out_yuv):
            raise CodecError(f"decoder produced no output at {out_yuv}")


def _check(cmd: list[str], what: str, timeout: float) -> None:
    try:
        p = cpu.run(cmd, check=False, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        raise CodecError(f"codec {what} timed out after {timeout:.0f}s: {' '.join(cmd)}") from exc
    except OSError as exc:
        raise CodecError(f"could not launch codec {what}: {exc}") from exc
    if p.returncode != 0:
        tail = "\n".join((p.stderr or "").strip().splitlines()[-15:])
        raise CodecError(f"codec {what} failed (exit {p.returncode}): {' '.join(cmd)}\n{tail}")
