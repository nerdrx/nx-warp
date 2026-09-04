"""nxq -- the NX Warp quality and comparison harness library.

Modules:

``yuv``       raw planar YUV geometry, frame I/O, pose logs
``metrics``   PSNR / SSIM / MS-SSIM in numpy
``bdrate``    Bjontegaard delta rate and delta PSNR
``ffmpeg``    capability probe, x264/x265 anchors, libvmaf
``codec``     driving ``nxv-enc`` / ``nxv-dec``
``cpu``       the ``chrt -i 0 taskset -c 28-31 nice -n 19`` discipline
"""

__all__ = ["yuv", "metrics", "bdrate", "ffmpeg", "codec", "cpu"]

__version__ = "0.1.0"
