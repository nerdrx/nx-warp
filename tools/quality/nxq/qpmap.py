"""Foveated quantization maps for the standard-codec anchors.

Why this module exists
----------------------
``VK_KHR_video_encode_quantization_map`` (Khronos, November 2024; see
docs/RESEARCH-INDUSTRY.md 2.2) lets an application hand a hardware encoder a
per-coding-block QP delta with every input picture.  RADV implements it.  That
means a competitor can build a *foveated* hardware HEVC streamer today without
a new codec, and NX Warp's foveation claim has to be measured against that, not
against a flat-QP HEVC encode.

Nothing on this machine can drive that extension end to end -- FFmpeg 9.0.1's
``hevc_vulkan``/``h264_vulkan`` wrappers expose no quantization-map option (see
:mod:`nxq.ffmpeg`) -- so the harness **emulates** the delta quantization map on
the software encoders instead, through FFmpeg's ``addroi`` filter.  ``addroi``
attaches ``AV_FRAME_DATA_REGIONS_OF_INTEREST`` side data, and both the libx264
and libx265 wrappers turn that into a per-macroblock / per-CTU quantizer offset
array.  That is the same *kind* of control the Vulkan extension provides, at the
same granularity, which is what makes it a fair anchor.

Read every number this produces as "a standard encoder given a foveated delta-QP
map", not as "a Vulkan quantization map".  The emulation differs from the real
extension in three ways worth stating:

* the map is a set of axis-aligned rectangles, not an arbitrary per-block image,
  so the eccentricity falloff is quantised into concentric bands;
* the offsets ride on the encoder's adaptive-quantization path, so they are a
  *hint* the rate controller may temper, not an absolute per-block QP; and
* consequently they only bite in CRF mode.  In constant-QP mode both x264 and
  x265 switch adaptive quantization off, the offsets are silently discarded, and
  the encode is bit-identical to the flat one.  :mod:`nxq.ffmpeg` therefore runs
  the foveated anchors on CRF and says so in the results JSON.

Geometry
--------
Concentric rectangles centred on each view, with a fixation at the view centre
(the same fixation :mod:`foveated_metrics` uses by default, so the anchor is
optimising the metric it is scored on):

.. code-block:: text

    +-------------------------------+  periphery   qp + periphery_delta
    |   +-----------------------+   |
    |   |      +---------+      |   |  mid         qp + mid_delta
    |   |      | centre  |      |   |
    |   |      +---------+      |   |  centre      qp + center_delta
    |   +-----------------------+   |
    +-------------------------------+

For a side-by-side stereo layout the pattern is emitted once per eye, so each
eye gets its own fovea rather than one shared box straddling the seam.

Ordering matters and is easy to get backwards: when regions overlap, the
*first* region in the side-data list wins (verified on this ffmpeg, not assumed).
:meth:`FoveaMap.regions` therefore emits the centre first and the full-frame
periphery last.
"""

from __future__ import annotations

from dataclasses import dataclass, replace

#: QP range the ``qoffset`` rational is scaled against.  FFmpeg's libx264 and
#: libx265 wrappers both multiply ``qoffset`` by 51, so ``-6/51`` is "QP minus 6".
QP_RANGE = 51


@dataclass(frozen=True)
class FoveaMap:
    """A concentric delta-QP map, in fractions of a view.

    *center_frac* and *mid_frac* are the side lengths of the centre and middle
    boxes as a fraction of the view's width and height.  The deltas are in QP
    steps and are added to the base quantizer.
    """

    center_frac: float = 0.25
    mid_frac: float = 0.55
    center_delta: float = -6.0
    mid_delta: float = 0.0
    periphery_delta: float = 6.0

    #: The default the anchors use: centre QP-6, mid QP, periphery QP+6.
    @classmethod
    def default(cls) -> "FoveaMap":
        return cls()

    @classmethod
    def parse(cls, spec: str | None) -> "FoveaMap":
        """Parse ``center=0.25:mid=0.55:dc=-6:dm=0:dp=6``.

        Every key is optional; unknown keys are an error rather than a silent
        typo.  ``None`` or an empty string gives the default map.
        """
        m = cls()
        if not spec:
            return m
        keys = {
            "center": "center_frac", "centre": "center_frac", "mid": "mid_frac",
            "dc": "center_delta", "dm": "mid_delta", "dp": "periphery_delta",
        }
        fields: dict[str, float] = {}
        for part in spec.replace(",", ":").split(":"):
            part = part.strip()
            if not part:
                continue
            if "=" not in part:
                raise ValueError(f"foveation map term {part!r} is not key=value")
            k, v = part.split("=", 1)
            k = k.strip().lower()
            if k not in keys:
                raise ValueError(
                    f"unknown foveation map key {k!r}; known keys are "
                    f"{sorted(set(keys))}"
                )
            try:
                fields[keys[k]] = float(v)
            except ValueError as exc:
                raise ValueError(f"foveation map {k}={v!r} is not a number") from exc
        m = replace(m, **fields)
        m.validate()
        return m

    def validate(self) -> None:
        if not 0.0 < self.center_frac <= 1.0:
            raise ValueError(f"center fraction {self.center_frac} must be in (0, 1]")
        if not 0.0 < self.mid_frac <= 1.0:
            raise ValueError(f"mid fraction {self.mid_frac} must be in (0, 1]")
        if self.center_frac > self.mid_frac:
            raise ValueError(
                f"centre box ({self.center_frac}) must not be larger than the mid box "
                f"({self.mid_frac})"
            )

    # --- geometry --------------------------------------------------------

    def views(self, width: int, height: int, layout: str = "mono") -> list[tuple[int, int]]:
        """``(x_offset, view_width)`` for each view in the frame."""
        if layout == "sbs":
            half = width // 2
            return [(0, half), (half, width - half)]
        return [(0, width)]

    def regions(
        self, width: int, height: int, layout: str = "mono"
    ) -> list[tuple[int, int, int, int, float]]:
        """``(x, y, w, h, delta)`` rectangles, most specific first.

        The centre boxes of every view come first, then the mid boxes, then one
        full-frame periphery rectangle, because the first matching region wins.
        """
        self.validate()
        views = self.views(width, height, layout)
        bands: list[list[tuple[int, int, int, int, float]]] = [[], []]
        for frac, delta, band in (
            (self.center_frac, self.center_delta, 0),
            (self.mid_frac, self.mid_delta, 1),
        ):
            for ox, vw in views:
                bw = max(1, int(round(vw * frac)))
                bh = max(1, int(round(height * frac)))
                bands[band].append(
                    (ox + (vw - bw) // 2, (height - bh) // 2, bw, bh, delta)
                )
        return bands[0] + bands[1] + [(0, 0, width, height, self.periphery_delta)]

    def addroi_chain(self, width: int, height: int, layout: str = "mono") -> str:
        """The ``addroi`` filter chain that installs this map.

        Regions with a zero delta are still emitted: they are what stops the
        periphery rectangle from swallowing the middle band.
        """
        parts = []
        for x, y, w, h, delta in self.regions(width, height, layout):
            num = int(round(delta))
            parts.append(f"addroi=x={x}:y={y}:w={w}:h={h}:qoffset={num}/{QP_RANGE}")
        return ",".join(parts)

    def describe(self) -> str:
        return (
            f"centre {self.center_frac:.0%} of the view at QP{self.center_delta:+g}, "
            f"mid {self.mid_frac:.0%} at QP{self.mid_delta:+g}, "
            f"periphery at QP{self.periphery_delta:+g}"
        )

    def to_json(self) -> dict:
        return {
            "kind": "concentric-rect delta-QP map (emulates "
                    "VK_KHR_video_encode_quantization_map)",
            "center_frac": self.center_frac,
            "mid_frac": self.mid_frac,
            "center_delta": self.center_delta,
            "mid_delta": self.mid_delta,
            "periphery_delta": self.periphery_delta,
            "qp_range": QP_RANGE,
            "mechanism": "ffmpeg addroi -> AV_FRAME_DATA_REGIONS_OF_INTEREST -> "
                         "libx264/libx265 per-block quantizer offsets (CRF only)",
        }
