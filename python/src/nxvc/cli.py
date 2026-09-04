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
from ._ffi import TileMode, Tool

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
        print(f"                unsupported by the reference decoder: {Tool.names(unsupported)}")
    non_phase1 = stream.non_phase1_tools() & ~unsupported
    if non_phase1:
        print(f"                unsupported by the Phase 1 decoder: {Tool.names(non_phase1)}")
    print(
        f"  ext_len       {stream.ext_len} ({len(stream.tlvs)} TLVs, "
        f"{sum(1 for t in stream.tlvs if t.private)} private)"
    )
    for tlv in stream.tlvs:
        print(f"    tlv 0x{tlv.type:04x}  {tlv.length} bytes")
    grid = f"  tile grid     {stream.cols_per_eye}x{stream.rows}"
    if stream.eyes > 1:
        grid += f" per eye, cols {stream.cols}"
    print(f"{grid} = {stream.tile_count} tiles")

    offset = stream.total_size
    for n, frame in enumerate(frames):
        h = frame.header
        print(
            f"frame {n} @{offset}: num {h.frame_number}  bytes {h.frame_bytes}  "
            f"qp {h.base_qp}  cqpo {h.chroma_qp_off:+d}  aqpo {h.alpha_qp_off:+d}  "
            f"matrix {h.quant_matrix}  tables 0x{h.tables_present:02x}  "
            f"refs 0x{h.ref_slots:02x}  flags 0x{h.flags:02x}"
            f"{'  dir-layer' if h.intra_dir_layer else ''}"
            f"{'  warp' if h.warp_present else ''}  tiles {len(frame.tiles)}"
        )
        for eye, m in enumerate(frame.warp):
            print(
                f"  warp_ext eye {eye}  Q10.21 [{m[0]} {m[1]} {m[2]} / "
                f"{m[3]} {m[4]} {m[5]}]  Q2.29 [{m[6]} {m[7]} {m[8]}]"
            )
        if h.table_sets:
            print(
                f"  tables: sets {h.table_sets}, "
                f"{bitstream.table_set_bytes(stream.tools)} bytes each"
            )
        print("  pose:" + "".join(f" {b:02x}" for b in h.pose))
        if args.tiles:
            for row in frame.rows:
                for tile in row.tiles:
                    t = tile.header
                    if not t.mv_present:
                        vector = ""
                    elif t.mode == TileMode.STEREO:
                        vector = f" disp{t.disparity}"
                    else:
                        vector = f" mv({t.mv_x},{t.mv_y})"
                    print(
                        f"  tile {t.tile_index:4d} eye{row.eye} row{row.header.row_index}"
                        f"  {t.mode_name:<9s} res{t.res_level} "
                        f"{'c444' if t.chroma444 else 'c420'} "
                        f"qp{t.resolved_qp(h.base_qp):2d} "
                        f"dq{t.qp_delta:+3d} ts{t.tskip} set{t.table_set} "
                        f"nsub{t.nsub_log2} wm{t.wm_id} ref{t.ref_sel}{vector} "
                        f"{t.payload_len} bytes"
                    )
            for i, row in enumerate(frame.rows):
                skipped = row.header.skipped_tiles(stream.cols_per_eye)
                if skipped:
                    print(
                        f"  row {row.header.row_index} eye {row.eye}: "
                        f"tiles {skipped} are WARP_SKIP and not transmitted"
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
            "non_phase1_tools": Tool.names(stream.non_phase1_tools()),
            "ext_len": stream.ext_len,
            "tlvs": [{"type": t.type, "length": t.length} for t in stream.tlvs],
            "eyes": stream.eyes,
            "cols_per_eye": stream.cols_per_eye,
            "rows": stream.rows,
            "cols": stream.cols,
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
                "table_sets": f.header.table_sets,
                "table_set_bytes": bitstream.table_set_bytes(stream.tools),
                "ref_slots": f.header.ref_slots,
                "flags": f.header.flags,
                "intra_dir_layer": f.header.intra_dir_layer,
                "warp_present": f.header.warp_present,
                "warp": [list(m) for m in f.warp],
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
                        "wm_id": t.header.wm_id,
                        "ref_sel": t.header.ref_sel,
                        "eye": t.header.eye,
                        "mv_present": t.header.mv_present,
                        "disparity": t.header.disparity,
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

    from ._ffi import NXVC_BITSTREAM_MINOR, library_minor, version_string

    print(f"nxvc python bindings {__version__}")
    print(f"  syntax parsed       v1.{NXVC_BITSTREAM_MINOR}")
    print(f"  library available   {NXVC_AVAILABLE}")
    print(f"  library path        {NXVC_LIBRARY_PATH or '-'}")
    if NXVC_AVAILABLE:
        minor = library_minor()
        note = ""
        if minor is not None and minor != NXVC_BITSTREAM_MINOR:
            note = (
                "  (ahead of the parser)"
                if minor > NXVC_BITSTREAM_MINOR
                else "  (behind the parser)"
            )
        print(f"  library version     {version_string() or 'unknown'}{note}")
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
    # nxv-enc spells the directional-intra choice with one flag: `layer` is the
    # layered form (frame flag bit 2), which implies the tool itself.
    intra_dir = {"off": (0, 0), "on": (1, 0), "layer": (1, 1)}.get(args.intra_dir)

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
    # Everything below is "unset means the C default": nxvc_config_default()
    # turns the RD trellis and the v2 intra tools on, and passing an explicit 0
    # for a flag the user never mentioned would quietly turn them off.
    if args.rdo is not None:
        kwargs["rdo"] = 1 if args.rdo else 0
    if args.rdo_lambda is not None:
        kwargs["rdo_lambda_q8"] = int(args.rdo_lambda * 256.0 + 0.5)
    if args.qp_search:
        kwargs["qp_search"] = args.qp_search
    if args.wm is not None:
        kwargs["wm_id"] = 255 if args.wm == "auto" else int(args.wm)
    if intra_dir is not None:
        kwargs["intra_dir"], kwargs["intra_dir_layer"] = intra_dir
    if args.intra_dir_cand:
        kwargs["intra_dir_cand"] = args.intra_dir_cand
    if args.ctx is not None:
        kwargs["ctx_v2"] = 1 if args.ctx == "v2" else 0
    if args.sign_hide is not None:
        kwargs["sign_hide"] = 1 if args.sign_hide else 0
    if args.split4 is not None:
        kwargs["split4"] = 1 if args.split4 == "on" else 0
    if args.cfl is not None:
        kwargs["cfl"] = 1 if args.cfl == "on" else 0

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
    # Encoder tuning (syntax v1.2) and the v2 intra tools (v1.3), spelled as
    # nxv-enc spells them.  Every one defaults to None = "leave the C default".
    e.add_argument("--rdo", dest="rdo", action="store_const", const=True, default=None,
                   help="RD trellis quantizer (the reference default)")
    e.add_argument("--no-rdo", dest="rdo", action="store_const", const=False,
                   help="plain dead-zone quantizer")
    e.add_argument("--rdo-lambda", dest="rdo_lambda", type=float, default=None,
                   help="RD lambda scale (default 0.30)")
    e.add_argument("--qp-search", dest="qp_search", type=int, default=0,
                   help="try per-tile qp_delta in [-N, +N]")
    e.add_argument("--wm", default=None, choices=("0", "1", "2", "3", "auto"),
                   help="per-tile weighting matrix id (tool bit 20 WM_ID)")
    e.add_argument("--intra-dir", dest="intra_dir", default=None,
                   choices=("off", "on", "layer"),
                   help="directional intra (tool bit 17); `layer` predicts the "
                        "DC-plane residual instead of the samples")
    e.add_argument("--intra-dir-cand", dest="intra_dir_cand", type=int, default=0,
                   help="modes RD-checked per block (0 = the encoder default)")
    e.add_argument("--ctx", default=None, choices=("v1", "v2"),
                   help="12 or 16 entropy contexts (tool bit 21 CTX_V2)")
    e.add_argument("--sign-hide", dest="sign_hide", action="store_const",
                   const=True, default=None,
                   help="sign data hiding (tool bit 22; the reference default)")
    e.add_argument("--split4", dest="split4", default=None,
                   choices=("off", "on"),
                   help="per-block 4x4 transform split (tool bit 19)")
    e.add_argument("--cfl", dest="cfl", default=None, choices=("off", "on"),
                   help="chroma from luma (tool bit 24; needs 17 and 21)")
    e.add_argument("--no-sign-hide", dest="sign_hide", action="store_const", const=False,
                   help="code every sign")
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
