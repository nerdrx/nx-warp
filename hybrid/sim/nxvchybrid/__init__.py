"""nxvchybrid -- the NX Warp hybrid-mode CPU experiment.

A rate-distortion simulator for the hybrid decode path of PAPER.md 1.7 / 2.9 /
3.5: a hardware HEVC base layer plus our pose-warped enhancement layer.  It
answers, in numbers, how a total bitrate should split between the two on a
Pico 4, and what that buys against HEVC alone and against our pure codec.

Entry point: ``nxvchybrid.cli.main`` (the ``nxvc-hybridsim`` script).
"""

__all__ = ["base", "cli", "codec", "cpu", "hybrid", "metrics", "panorama",
           "report", "sweep", "warp", "yuv"]
__version__ = "0.1.0"
