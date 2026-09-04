"""Motion-to-photon, reported alongside every quality number.

PAPER.md 5.3, last decision:

    Latency is a quality metric: motion-to-photon measured with a photodiode on
    the panel and an IMU on the headset, reported alongside every quality
    number; a codec that gains 1 JOD by adding 8 ms has lost.

**This module does not measure motion to photon.**  A photodiode and an IMU are
not attached to this machine and there is no headset in the loop, so what
follows is the paper's own budget (4.2, "Latency floor, render-finish to
photons") with the one term this harness can actually observe — the codec's own
encode and decode time — substituted for the budget's assumption.  Every
result carries ``measured: false`` on the terms that are budget, and names what
would have to exist to measure them.

The point of reporting it anyway is the sentence above: a rate-distortion table
without a latency column invites exactly the trade the paper forbids.  A
codec's JOD gain has to be read next to what it costs in milliseconds, even
when the milliseconds are a model.

The budget, from PAPER.md 4.2
-----------------------------

    pose uplink            1 to 2 ms
    render                 up to 11 ms (one frame period at 90 Hz)
    encode + air + decode  6.8 ms on WiFi 6, 4.8 ms on USB, tile-row pipelined
    compositor phase wait  0 to 11.1 ms, 5.5 ms average at 90 Hz
    reprojection + scanout about 5 ms on the Pico 4 LCD
    -----------------------------------------------------
    motion to photons      25 to 35 ms floor, against about 100 ms today
"""

from __future__ import annotations

import dataclasses


@dataclasses.dataclass(frozen=True)
class LatencyBudget:
    """PAPER.md 4.2's terms.  All milliseconds."""

    pose_uplink_ms: float = 1.5          # "1 to 2 ms"
    render_ms: float = 11.0              # "up to 11 ms"
    pipeline_ms: float = 6.8             # encode + air + decode, WiFi 6, pipelined
    phase_wait_ms: float = 5.5           # "0 to 11.1, average 5.5" at 90 Hz
    reproject_scanout_ms: float = 5.0    # "about 5 ms on the Pico 4 LCD"
    link: str = "wifi6"

    @classmethod
    def usb(cls) -> "LatencyBudget":
        return cls(pipeline_ms=4.8, link="usb")

    def total_ms(self) -> float:
        return (self.pose_uplink_ms + self.render_ms + self.pipeline_ms
                + self.phase_wait_ms + self.reproject_scanout_ms)


def motion_to_photon(
    budget: LatencyBudget, *, encode_ms_per_frame: float | None = None,
    decode_ms_per_frame: float | None = None, fps: float = 90.0,
) -> dict:
    """The budget, plus what this run actually timed.

    ``encode_ms_per_frame``/``decode_ms_per_frame`` are the harness's wall-clock
    measurements of the **reference** implementation, which is single-threaded
    C++ on a CPU.  They are reported next to the budget and are deliberately
    *not* substituted into it: PAPER.md 4.2's 6.8 ms assumes the GPU encoder
    (3.0 ms on a 7900 XTX) and the GPU decoder (4.0 ms on an XR2), and swapping
    a CPU reference encoder into that slot would produce a number that means
    nothing.  The ratio is the useful part, and it is given as
    ``reference_over_budget``.
    """
    out: dict = {
        "budget_ms": dataclasses.asdict(budget),
        "budget_total_ms": budget.total_ms(),
        "frame_period_ms": 1000.0 / fps if fps > 0 else None,
        "measured": False,
        "source": "PAPER.md 4.2 budget; no photodiode and no IMU in this harness",
        "what_would_measure_it": (
            "a photodiode on the panel and an IMU on the headset, per PAPER.md 5.3; "
            "the harness can only time the codec"
        ),
    }
    if encode_ms_per_frame is not None:
        out["reference_encode_ms_per_frame"] = encode_ms_per_frame
    if decode_ms_per_frame is not None:
        out["reference_decode_ms_per_frame"] = decode_ms_per_frame
    if encode_ms_per_frame is not None and decode_ms_per_frame is not None:
        ref_total = encode_ms_per_frame + decode_ms_per_frame
        out["reference_codec_ms_per_frame"] = ref_total
        out["reference_over_budget"] = ref_total / budget.pipeline_ms
        out["note"] = (
            "the reference codec is a CPU implementation and is not the "
            "pipelined GPU path the budget's 6.8 ms describes"
        )
    return out
