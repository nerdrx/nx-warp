"""``python -m nxvc`` -- info, encode and decode, mirroring the reference CLIs.

``info`` uses the pure-Python parser in :mod:`nxvc.bitstream` and therefore
works with no shared library present; ``--library`` re-runs it through the C
decoder as well, which is the only thing that can validate the entropy-coded
payload.  ``encode`` and ``decode`` need the library and mirror the flags of
``nxv-enc`` / ``nxv-dec`` so a harness command line can be moved between them
unchanged.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import NXVC_AVAILABLE, NXVC_LIBRARY_PATH, NXVC_LOAD_ERROR, __version__, bitstream
from ._ffi import Tool

_PIX = ("yuv420p", "yuv444p")


def _die(msg: str, code: int = 1) -> int:
    print(f"nxvc: {msg}", file=sys.stderr)
    return code


def _need_library() -> None:
    if not NXVC_AVAILABLE:
        raise SystemExit(f"nxvc: {NXVC_LOAD_ERROR}")


# ----------------------------------------------------------------------- info


def _tools_line(mask: int) -> str:
    names = Tool.names(mask)
    return f"0x{mask:016x}" + (f"  ({', '.join(names)})" if names else "")


def cmd_info(args: argparse.Namespace) -> int:
    if args.probe:
        return cmd_probe(args)
    path = args.file or args.input
    if not path:
        return _die("info needs a file: nxvc info FILE.nxv")
    data = Path(path).read_bytes()

    try:
        stream = bitstream.parse_stream_header(data)
    except bitstream.BitstreamError as exc:
        return _die(f"{path}: {exc}", 2)

    frames: list[bitstream.Frame] = []
    error = None
    pos = stream.total_size
    while pos < len(data):
        if args.frames and len(frames) >= args.frames:
            break
        try:
            frame = bitstream.parse_frame(data, pos, stream)
        except bitstream.BitstreamError as exc:
            error = exc
            break
        frames.append(frame)
        pos += frame.header.frame_bytes

    if args.json:
        print(json.dumps(_info_json(path, data, stream, frames, error), indent=2))
        return 0 if error is None else 2

    print(f"stream header ({stream.total_size} bytes)")
    print(f"  magic         0x{stream.magic:08x}")
    print(f"  version       {stream.version}")
    print(f"  profile/level {stream.profile}/{stream.level}")
    print(f"  tile_size     {stream.tile_size}x{stream.tile_size}")
    print(
        f"  size          {stream.width}x{stream.height}  eyes {stream.eyes}"
        f"  bitdepth {stream.bit_depth}"
    )
    print(f"  chroma        {'4:4:4' if stream.chroma444 else '4:2:0'}")
    print(f"  color xform   {'YCoCg-R' if stream.color_transform else 'none'}")
    print(f"  color space   {stream.color_space_name}")
    print(f"  alpha         {stream.alpha_present}")
    print(f"  layers        {stream.num_layers}")
    print(f"  tools         {_tools_line(stream.tools)}")
    unsupported = stream.unsupported_tools()
    if unsupported:
        print(f"                unsupported by the Phase 1 decoder: {Tool.names(unsupported)}")
    print(
        f"  ext_len       {stream.ext_len} ({len(stream.tlvs)} TLVs, "
        f"{sum(1 for t in stream.tlvs if t.private)} private)"
    )
    for tlv in stream.tlvs:
        print(f"    tlv 0x{tlv.type:04x}  {tlv.length} bytes")
    print(
        f"  tile grid     {stream.tiles_x}x{stream.tiles_y} = {stream.tile_count} tiles"
    )

    offset = stream.total_size
    for n, frame in enumerate(frames):
        h = frame.header
        print(
            f"frame {n} @{offset}: num {h.frame_number}  bytes {h.frame_bytes}  "
            f"qp {h.base_qp}  cqpo {h.chroma_qp_off:+d}  aqpo {h.alpha_qp_off:+d}  "
            f"matrix {h.quant_matrix}  tables 0x{h.tables_present:02x}  "
            f"flags 0x{h.flags:02x}  tiles {len(frame.tiles)}"
        )
        print("  pose:" + "".join(f" {b:02x}" for b in h.pose))
        if args.tiles:
            for tile in frame.tiles:
                t = tile.header
                print(
                    f"  tile {t.tile_index:4d}  {t.mode_name:<9s} res{t.res_level} "
                    f"{'c444' if t.chroma444 else 'c420'} qp{t.resolved_qp(h.base_qp):2d} "
                    f"dq{t.qp_delta:+3d} ts{t.tskip} set{t.table_set} "
                    f"nsub{t.nsub_log2} {t.payload_len} bytes"
                )
        offset += h.frame_bytes

    reason = bitstream.phase1_reject_reason(stream, frames[0] if frames else None)
    if reason:
        print(f"phase 1: would be refused -- {reason}")
    print(f"{len(frames)} frame(s)")

    if error is not None:
        print(f"parse stopped: {error}", file=sys.stderr)
        return 2

    if args.library:
        return _info_with_library(path, data)
    return 0


def _info_json(path, data, stream, frames, error) -> dict:
    return {
        "file": str(path),
        "bytes": len(data),
        "stream": {
            "magic": stream.magic,
            "version": stream.version,
            "profile": stream.profile,
            "level": stream.level,
            "tile_size": stream.tile_size,
            "width": stream.width,
            "height": stream.height,
            "eyes": stream.eyes,
            "bit_depth": stream.bit_depth,
            "num_layers": stream.num_layers,
            "chroma": "4:4:4" if stream.chroma444 else "4:2:0",
            "color_transform": stream.color_transform,
            "color_space": stream.color_space,
            "color_space_name": stream.color_space_name,
            "alpha_present": stream.alpha_present,
            "tools": stream.tools,
            "tool_names": stream.tool_names(),
            "unsupported_tools": Tool.names(stream.unsupported_tools()),
            "ext_len": stream.ext_len,
            "tlvs": [{"type": t.type, "length": t.length} for t in stream.tlvs],
            "tiles_x": stream.tiles_x,
            "tiles_y": stream.tiles_y,
            "tile_count": stream.tile_count,
        },
        "frames": [
            {
                "frame_number": f.header.frame_number,
                "frame_bytes": f.header.frame_bytes,
                "base_qp": f.header.base_qp,
                "chroma_qp_off": f.header.chroma_qp_off,
                "alpha_qp_off": f.header.alpha_qp_off,
                "quant_matrix": f.header.quant_matrix,
                "tables_present": f.header.tables_present,
                "flags": f.header.flags,
                "tile_count": len(f.tiles),
                "payload_bytes": f.payload_bytes,
                "tiles": [
                    {
                        "tile_index": t.header.tile_index,
                        "mode": t.header.mode_name,
                        "res_level": t.header.res_level,
                        "qp": t.header.resolved_qp(f.header.base_qp),
                        "qp_delta": t.header.qp_delta,
                        "tskip": t.header.tskip,
                        "table_set": t.header.table_set,
                        "nsub_log2": t.header.nsub_log2,
                        "payload_len": t.header.payload_len,
                    }
                    for t in f.tiles
                ],
            }
            for f in frames
        ],
        "error": str(error) if error is not None else None,
    }


def _info_with_library(path, data: bytes) -> int:
    _need_library()
    from .codec import Decoder

    with Decoder() as dec:
        try:
            n = 0
            for _ in dec.frames(data):
                n += 1
        except Exception as exc:  # NxvcError or a structural failure
            return _die(f"{path}: the reference decoder refused this stream: {exc}", 2)
    print(f"reference decoder: {n} frame(s) decoded cleanly")
    return 0


def cmd_probe(args: argparse.Namespace) -> int:
    """Report what this installation can do -- the first thing to run when stuck."""
    from . import metrics

    print(f"nxvc python bindings {__version__}")
    print(f"  library available   {NXVC_AVAILABLE}")
    print(f"  library path        {NXVC_LIBRARY_PATH or '-'}")
    print(f"  metrics backend     {metrics.BACKEND}")
    print(f"  quality harness     {metrics.nxq_path() or 'not found'}")
    if not NXVC_AVAILABLE:
        print()
        print(NXVC_LOAD_ERROR)
        return 1
    return 0


# --------------------------------------------------------------------- encode


def cmd_encode(args: argparse.Namespace) -> int:
    _need_library()
    import numpy as np

    from .codec import Encoder, plane_shapes, read_planar_yuv, _pix_to_chroma

    if args.pix not in _PIX:
        return _die("--pix must be yuv420p or yuv444p", 2)
    if args.rgb and args.pix != "yuv444p":
        return _die("--rgb requires --pix yuv444p (YCoCg-R needs 4:4:4)", 2)

    tskip = {"off": 0, "on": 1, "auto": 2}[args.tskip]
    nsub = 255 if args.nsub == "auto" else int(args.nsub)

    kwargs = dict(
        pix=args.pix,
        base_qp=args.qp,
        chroma_qp_off=args.chroma_qp_off,
        quant_matrix=args.matrix,
        lossless=1 if args.lossless else 0,
        transform_skip=tskip,
        nsub_log2=nsub,
        tile_chroma420=1 if args.tile_420 else 0,
        custom_tables=1 if args.custom_tables else 0,
        color_transform=1 if args.rgb else 0,
        color_space=_color_space(args),
    )

    # --frames 0 is "all of them", the nxv-enc meaning; None is what
    # read_planar_yuv spells that with.
    frames = read_planar_yuv(
        args.input, args.w, args.h, args.pix, frames=args.frames or None
    )
    if not frames:
        return _die(f"{args.input}: no complete frame of {args.w}x{args.h} {args.pix}")

    with Encoder(args.w, args.h, **kwargs) as enc:
        qp_map = _read_map(args.qp_map, enc.layout, len(frames))
        res_map = _read_map(args.res_map, enc.layout, len(frames))
        out = bytearray(enc.stream_header())
        for i, planes in enumerate(frames):
            out += enc.encode(
                planes,
                qp_map[i] if qp_map else None,
                res_map[i] if res_map else None,
            )
    Path(args.output).write_bytes(bytes(out))
    if not args.quiet:
        bits = len(out) * 8
        print(
            f"{len(frames)} frame(s), {args.w}x{args.h} {args.pix} -> {len(out)} bytes "
            f"({bits / len(frames) / 1000.0:.1f} kbit/frame)"
        )
    return 0


def _color_space(args) -> int:
    """SYNTAX.md 2.2: RGB iff YCoCg-R, so --rgb picks RGB unless told otherwise."""
    from ._ffi import ColorSpace

    names = {
        "unspecified": ColorSpace.UNSPECIFIED,
        "bt709-limited": ColorSpace.YCBCR_709_LIMITED,
        "bt709-full": ColorSpace.YCBCR_709_FULL,
        "rgb": ColorSpace.RGB,
    }
    if args.color_space is None:
        return ColorSpace.RGB if args.rgb else ColorSpace.UNSPECIFIED
    return names[args.color_space]


def _read_map(path, layout, frames: int):
    """One byte per tile per frame, raster order -- the ``--qp-map`` format."""
    if not path:
        return None
    import numpy as np

    raw = Path(path).read_bytes()
    per = layout.tile_count
    if len(raw) < per:
        raise SystemExit(
            f"nxvc: {path}: {len(raw)} bytes is less than one frame of "
            f"{per} tiles ({layout.tiles_x}x{layout.tiles_y})"
        )
    have = len(raw) // per
    arr = np.frombuffer(raw[: have * per], dtype=np.uint8).reshape(have, per)
    # A single-frame map applies to every frame, which is what a static
    # foveation pattern is.
    return [arr[min(i, have - 1)] for i in range(frames)]


# --------------------------------------------------------------------- decode


def cmd_decode(args: argparse.Namespace) -> int:
    _need_library()
    from .codec import Decoder, write_planar_yuv

    data = Path(args.input).read_bytes()
    with Decoder() as dec:
        info, _ = dec.parse_stream_header(data)
        if args.pix and args.pix != info.pix_fmt:
            return _die(f"stream is {info.pix_fmt}, --pix says {args.pix}", 2)
        frames = []
        for planes in dec.frames(data):
            frames.append(planes)
            if args.frames and len(frames) >= args.frames:
                break
        write_planar_yuv(args.output, frames)
    if not args.quiet:
        print(f"{len(frames)} frame(s), {info.width}x{info.height} {info.pix_fmt}")
    return 0


# ---------------------------------------------------------------------- parser


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="python -m nxvc",
        description="NX Warp nxvc bitstream tools (Python bindings)",
    )
    p.add_argument("--version", action="version", version=f"nxvc {__version__}")
    sub = p.add_subparsers(dest="command")

    # info -----------------------------------------------------------------
    i = sub.add_parser("info", help="describe a .nxv stream (no library needed)")
    i.add_argument("file", nargs="?", help="the .nxv file")
    i.add_argument("--in", dest="input", help="the .nxv file (nxv-info spelling)")
    i.add_argument("--tiles", action="store_true", help="list every tile")
    i.add_argument("--frames", type=int, default=0, help="stop after N frames")
    i.add_argument("--json", action="store_true", help="machine-readable output")
    i.add_argument(
        "--library",
        action="store_true",
        help="also run the stream through the reference decoder",
    )
    i.add_argument("--probe", action="store_true", help="report what this install can do")
    i.set_defaults(func=cmd_info)

    # encode ---------------------------------------------------------------
    e = sub.add_parser("encode", help="encode raw planar YUV (mirrors nxv-enc)")
    e.add_argument("--in", dest="input", required=True)
    e.add_argument("--out", dest="output", required=True)
    e.add_argument("--w", type=int, required=True)
    e.add_argument("--h", type=int, required=True)
    e.add_argument("--pix", default="yuv420p", choices=_PIX)
    e.add_argument("--qp", type=int, default=28)
    e.add_argument("--res-map", dest="res_map")
    e.add_argument("--qp-map", dest="qp_map")
    e.add_argument("--frames", type=int, default=0)
    e.add_argument("--lossless", action="store_true")
    e.add_argument("--tile-420", dest="tile_420", action="store_true")
    e.add_argument("--custom-tables", action="store_true")
    e.add_argument("--rgb", action="store_true", help="planes are R,G,B; apply YCoCg-R")
    e.add_argument(
        "--color-space", dest="color_space", default=None,
        choices=("unspecified", "bt709-limited", "bt709-full", "rgb"),
        help="descriptive stream colour space; defaults to rgb with --rgb, "
             "else unspecified",
    )
    e.add_argument("--quiet", action="store_true")
    e.add_argument("--matrix", type=int, default=0, choices=(0, 1, 2, 3))
    e.add_argument("--chroma-qp-off", dest="chroma_qp_off", type=int, default=0)
    e.add_argument("--tskip", default="auto", choices=("off", "on", "auto"))
    e.add_argument("--nsub", default="auto", choices=("auto", "0", "1", "2", "3", "4", "5"))
    e.set_defaults(func=cmd_encode)

    # decode ---------------------------------------------------------------
    d = sub.add_parser("decode", help="decode to raw planar YUV (mirrors nxv-dec)")
    d.add_argument("--in", dest="input", required=True)
    d.add_argument("--out", dest="output", required=True)
    d.add_argument("--pix", choices=_PIX, help="checked against the stream, not trusted")
    d.add_argument("--frames", type=int, default=0)
    d.add_argument("--quiet", action="store_true")
    d.set_defaults(func=cmd_decode)

    # probe ----------------------------------------------------------------
    pr = sub.add_parser("probe", help="report library and metric backends")
    pr.set_defaults(func=cmd_probe)

    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not getattr(args, "command", None):
        parser.print_help()
        return 2
    return args.func(args)


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
