"""Sequence descriptors.

A raw YUV file carries no geometry, so every tool would otherwise need
``--w/--h/--pix`` repeated on every invocation.  Instead each generated or
imported sequence gets a small JSON sidecar next to it, ``<name>.json``:

.. code-block:: json

    {
      "name": "vr-mixed-512",
      "path": "vr-mixed-512.yuv444p.yuv",
      "width": 1024, "height": 512, "pix_fmt": "yuv444p",
      "fps": 90.0, "frames": 10,
      "pose_log": "vr-mixed-512.poses.json",
      "source": "synthetic:mixed"
    }

``path`` and ``pose_log`` are resolved relative to the sidecar's directory, so
a sequence directory can be moved or copied wholesale.
"""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass

from .yuv import Format


@dataclass
class Sequence:
    name: str
    path: str
    width: int
    height: int
    pix_fmt: str = "yuv444p"
    fps: float = 90.0
    frames: int = 0
    pose_log: str | None = None
    source: str = "unknown"
    layout: str = "mono"

    @property
    def fmt(self) -> Format:
        return Format(self.width, self.height, self.pix_fmt)

    def resolved_path(self) -> str:
        return self.path

    def check(self) -> None:
        """Validate that the file exists and its size matches the geometry."""
        if not os.path.exists(self.path):
            raise FileNotFoundError(f"sequence data missing: {self.path}")
        n = self.fmt.frame_count(self.path)
        if self.frames and n != self.frames:
            raise ValueError(f"{self.path}: sidecar says {self.frames} frames, file holds {n}")
        self.frames = n

    def save(self, sidecar: str) -> None:
        d = asdict(self)
        base = os.path.dirname(os.path.abspath(sidecar))
        for key in ("path", "pose_log"):
            if d[key]:
                d[key] = os.path.relpath(os.path.abspath(d[key]), base)
        os.makedirs(base, exist_ok=True)
        with open(sidecar, "w") as fh:
            json.dump(d, fh, indent=1)
            fh.write("\n")

    @classmethod
    def load(cls, sidecar: str) -> "Sequence":
        with open(sidecar) as fh:
            d = json.load(fh)
        base = os.path.dirname(os.path.abspath(sidecar))
        d["path"] = os.path.join(base, d["path"])
        if d.get("pose_log"):
            d["pose_log"] = os.path.join(base, d["pose_log"])
        known = {f for f in cls.__dataclass_fields__}
        seq = cls(**{k: v for k, v in d.items() if k in known})
        return seq

    @classmethod
    def open(cls, spec: str, width=None, height=None, pix_fmt=None, fps=None) -> "Sequence":
        """Open a sequence from a sidecar path, or from a raw path plus geometry."""
        if spec.endswith(".json"):
            seq = cls.load(spec)
        else:
            if not (width and height):
                raise ValueError(
                    f"{spec} is a raw YUV file with no sidecar; pass --w and --h "
                    "(or point at the sequence's .json descriptor)"
                )
            seq = cls(
                name=os.path.splitext(os.path.basename(spec))[0],
                path=spec,
                width=int(width),
                height=int(height),
                pix_fmt=pix_fmt or "yuv444p",
                fps=float(fps or 90.0),
                source="raw",
            )
        if fps:
            seq.fps = float(fps)
        seq.check()
        return seq
