"""Encoder and Decoder over the nxvc C ABI, with numpy planes in and out.

Planar 8-bit images are represented as a list of 2D ``uint8`` numpy arrays in
plane order ``[Y, Co, Cg, A]`` (``[R', G', B', A]`` when the stream uses the
YCoCg-R colour transform).  For 4:2:0 the chroma planes are half size in each
dimension; for 4:4:4 they are full size.  Alpha, when present, is full size.

Per-tile foveation maps are ``uint8`` numpy arrays with one entry per tile in
raster order.  They may be given flat (``tile_count``) or shaped
(``tiles_y, tiles_x``); both are accepted and the 2D form is the convenient
one.

Nothing in this module reimplements any codec behaviour.  It marshals buffers
and copies out the structures the C library fills in; the reference codec
remains the only implementation of the bitstream.
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass, field
from typing import Iterator, Sequence

import numpy as np

from . import _ffi
from ._ffi import (
    Chroma,
    ColorSpace,
    ColorTransform,
    LibraryNotFound,
    NxvcError,
    Status,
    TileMode,
    Tool,
    nxvc_config,
    nxvc_encode_stats,
    nxvc_frame_info,
    nxvc_image,
    nxvc_stream_info,
    nxvc_tile_layout,
)

__all__ = [
    "TileLayout",
    "TileInfo",
    "StreamInfo",
    "FrameInfo",
    "EncoderConfig",
    "EncodeStats",
    "Encoder",
    "Decoder",
    "tile_layout",
    "plane_shapes",
    "read_planar_yuv",
    "write_planar_yuv",
    "ycocgr_forward",
    "ycocgr_inverse",
]

Planes = list[np.ndarray]

#: Auto sentinels the C ABI uses for "pick it yourself".
NSUB_AUTO = 255
QUANT_MATRIX_CUSTOM = 255


# ------------------------------------------------------------------ dataclasses


@dataclass(frozen=True)
class TileLayout:
    """``nxvc_tile_layout``: the tile grid of a picture."""

    tiles_x: int
    tiles_y: int
    tile_count: int
    tile_size: int

    @classmethod
    def _from_c(cls, c: nxvc_tile_layout) -> "TileLayout":
        return cls(int(c.tiles_x), int(c.tiles_y), int(c.tile_count), int(c.tile_size))

    @property
    def shape(self) -> tuple[int, int]:
        """``(tiles_y, tiles_x)`` -- the shape of a per-tile map."""
        return (self.tiles_y, self.tiles_x)


@dataclass(frozen=True)
class TileInfo:
    """``nxvc_tile_info``: one tile's coding decisions, as reported by the codec."""

    tile_index: int
    payload_len: int
    layer: int
    eye: int
    mode: int
    res_level: int
    chroma444: int
    alpha_mode: int
    table_set: int
    nsub_log2: int
    tskip: int
    wgt: int
    ref_sel: int
    mv_present: int
    qp_delta: int
    mv_x: int
    mv_y: int
    alpha_value: int
    qp: int
    wm_id: int
    intra_dir: int
    skipped: int
    concealed: int
    disparity: int
    ref_delta: int
    age_since_coded: int

    _FIELDS = (
        "tile_index",
        "payload_len",
        "layer",
        "eye",
        "mode",
        "res_level",
        "chroma444",
        "alpha_mode",
        "table_set",
        "nsub_log2",
        "tskip",
        "wgt",
        "ref_sel",
        "mv_present",
        "qp_delta",
        "mv_x",
        "mv_y",
        "alpha_value",
        "qp",
        "wm_id",
        "intra_dir",
        "skipped",
        "concealed",
        "disparity",
        "ref_delta",
        "age_since_coded",
    )

    @classmethod
    def _from_c(cls, c: _ffi.nxvc_tile_info) -> "TileInfo":
        return cls(**{name: int(getattr(c, name)) for name in cls._FIELDS})

    @property
    def mode_name(self) -> str:
        return TileMode.name(self.mode)

    @property
    def coded_size(self) -> int:
        return 64 >> self.res_level


@dataclass(frozen=True)
class StreamInfo:
    """``nxvc_stream_info``: the parsed stream header, as the C decoder sees it."""

    magic: int
    version: int
    profile: int
    level: int
    tile_size: int
    width: int
    height: int
    eyes: int
    bit_depth: int
    num_layers: int
    chroma: int
    color_transform: int
    color_space: int
    alpha: int
    tools: int
    layer_desc: tuple[int, int, int, int]
    ext_len: int
    ext_tlv_count: int
    ext_unknown_count: int

    @classmethod
    def _from_c(cls, c: nxvc_stream_info) -> "StreamInfo":
        return cls(
            magic=int(c.magic),
            version=int(c.version),
            profile=int(c.profile),
            level=int(c.level),
            tile_size=int(c.tile_size),
            width=int(c.width),
            height=int(c.height),
            eyes=int(c.eyes),
            bit_depth=int(c.bit_depth),
            num_layers=int(c.num_layers),
            chroma=int(c.chroma),
            color_transform=int(c.color_transform),
            color_space=int(c.color_space),
            alpha=int(c.alpha),
            tools=int(c.tools),
            layer_desc=tuple(int(x) for x in c.layer_desc),  # type: ignore[arg-type]
            ext_len=int(c.ext_len),
            ext_tlv_count=int(c.ext_tlv_count),
            ext_unknown_count=int(c.ext_unknown_count),
        )

    @property
    def chroma444(self) -> bool:
        return self.chroma == Chroma.C444

    @property
    def color_space_name(self) -> str:
        """Human name of the descriptive ``color_space`` field (SYNTAX.md 2.2)."""
        return ColorSpace.name(self.color_space)

    @property
    def pix_fmt(self) -> str:
        """``yuv444p`` or ``yuv420p``, the name the CLIs and the quality harness use."""
        return "yuv444p" if self.chroma444 else "yuv420p"

    def tool_names(self) -> list[str]:
        return Tool.names(self.tools)


@dataclass(frozen=True)
class FrameInfo:
    """``nxvc_frame_info``: a frame header without decoding the frame."""

    frame_number: int
    base_qp: int
    chroma_qp_off: int
    alpha_qp_off: int
    quant_matrix: int
    tables_present: int
    ref_slots: int
    flags: int
    frame_bytes: int
    pose: bytes
    tile_count: int
    #: Frame flags bit 3 (syntax v1.4): ``warp_ext()`` follows the frame header.
    warp_present: int = 0
    #: The nine int32 of ``warp_ext()`` per eye, h00..h22.
    warp: tuple[tuple[int, ...], ...] = ()

    @classmethod
    def _from_c(cls, c: nxvc_frame_info) -> "FrameInfo":
        return cls(
            frame_number=int(c.frame_number),
            base_qp=int(c.base_qp),
            chroma_qp_off=int(c.chroma_qp_off),
            alpha_qp_off=int(c.alpha_qp_off),
            quant_matrix=int(c.quant_matrix),
            tables_present=int(c.tables_present),
            ref_slots=int(c.ref_slots),
            flags=int(c.flags),
            frame_bytes=int(c.frame_bytes),
            pose=bytes(bytearray(c.pose)),
            tile_count=int(c.tile_count),
            warp_present=int(getattr(c, "warp_present", 0)),
            warp=tuple(tuple(int(v) for v in row) for row in getattr(c, "warp", ())),
        )

    @property
    def tile_map_reset(self) -> bool:
        return bool(self.flags & 1)

    @property
    def table_sets(self) -> list[int]:
        return [k for k in range(8) if self.tables_present & (1 << k)]


@dataclass(frozen=True)
class EncodeStats:
    """``nxvc_encode_stats``: where the bits went in the last encoded frame.

    Only filled when the encoder was created with ``collect_stats=1``; the
    reference encoder does extra work to produce it.
    """

    bytes_total: int
    bytes_frame_header: int
    bytes_tables: int
    bytes_row_headers: int
    bytes_tile_headers: int
    bytes_payload: int
    bytes_rans_init: int
    bits_dc_plane: int
    bits_luma_blocks: int
    bits_chroma_blocks: int
    bits_alpha_blocks: int
    tiles: int
    tiles_tskip: int
    tiles_res: tuple[int, int, int]
    lanes_total: int

    @classmethod
    def _from_c(cls, c: nxvc_encode_stats) -> "EncodeStats":
        values = {}
        for name, _ in c._fields_:
            v = getattr(c, name)
            values[name] = tuple(int(x) for x in v) if name == "tiles_res" else int(v)
        return cls(**values)

    @property
    def overhead_bytes(self) -> int:
        """Everything that is not entropy-coded tile payload."""
        return self.bytes_total - self.bytes_payload


@dataclass
class EncoderConfig:
    """``nxvc_config`` as a Python dataclass, with the header's own defaults.

    Only the fields you set are overridden: the object is applied on top of
    whatever ``nxvc_config_default`` produces, so a field the C side grows a
    new default for keeps that default here too.
    """

    width: int
    height: int
    chroma: int = Chroma.C420
    bit_depth: int = 8
    color_transform: int = ColorTransform.NONE
    color_space: int = ColorSpace.UNSPECIFIED
    alpha: int = 0
    base_qp: int = 28
    chroma_qp_off: int = 0
    alpha_qp_off: int = 0
    quant_matrix: int = 0
    custom_matrix: bytes | None = None
    lossless: int = 0
    transform_skip: int = 0
    nsub_log2: int = NSUB_AUTO
    tile_chroma420: int = 0
    custom_tables: int = 0
    profile: int = 0
    level: int = 0
    collect_stats: int = 0

    # --- additive since syntax v1.2: encoder-side tuning.  They change which
    # levels and per-tile parameters are chosen, never how a stream decodes.
    #: ``None`` means "whatever ``nxvc_config_default`` chose" -- the reference
    #: encoder turns rdo and the v2 intra tools on, and pinning a 0 here would
    #: silently change what a bare ``Encoder(w, h)`` emits.
    rdo: int | None = None
    rdo_lambda_q8: int | None = None
    qp_search: int | None = None
    wm_id: int | None = None

    # --- additive since syntax v1.3.  These DO change the bitstream: each sets
    # a tool bit in the stream header and a decoder without it refuses the
    # stream at the handshake.
    intra_dir: int | None = None
    intra_dir_layer: int | None = None
    ctx_v2: int | None = None
    intra_dir_cand: int | None = None
    sign_hide: int | None = None

    # --- additive since syntax v1.4: the Phase 2 inter path.  ``width`` and
    # ``height`` are per eye; with ``eyes == 2`` the image planes are
    # ``eyes * width`` samples wide, one picture per eye, eye 0 first.
    eyes: int | None = None
    inter: int | None = None
    stereo: int | None = None
    intra_period: int | None = None
    ref_sel: int | None = None
    mv_range: int | None = None
    skip_thresh: int | None = None
    mode_lambda_q8: int | None = None

    #: Fields set explicitly by the caller; everything else keeps the C default.
    _explicit: set[str] = field(default_factory=set, repr=False, compare=False)

    @classmethod
    def from_kwargs(cls, width: int, height: int, **kwargs) -> "EncoderConfig":
        pix = kwargs.pop("pix", None)
        if pix is not None:
            kwargs["chroma"] = _pix_to_chroma(pix)
        cfg = cls(width=width, height=height, **kwargs)
        cfg._explicit = set(kwargs) | {"width", "height"}
        return cfg

    def to_c(self) -> nxvc_config:
        lib = _ffi.load()
        c = nxvc_config()
        lib.nxvc_config_default(ctypes.byref(c))
        explicit = self._explicit or {
            f.name for f in self.__dataclass_fields__.values() if not f.name.startswith("_")
        }
        for name in explicit:
            if name == "custom_matrix":
                continue
            value = getattr(self, name)
            if value is None:  # keep whatever nxvc_config_default() chose
                continue
            setattr(c, name, value)
        if self.custom_matrix is not None:
            if len(self.custom_matrix) != 128:
                raise ValueError(
                    f"custom_matrix must be 128 bytes (64 luma + 64 chroma), "
                    f"got {len(self.custom_matrix)}"
                )
            # Keep the buffer alive for as long as the config is used: the C
            # side stores the pointer, it does not copy.
            self._matrix_buf = (ctypes.c_uint8 * 128).from_buffer_copy(self.custom_matrix)
            c.custom_matrix = ctypes.cast(self._matrix_buf, _ffi.u8p)
            c.quant_matrix = QUANT_MATRIX_CUSTOM
        return c


def _pix_to_chroma(pix: str) -> int:
    p = pix.lower()
    if p in ("yuv420p", "420", "i420"):
        return Chroma.C420
    if p in ("yuv444p", "444"):
        return Chroma.C444
    raise ValueError(f"unsupported pixel format {pix!r}: use yuv420p or yuv444p")


# --------------------------------------------------------------------- helpers


def tile_layout(width: int, height: int) -> TileLayout:
    """``nxvc_tile_layout_get``.  Requires the library."""
    lib = _ffi.load()
    c = nxvc_tile_layout()
    lib.nxvc_tile_layout_get(int(width), int(height), ctypes.byref(c))
    return TileLayout._from_c(c)


def plane_shapes(
    width: int, height: int, chroma: int = Chroma.C420, alpha: bool = False
) -> list[tuple[int, int]]:
    """``(height, width)`` of each plane, in plane order.

    Pure arithmetic, no library needed -- the quality harness uses it to size
    reads of a raw YUV file.
    """
    cw = width if chroma == Chroma.C444 else (width + 1) // 2
    ch = height if chroma == Chroma.C444 else (height + 1) // 2
    shapes = [(height, width), (ch, cw), (ch, cw)]
    if alpha:
        shapes.append((height, width))
    return shapes


def _as_plane(arr: np.ndarray, shape: tuple[int, int], index: int) -> np.ndarray:
    a = np.asarray(arr)
    if a.dtype != np.uint8:
        raise TypeError(f"plane {index} must be uint8, got {a.dtype}")
    if a.ndim != 2:
        raise ValueError(f"plane {index} must be 2D, got shape {a.shape}")
    if a.shape != shape:
        raise ValueError(f"plane {index} must have shape {shape}, got {a.shape}")
    if a.strides[1] != 1:
        a = np.ascontiguousarray(a)
    return a


def _image_from_planes(planes: Sequence[np.ndarray], shapes: Sequence[tuple[int, int]]):
    """Build an ``nxvc_image`` viewing *planes*.  Returns (image, keepalive)."""
    if len(planes) < len(shapes):
        raise ValueError(f"expected {len(shapes)} planes, got {len(planes)}")
    img = nxvc_image()
    keep: list[np.ndarray] = []
    for i, shape in enumerate(shapes):
        a = _as_plane(planes[i], shape, i)
        keep.append(a)
        img.plane[i] = a.ctypes.data_as(_ffi.u8p)
        img.stride[i] = int(a.strides[0])
    return img, keep


def _as_map(m, layout: TileLayout, name: str) -> np.ndarray | None:
    if m is None:
        return None
    a = np.asarray(m)
    if a.ndim == 2:
        if a.shape != layout.shape:
            raise ValueError(f"{name} must have shape {layout.shape}, got {a.shape}")
        a = a.reshape(-1)
    if a.ndim != 1 or a.size != layout.tile_count:
        raise ValueError(
            f"{name} must have {layout.tile_count} entries (one per tile), got {a.size}"
        )
    return np.ascontiguousarray(a, dtype=np.uint8)


def _map_ptr(a: np.ndarray | None):
    return a.ctypes.data_as(_ffi.u8p) if a is not None else ctypes.cast(None, _ffi.u8p)


def _tiles_from(getter, handle) -> list[TileInfo]:
    count = ctypes.c_uint32(0)
    ptr = getter(handle, ctypes.byref(count))
    if not ptr:
        return []
    return [TileInfo._from_c(ptr[i]) for i in range(count.value)]


# --------------------------------------------------------------------- encoder


class Encoder:
    """The nxvc reference encoder.

    >>> enc = Encoder(256, 256, pix="yuv444p", base_qp=24)      # doctest: +SKIP
    >>> stream = enc.stream_header()                            # doctest: +SKIP
    >>> stream += enc.encode(planes)                            # doctest: +SKIP
    """

    def __init__(self, width: int, height: int, **kwargs) -> None:
        self.config = EncoderConfig.from_kwargs(width, height, **kwargs)
        self._lib = _ffi.load()
        c = self.config.to_c()
        st = ctypes.c_int(0)
        self._enc = self._lib.nxvc_encoder_create(ctypes.byref(c), ctypes.byref(st))
        if not self._enc:
            raise NxvcError(st.value, "nxvc_encoder_create")
        self.width = int(c.width)
        self.height = int(c.height)
        self.chroma = int(c.chroma)
        self.alpha = bool(c.alpha)
        #: 1 or 2.  ``width``/``height`` are per eye; with two eyes the image
        #: planes are ``eyes * width`` samples wide, one picture per eye, eye 0
        #: first (nxvc.h, syntax v1.4).
        self.eyes = int(getattr(c, "eyes", 1)) or 1
        self.layout = tile_layout(self.width, self.height)
        self._header_emitted = False

    # ------------------------------------------------------------- lifetime
    def close(self) -> None:
        enc, self._enc = getattr(self, "_enc", None), None
        if enc:
            self._lib.nxvc_encoder_destroy(enc)

    def __del__(self) -> None:  # best effort
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "Encoder":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _handle(self):
        if not self._enc:
            raise RuntimeError("encoder is closed")
        return self._enc

    # -------------------------------------------------------------- geometry
    @property
    def plane_shapes(self) -> list[tuple[int, int]]:
        return plane_shapes(
            self.width * self.eyes, self.height, self.chroma, self.alpha
        )

    # ---------------------------------------------------------------- header
    def add_tlv(self, type_: int, data: bytes) -> None:
        """Attach a TLV to the stream header.  Must precede :meth:`stream_header`."""
        if self._header_emitted:
            raise RuntimeError("add_tlv must be called before stream_header")
        if len(data) > 0xFFFF:
            raise ValueError("TLV payload exceeds 65535 bytes")
        _ffi.check(
            self._lib.nxvc_encoder_add_tlv(
                self._handle(), int(type_), _ffi.bytes_ptr(bytes(data)), len(data)
            ),
            "nxvc_encoder_add_tlv",
        )

    def stream_header(self, cap: int = 4096) -> bytes:
        """Serialize the stream header (64 bytes plus the TLV area)."""
        buf = (ctypes.c_uint8 * cap)()
        out = ctypes.c_size_t(0)
        _ffi.check(
            self._lib.nxvc_encoder_stream_header(
                self._handle(), ctypes.cast(buf, _ffi.u8p), cap, ctypes.byref(out)
            ),
            "nxvc_encoder_stream_header",
        )
        self._header_emitted = True
        return bytes(bytearray(buf[: out.value]))

    def set_pose(self, pose: bytes) -> None:
        """Set the 26 opaque pose bytes copied into the next frame header."""
        if len(pose) != 26:
            raise ValueError(f"pose must be exactly 26 bytes, got {len(pose)}")
        buf = (ctypes.c_uint8 * 26).from_buffer_copy(bytes(pose))
        self._lib.nxvc_encoder_set_pose(self._handle(), ctypes.cast(buf, _ffi.u8p))

    # ---------------------------------------------------------------- encode
    def encode(
        self,
        planes: Sequence[np.ndarray],
        qp_map: np.ndarray | None = None,
        res_map: np.ndarray | None = None,
        cap: int | None = None,
    ) -> bytes:
        """Encode one frame and return its bytes.

        *qp_map* and *res_map* are per-tile ``uint8`` arrays in raster order
        (shape ``(tiles_y, tiles_x)`` or flat), or None for "use base_qp" and
        "res_level 0".
        """
        img, _keep = _image_from_planes(planes, self.plane_shapes)
        qm = _as_map(qp_map, self.layout, "qp_map")
        rm = _as_map(res_map, self.layout, "res_map")
        if cap is None:
            # Worst case is bounded by the uncompressed picture plus per-tile
            # header overhead; be generous, the buffer is transient.
            cap = self.width * self.height * 3 + self.layout.tile_count * 64 + 65536
        buf = (ctypes.c_uint8 * cap)()
        out = ctypes.c_size_t(0)
        _ffi.check(
            self._lib.nxvc_encoder_encode_frame(
                self._handle(),
                ctypes.byref(img),
                _map_ptr(qm),
                _map_ptr(rm),
                ctypes.cast(buf, _ffi.u8p),
                cap,
                ctypes.byref(out),
            ),
            "nxvc_encoder_encode_frame",
        )
        return bytes(bytearray(buf[: out.value]))

    def tiles(self) -> list[TileInfo]:
        """Per-tile records of the most recently encoded frame."""
        return _tiles_from(self._lib.nxvc_encoder_tiles, self._handle())

    def stats(self) -> EncodeStats:
        """Bit accounting for the last encoded frame (needs ``collect_stats=1``)."""
        c = nxvc_encode_stats()
        _ffi.check(
            self._lib.nxvc_encoder_stats(self._handle(), ctypes.byref(c)),
            "nxvc_encoder_stats",
        )
        return EncodeStats._from_c(c)

    def tile_map(self, attr: str = "qp") -> np.ndarray:
        """One attribute of :meth:`tiles` as a ``(tiles_y, tiles_x)`` array."""
        tiles = self.tiles()
        values = np.array([getattr(t, attr) for t in tiles])
        if values.size != self.layout.tile_count:
            return values
        return values.reshape(self.layout.shape)


# --------------------------------------------------------------------- decoder


class Decoder:
    """The nxvc reference decoder.

    >>> dec = Decoder()                                          # doctest: +SKIP
    >>> info, n = dec.parse_stream_header(data)                  # doctest: +SKIP
    >>> planes, consumed = dec.decode(data[n:])                  # doctest: +SKIP
    """

    def __init__(self) -> None:
        self._lib = _ffi.load()
        st = ctypes.c_int(0)
        self._dec = self._lib.nxvc_decoder_create(ctypes.byref(st))
        if not self._dec:
            raise NxvcError(st.value, "nxvc_decoder_create")
        self.stream: StreamInfo | None = None

    def close(self) -> None:
        dec, self._dec = getattr(self, "_dec", None), None
        if dec:
            self._lib.nxvc_decoder_destroy(dec)

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "Decoder":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _handle(self):
        if not self._dec:
            raise RuntimeError("decoder is closed")
        return self._dec

    # ---------------------------------------------------------------- header
    def parse_stream_header(self, data: bytes) -> tuple[StreamInfo, int]:
        """Parse the stream header.  Returns the info and the bytes consumed."""
        consumed = ctypes.c_size_t(0)
        buf = bytes(data)
        _ffi.check(
            self._lib.nxvc_decoder_parse_stream_header(
                self._handle(), _ffi.bytes_ptr(buf), len(buf), ctypes.byref(consumed)
            ),
            "nxvc_decoder_parse_stream_header",
        )
        self.stream = self.stream_info()
        return self.stream, consumed.value

    def stream_info(self) -> StreamInfo:
        c = nxvc_stream_info()
        _ffi.check(
            self._lib.nxvc_decoder_stream_info(self._handle(), ctypes.byref(c)),
            "nxvc_decoder_stream_info",
        )
        return StreamInfo._from_c(c)

    def plane_size(self, plane: int) -> tuple[int, int]:
        """``(width, height)`` of *plane* for the current stream geometry."""
        w, h = ctypes.c_uint32(0), ctypes.c_uint32(0)
        _ffi.check(
            self._lib.nxvc_decoder_plane_size(
                self._handle(), int(plane), ctypes.byref(w), ctypes.byref(h)
            ),
            "nxvc_decoder_plane_size",
        )
        return int(w.value), int(h.value)

    @property
    def plane_count(self) -> int:
        if self.stream is None:
            raise RuntimeError("parse a stream header first")
        return 4 if self.stream.alpha else 3

    def allocate(self) -> Planes:
        """Allocate output planes matching the current stream geometry."""
        out: Planes = []
        for i in range(self.plane_count):
            w, h = self.plane_size(i)
            out.append(np.zeros((h, w), dtype=np.uint8))
        return out

    # ---------------------------------------------------------------- decode
    def scan(self, data: bytes) -> tuple[FrameInfo, int]:
        """Parse only the headers of the next frame (what ``nxv-info`` does).

        The reference decoder leaves ``tile_count`` at 0 here: nothing has
        walked the tile structures yet.  Use :meth:`decode` and then
        :meth:`frame_info` / :meth:`tiles` for per-tile records, or the
        pure-Python :func:`nxvc.bitstream.parse_frame` to count them without
        decoding pixels.
        """
        c = nxvc_frame_info()
        consumed = ctypes.c_size_t(0)
        buf = bytes(data)
        _ffi.check(
            self._lib.nxvc_decoder_scan_frame(
                self._handle(), _ffi.bytes_ptr(buf), len(buf), ctypes.byref(c),
                ctypes.byref(consumed),
            ),
            "nxvc_decoder_scan_frame",
        )
        return FrameInfo._from_c(c), consumed.value

    def decode(self, data: bytes, into: Planes | None = None) -> tuple[Planes, int]:
        """Decode the next frame.  Returns the planes and the bytes consumed."""
        planes = self.allocate() if into is None else into
        shapes = [(p.shape[0], p.shape[1]) for p in planes[: self.plane_count]]
        img, keep = _image_from_planes(planes, shapes)
        consumed = ctypes.c_size_t(0)
        buf = bytes(data)
        _ffi.check(
            self._lib.nxvc_decoder_decode_frame(
                self._handle(),
                _ffi.bytes_ptr(buf),
                len(buf),
                ctypes.byref(img),
                ctypes.byref(consumed),
            ),
            "nxvc_decoder_decode_frame",
        )
        return keep, consumed.value

    def frame_info(self) -> FrameInfo:
        c = nxvc_frame_info()
        _ffi.check(
            self._lib.nxvc_decoder_frame_info(self._handle(), ctypes.byref(c)),
            "nxvc_decoder_frame_info",
        )
        return FrameInfo._from_c(c)

    def tiles(self) -> list[TileInfo]:
        """Per-tile records of the most recently decoded frame."""
        return _tiles_from(self._lib.nxvc_decoder_tiles, self._handle())

    # ------------------------------------------------------------ convenience
    def frames(self, data: bytes) -> Iterator[Planes]:
        """Decode a whole ``.nxv`` buffer, yielding one plane list per frame."""
        _, pos = self.parse_stream_header(data)
        while pos < len(data):
            planes, used = self.decode(data[pos:])
            if used <= 0:
                raise NxvcError(Status.ERR_BITSTREAM, "frame consumed zero bytes")
            pos += used
            yield planes

    def scan_frames(self, data: bytes) -> Iterator[FrameInfo]:
        """Walk a ``.nxv`` buffer's frame headers without decoding pixels."""
        _, pos = self.parse_stream_header(data)
        while pos < len(data):
            info, used = self.scan(data[pos:])
            pos += used
            yield info


# ------------------------------------------------------------------- raw YUV


def read_planar_yuv(
    path, width: int, height: int, pix: str = "yuv420p", alpha: bool = False,
    frames: int | None = None,
) -> list[Planes]:
    """Read a headerless planar 8-bit YUV file as a list of frames.

    The same layout ``nxv-enc --in`` expects and ``nxv-dec --out`` writes, and
    the same one the quality harness's ``nxq.yuv`` uses.
    """
    chroma = _pix_to_chroma(pix)
    shapes = plane_shapes(width, height, chroma, alpha)
    frame_bytes = sum(h * w for h, w in shapes)
    out: list[Planes] = []
    with open(path, "rb") as fh:
        while frames is None or len(out) < frames:
            raw = fh.read(frame_bytes)
            if not raw:
                break
            if len(raw) != frame_bytes:
                raise ValueError(
                    f"{path}: short frame {len(out)}: {len(raw)} of {frame_bytes} bytes"
                )
            pos = 0
            planes: Planes = []
            for h, w in shapes:
                planes.append(
                    np.frombuffer(raw, dtype=np.uint8, count=h * w, offset=pos).reshape(h, w)
                )
                pos += h * w
            out.append(planes)
    return out


def write_planar_yuv(path, frames: Sequence[Planes]) -> int:
    """Write frames of planes back out as a headerless planar file."""
    n = 0
    with open(path, "wb") as fh:
        for planes in frames:
            for p in planes:
                fh.write(np.ascontiguousarray(p, dtype=np.uint8).tobytes())
            n += 1
    return n


# ------------------------------------------------------------------- YCoCg-R


def ycocgr_forward(r: np.ndarray, g: np.ndarray, b: np.ndarray):
    """Normative YCoCg-R forward transform (``nxvc_ycocgr_forward``).

    Returns ``(y, co, cg)`` with the chroma planes biased by +256 in uint16.
    """
    lib = _ffi.load()
    r = np.ascontiguousarray(r, dtype=np.uint8)
    g = np.ascontiguousarray(g, dtype=np.uint8)
    b = np.ascontiguousarray(b, dtype=np.uint8)
    if not (r.shape == g.shape == b.shape):
        raise ValueError("r, g and b must have the same shape")
    n = r.size
    y = np.zeros(r.shape, dtype=np.uint8)
    co = np.zeros(r.shape, dtype=np.uint16)
    cg = np.zeros(r.shape, dtype=np.uint16)
    lib.nxvc_ycocgr_forward(
        r.ctypes.data_as(_ffi.u8p),
        g.ctypes.data_as(_ffi.u8p),
        b.ctypes.data_as(_ffi.u8p),
        y.ctypes.data_as(_ffi.u8p),
        co.ctypes.data_as(_ffi.u16p),
        cg.ctypes.data_as(_ffi.u16p),
        n,
    )
    return y, co, cg


def ycocgr_inverse(y: np.ndarray, co: np.ndarray, cg: np.ndarray):
    """Normative YCoCg-R inverse transform (``nxvc_ycocgr_inverse``)."""
    lib = _ffi.load()
    y = np.ascontiguousarray(y, dtype=np.uint8)
    co = np.ascontiguousarray(co, dtype=np.uint16)
    cg = np.ascontiguousarray(cg, dtype=np.uint16)
    if not (y.shape == co.shape == cg.shape):
        raise ValueError("y, co and cg must have the same shape")
    r = np.zeros(y.shape, dtype=np.uint8)
    g = np.zeros(y.shape, dtype=np.uint8)
    b = np.zeros(y.shape, dtype=np.uint8)
    lib.nxvc_ycocgr_inverse(
        y.ctypes.data_as(_ffi.u8p),
        co.ctypes.data_as(_ffi.u16p),
        cg.ctypes.data_as(_ffi.u16p),
        r.ctypes.data_as(_ffi.u8p),
        g.ctypes.data_as(_ffi.u8p),
        b.ctypes.data_as(_ffi.u8p),
        y.size,
    )
    return r, g, b


# Re-export so `from nxvc.codec import LibraryNotFound` works.
__all__.append("LibraryNotFound")
__all__.append("NxvcError")
