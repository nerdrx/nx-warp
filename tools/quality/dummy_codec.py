#!/usr/bin/env python3
"""A stand-in for ``nxv-enc`` / ``nxv-dec``.

The real codec CLIs are being built in ``ref/tools``.  Until they land, this
script speaks the identical command line so the whole harness -- encode,
decode, metrics, BD-rate, plots, report -- can be proven end to end:

.. code-block:: text

    dummy_codec.py enc --in file.yuv --w W --h H --pix yuv444p|yuv420p --qp N --out out.nxv
    dummy_codec.py dec --in out.nxv --out out.yuv

It is deliberately *not* a real codec.  It quantises each plane with a uniform
dead-zone-free scalar quantiser whose step follows the H.264/HEVC ladder
(``step = 2^(qp/6)``) and then deflates the indices.  That is enough to produce
a genuine rate-distortion curve -- bitrate falls and PSNR falls as QP rises --
which is all the harness needs in order to be tested.  Do not read anything
into the numbers it produces.

Use it with::

    --codec-enc 'python3 tools/quality/dummy_codec.py enc' \\
    --codec-dec 'python3 tools/quality/dummy_codec.py dec'
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nxq.yuv import Format  # noqa: E402

MAGIC = b"NXVD"          # "NX Warp dummy" -- not the real bitstream magic
VERSION = 1
HEADER = "<4sBHHBBI"     # magic, version, w, h, pix_index, qp, frames
PIX_INDEX = {"yuv444p": 0, "yuv420p": 1}
PIX_NAME = {v: k for k, v in PIX_INDEX.items()}


def qp_to_step(qp: int) -> float:
    """H.264-style step ladder: six QP steps double the quantiser."""
    return 2.0 ** (qp / 6.0)


def encode(src: str, fmt: Format, qp: int, out: str) -> int:
    nframes = fmt.frame_count(src)
    step = qp_to_step(qp)
    payload = bytearray()
    with open(src, "rb") as fh:
        for _ in range(nframes):
            buf = fh.read(fmt.frame_bytes)
            a = np.frombuffer(buf, np.uint8).astype(np.float32)
            # Quantise around the 8-bit midpoint so mid-grey stays exact.
            idx = np.rint((a - 128.0) / step).astype(np.int32)
            lo, hi = int(idx.min()), int(idx.max())
            span = hi - lo
            if span <= 255:
                payload += struct.pack("<Bi", 1, lo)
                payload += (idx - lo).astype(np.uint8).tobytes()
            else:
                payload += struct.pack("<Bi", 2, lo)
                payload += (idx - lo).astype(np.uint16).tobytes()
    body = zlib.compress(bytes(payload), 6)
    with open(out, "wb") as fh:
        fh.write(
            struct.pack(HEADER, MAGIC, VERSION, fmt.width, fmt.height,
                        PIX_INDEX[fmt.pix_fmt], qp, nframes)
        )
        fh.write(body)
    return os.path.getsize(out)


def decode(src: str, out: str) -> None:
    with open(src, "rb") as fh:
        blob = fh.read()
    hsize = struct.calcsize(HEADER)
    magic, version, w, h, pix, qp, nframes = struct.unpack(HEADER, blob[:hsize])
    if magic != MAGIC:
        raise SystemExit(f"{src}: not a dummy-codec stream (magic {magic!r})")
    if version != VERSION:
        raise SystemExit(f"{src}: unsupported version {version}")
    fmt = Format(w, h, PIX_NAME[pix])
    step = qp_to_step(qp)
    payload = zlib.decompress(blob[hsize:])
    n = fmt.frame_bytes
    off = 0
    with open(out, "wb") as fh:
        for _ in range(nframes):
            width_code, lo = struct.unpack_from("<Bi", payload, off)
            off += struct.calcsize("<Bi")
            dt = np.uint8 if width_code == 1 else np.uint16
            count = n
            idx = np.frombuffer(payload, dt, count=count, offset=off).astype(np.int32) + lo
            off += count * np.dtype(dt).itemsize
            rec = np.clip(np.rint(idx * step + 128.0), 0, 255).astype(np.uint8)
            fh.write(rec.tobytes())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    e = sub.add_parser("enc", help="encode a raw YUV sequence")
    e.add_argument("--in", dest="inp", required=True)
    e.add_argument("--w", type=int, required=True)
    e.add_argument("--h", type=int, required=True)
    e.add_argument("--pix", default="yuv444p", choices=sorted(PIX_INDEX))
    e.add_argument("--qp", type=int, required=True)
    e.add_argument("--out", required=True)

    d = sub.add_parser("dec", help="decode back to raw YUV")
    d.add_argument("--in", dest="inp", required=True)
    d.add_argument("--out", required=True)

    args = ap.parse_args(argv)
    if args.mode == "enc":
        if not 0 <= args.qp <= 63:
            ap.error("--qp must be in [0, 63]")
        size = encode(args.inp, Format(args.w, args.h, args.pix), args.qp, args.out)
        print(f"dummy-enc: qp={args.qp} -> {size} bytes")
    else:
        decode(args.inp, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
