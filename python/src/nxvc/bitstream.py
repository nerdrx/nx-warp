"""Pure-Python parser for the NX Warp v1 bitstream headers.

This module implements `docs/SYNTAX.md` sections 2, 3 and 4 -- the stream
header and its TLV area, the frame header, the tile-row header and the tile
header -- with **no dependency on the C library and no dependency on numpy**.
It exists so that a `.nxv` file can be inspected, validated structurally and
diffed anywhere, including on a machine where the codec is not built.

It deliberately stops at the entropy-coded payload: tile payloads are sliced
out as bytes and never decoded.  Decoding pixels is the C library's job and the
reference codec is the normative implementation; nothing here may be treated as
a second opinion about pixel output.

Every header structure is **table-driven**: the byte layout lives in a
``FIELDS`` list of ``(name, offset, format)`` and both :meth:`parse` and
:meth:`pack` are generated from it, with the undeclared gaps carried through
verbatim as ``reserved``.  Adding a field the specification grows -- e.g. a
``color_space`` byte in the stream header's reserved area -- is one line in
that table.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Any, Callable, ClassVar, Iterator, Sequence

from ._ffi import NXVC_MAGIC, ColorSpace, Status, TileMode, Tool

__all__ = [
    "BitstreamError",
    "FrameSection",
    "FRAME_SECTIONS",
    "table_set_bytes",
    "Tlv",
    "StreamHeader",
    "FrameHeader",
    "TileRowHeader",
    "TileHeader",
    "Tile",
    "TileRow",
    "Frame",
    "Stream",
    "parse_stream",
    "parse_stream_header",
    "parse_frame",
    "iter_frames",
]


class BitstreamError(ValueError):
    """A structural problem in a bitstream, with the byte offset it was found at."""

    def __init__(self, message: str, offset: int | None = None) -> None:
        self.offset = offset
        if offset is None:
            super().__init__(message)
        else:
            super().__init__(f"at byte {offset}: {message}")


def _need(buf: bytes, offset: int, count: int, what: str) -> None:
    if offset < 0 or offset + count > len(buf):
        raise BitstreamError(
            f"truncated {what}: need {count} bytes, {max(0, len(buf) - offset)} available",
            offset,
        )


# --------------------------------------------------------------- table engine


class _TableStruct:
    """Mixin driving parse/pack off a ``FIELDS`` table.

    ``FIELDS`` is a list of ``(attribute, byte_offset, struct_format)``.  The
    format is a little-endian ``struct`` format without the ``<``; a format
    with a repeat count (``"26s"``, ``"4I"``) yields bytes or a tuple.  Byte
    ranges inside ``SIZE`` that no field claims are gaps: they are captured
    into ``reserved`` on parse and written back on pack, so an unknown
    reserved byte survives a round trip instead of being zeroed.
    """

    FIELDS: ClassVar[Sequence[tuple[str, int, str]]] = ()
    SIZE: ClassVar[int] = 0

    @classmethod
    def _gaps(cls) -> list[tuple[int, int]]:
        claimed = bytearray(cls.SIZE)
        for _, off, fmt in cls.FIELDS:
            n = struct.calcsize("<" + fmt)
            claimed[off : off + n] = b"\x01" * n
        gaps: list[tuple[int, int]] = []
        start = None
        for i in range(cls.SIZE):
            if not claimed[i] and start is None:
                start = i
            elif claimed[i] and start is not None:
                gaps.append((start, i - start))
                start = None
        if start is not None:
            gaps.append((start, cls.SIZE - start))
        return gaps

    @classmethod
    def _parse_at(cls, buf: bytes, offset: int, what: str) -> dict[str, Any]:
        _need(buf, offset, cls.SIZE, what)
        values: dict[str, Any] = {}
        for name, off, fmt in cls.FIELDS:
            got = struct.unpack_from("<" + fmt, buf, offset + off)
            values[name] = got[0] if len(got) == 1 else got
        reserved = b"".join(
            bytes(buf[offset + s : offset + s + n]) for s, n in cls._gaps()
        )
        values["reserved"] = reserved
        return values

    def pack(self) -> bytes:
        """Serialize back to exactly ``SIZE`` bytes."""
        out = bytearray(self.SIZE)
        for name, off, fmt in self.FIELDS:
            value = getattr(self, name)
            if isinstance(value, (tuple, list)):
                struct.pack_into("<" + fmt, out, off, *value)
            else:
                struct.pack_into("<" + fmt, out, off, value)
        reserved = getattr(self, "reserved", b"") or b""
        pos = 0
        for start, n in self._gaps():
            chunk = reserved[pos : pos + n]
            out[start : start + len(chunk)] = chunk
            pos += n
        return bytes(out)


# ------------------------------------------------------------------- TLV area


@dataclass(frozen=True)
class Tlv:
    """One record of the stream header's extension area (SYNTAX.md 2.1)."""

    type: int
    payload: bytes

    @property
    def length(self) -> int:
        return len(self.payload)

    @property
    def private(self) -> bool:
        """Types 0x8000-0xFFFF are private and never mandatory."""
        return self.type >= 0x8000

    @property
    def total_size(self) -> int:
        """Bytes on the wire including the 4-byte prefix and the zero padding."""
        return 4 + len(self.payload) + ((4 - (len(self.payload) & 3)) & 3)

    def pack(self) -> bytes:
        pad = (4 - (len(self.payload) & 3)) & 3
        return struct.pack("<HH", self.type, len(self.payload)) + self.payload + b"\x00" * pad


def _parse_tlvs(buf: bytes, offset: int, ext_len: int) -> list[Tlv]:
    _need(buf, offset, ext_len, "TLV extension area")
    end = offset + ext_len
    pos = offset
    out: list[Tlv] = []
    while pos < end:
        if pos + 4 > end:
            raise BitstreamError("TLV header runs past the extension area", pos)
        typ, length = struct.unpack_from("<HH", buf, pos)
        pad = (4 - (length & 3)) & 3
        total = 4 + length + pad
        if pos + total > end:
            raise BitstreamError(
                f"TLV type 0x{typ:04x} length {length} runs past the extension area", pos
            )
        if pad and buf[pos + 4 + length : pos + total] != b"\x00" * pad:
            raise BitstreamError(f"TLV type 0x{typ:04x} has non-zero padding", pos)
        out.append(Tlv(typ, bytes(buf[pos + 4 : pos + 4 + length])))
        pos += total
    return out


# --------------------------------------------------------------- stream header


@dataclass
class StreamHeader(_TableStruct):
    """The 64-byte stream header plus its TLV area (SYNTAX.md 2)."""

    magic: int = NXVC_MAGIC
    version: int = 1
    profile: int = 0
    level: int = 0
    tile_size_code: int = 0
    width: int = 0
    height: int = 0
    eyes: int = 1
    bit_depth: int = 8
    num_layers: int = 1
    chroma_format: int = 0
    layer_desc: tuple[int, int, int, int] = (0, 0, 0, 0)
    tools: int = 0
    alpha_present: int = 0
    color_transform: int = 0
    color_space: int = 0
    ext_len: int = 0
    reserved: bytes = b""
    tlvs: list[Tlv] = field(default_factory=list)

    SIZE: ClassVar[int] = 64
    FIELDS: ClassVar[Sequence[tuple[str, int, str]]] = (
        ("magic", 0, "I"),
        ("version", 4, "B"),
        ("profile", 5, "B"),
        ("level", 6, "B"),
        ("tile_size_code", 7, "B"),
        ("width", 8, "H"),
        ("height", 10, "H"),
        ("eyes", 12, "B"),
        ("bit_depth", 13, "B"),
        ("num_layers", 14, "B"),
        ("chroma_format", 15, "B"),
        ("layer_desc", 16, "4I"),
        ("tools", 32, "Q"),
        ("alpha_present", 40, "B"),
        ("color_transform", 41, "B"),
        ("color_space", 42, "B"),
        # Bytes 43-61 are reserved and must be zero.  A field the spec grows
        # into that area is one more line in this table and nothing else: the
        # gap, the parser, the packer and `reserved` all follow from it.
        ("ext_len", 62, "H"),
    )

    # ------------------------------------------------------------ derived
    @property
    def tile_size(self) -> int:
        """Tile edge in luma samples: 64, or 32 when bit 0 of ``tile_size_code`` is set."""
        return 32 if (self.tile_size_code & 1) else 64

    @property
    def tiles_x(self) -> int:
        t = self.tile_size
        return (self.width + t - 1) // t

    @property
    def tiles_y(self) -> int:
        t = self.tile_size
        return (self.height + t - 1) // t

    @property
    def tile_count(self) -> int:
        return self.tiles_x * self.tiles_y

    @property
    def chroma444(self) -> bool:
        return self.chroma_format == 1

    @property
    def color_space_name(self) -> str:
        """Human name of the descriptive ``color_space`` field (SYNTAX.md 2.2)."""
        return ColorSpace.name(self.color_space)

    @property
    def total_size(self) -> int:
        """Bytes on the wire: the fixed header plus the TLV area."""
        return self.SIZE + self.ext_len

    def tool_names(self) -> list[str]:
        return Tool.names(self.tools)

    def unsupported_tools(self) -> int:
        """Tool bits outside what the reference decoder implements."""
        from ._ffi import TOOLS_SUPPORTED

        return self.tools & ~TOOLS_SUPPORTED

    def non_phase1_tools(self) -> int:
        """Tool bits outside the Phase 1 (intra-only) subset.

        Distinct from :meth:`unsupported_tools` since syntax v1.4: the
        reference decoder implements the inter tools, an intra-only decoder
        does not.
        """
        from ._ffi import TOOLS_PHASE1

        return self.tools & ~TOOLS_PHASE1

    # -------------------------------------------------------------- codec
    @classmethod
    def parse(cls, buf: bytes, offset: int = 0, *, validate: bool = True) -> "StreamHeader":
        values = cls._parse_at(buf, offset, "stream header")
        hdr = cls(**values)
        # Validate the fixed header BEFORE walking the extension area: on junk
        # input `ext_len` is junk too, and "bad magic" is the useful message,
        # not "truncated TLV area of 28001 bytes".
        if validate:
            hdr.validate(offset)
        hdr.tlvs = _parse_tlvs(buf, offset + cls.SIZE, hdr.ext_len)
        return hdr

    def pack(self) -> bytes:
        """The 64-byte header followed by the packed TLV area.

        ``ext_len`` is recomputed from ``tlvs`` when any TLV is present, so a
        hand-built header cannot disagree with its own extension area.
        """
        if self.tlvs:
            self.ext_len = sum(t.total_size for t in self.tlvs)
        return super().pack() + b"".join(t.pack() for t in self.tlvs)

    def validate(self, offset: int = 0) -> None:
        """Apply the constraints SYNTAX.md 2 requires a decoder to check."""
        if self.magic != NXVC_MAGIC:
            raise BitstreamError(
                f"bad magic 0x{self.magic:08x}, expected 0x{NXVC_MAGIC:08x} ('NXV1')", offset
            )
        if self.version != 1:
            raise BitstreamError(f"unsupported version {self.version}", offset)
        if self.tile_size_code & ~1:
            raise BitstreamError(
                f"tile_size bits 1-7 must be zero, got 0x{self.tile_size_code:02x}", offset + 7
            )
        for name, value in (("width", self.width), ("height", self.height)):
            if not (16 <= value <= 4096):
                raise BitstreamError(f"{name} {value} outside [16, 4096]", offset)
            if value & 1:
                raise BitstreamError(f"{name} {value} is not even", offset)
        if self.tiles_x > 64:
            raise BitstreamError(
                f"{self.tiles_x} tiles per row exceeds the 64-bit skip bitmap", offset
            )
        if self.chroma_format not in (0, 1):
            raise BitstreamError(f"chroma_format {self.chroma_format} out of range", offset + 15)
        if self.color_transform not in (0, 1):
            raise BitstreamError(
                f"color_transform {self.color_transform} out of range", offset + 41
            )
        if self.alpha_present not in (0, 1):
            raise BitstreamError(f"alpha_present {self.alpha_present} out of range", offset + 40)
        if self.color_transform == 1 and self.chroma_format != 1:
            raise BitstreamError("YCoCg-R requires 4:4:4 chroma", offset)
        if self.color_space > 3:
            raise BitstreamError(f"color_space {self.color_space} out of range", offset + 42)
        # SYNTAX.md 2: color_space == 3 (RGB) if and only if YCoCg-R is in use.
        if (self.color_space == ColorSpace.RGB) != (self.color_transform == 1):
            raise BitstreamError(
                f"color_space {ColorSpace.name(self.color_space)!r} and "
                f"color_transform {self.color_transform} disagree: RGB requires "
                f"YCoCg-R and YCoCg-R requires RGB",
                offset + 42,
            )
        if self.eyes not in (1, 2):
            raise BitstreamError(f"eyes {self.eyes} must be 1 or 2", offset + 12)
        if self.bit_depth not in (8, 10):
            raise BitstreamError(f"bit_depth {self.bit_depth} must be 8 or 10", offset + 13)
        if not (1 <= self.num_layers <= 4):
            raise BitstreamError(f"num_layers {self.num_layers} outside [1, 4]", offset + 14)
        for i in range(self.num_layers, 4):
            if self.layer_desc[i]:
                raise BitstreamError(
                    f"layer_desc[{i}] must be zero above num_layers", offset + 16 + 4 * i
                )
        if self.tools >> Tool.RESERVED_FROM:
            raise BitstreamError(
                f"reserved tool bits set: 0x{self.tools:016x}", offset + 32
            )
        # SYNTAX.md 2.3: hiding a sign spends one level step, so the two cannot
        # both be true (rejection vector r17).
        if (self.tools & Tool.LOSSLESS) and (self.tools & Tool.SIGN_HIDE):
            raise BitstreamError(
                "tool bits LOSSLESS and SIGN_HIDE are mutually exclusive", offset + 32
            )
        if any(self.reserved):
            raise BitstreamError("reserved bytes 43-61 must be zero", offset + 43)


# ---------------------------------------------------------------- frame header


@dataclass
class FrameHeader(_TableStruct):
    """The 40-byte frame header (SYNTAX.md 3.1)."""

    frame_number: int = 0
    pose: bytes = b"\x00" * 26
    base_qp: int = 0
    chroma_qp_off: int = 0
    alpha_qp_off: int = 0
    quant_matrix: int = 0
    tables_present: int = 0
    ref_slots: int = 0
    flags: int = 0
    frame_bytes: int = 40
    reserved: bytes = b""

    SIZE: ClassVar[int] = 40
    FIELDS: ClassVar[Sequence[tuple[str, int, str]]] = (
        ("frame_number", 0, "H"),
        ("pose", 2, "26s"),
        ("base_qp", 28, "B"),
        ("chroma_qp_off", 29, "b"),
        ("alpha_qp_off", 30, "b"),
        ("quant_matrix", 31, "B"),
        ("tables_present", 32, "B"),
        ("ref_slots", 33, "B"),
        ("flags", 34, "B"),
        ("frame_bytes", 36, "I"),
    )

    #: bit 0 of ``flags``
    @property
    def tile_map_reset(self) -> bool:
        return bool(self.flags & 1)

    #: bit 1 of ``flags``
    @property
    def stereo_inter_view(self) -> bool:
        return bool(self.flags & 2)

    #: bit 2 of ``flags``: the layered form of directional intra (SYNTAX.md 7.5).
    #: Requires tool bit 17 ``INTRA_DIR``; the pairing is checked in
    #: :func:`parse_frame`, which is the first place the stream header is in hand.
    @property
    def intra_dir_layer(self) -> bool:
        return bool(self.flags & 4)

    @property
    def custom_matrix(self) -> bool:
        return self.quant_matrix == 255

    @property
    def table_sets(self) -> list[int]:
        """Indices of the probability table sets transmitted with this frame."""
        return [k for k in range(8) if self.tables_present & (1 << k)]

    def decode_pose(self) -> dict[str, tuple[float, ...]]:
        """Interpret the 26 opaque pose bytes as SYNTAX.md 3.2 suggests.

        The codec never does this -- pose is carried byte-wise and the decode
        path does no floating point.  This is for humans reading a stream.
        """
        quat = struct.unpack_from("<4e", self.pose, 0)
        angvel = struct.unpack_from("<3e", self.pose, 8)
        pos = struct.unpack_from("<3f", self.pose, 14)
        return {"quat": quat, "angvel": angvel, "pos": pos}

    @classmethod
    def parse(cls, buf: bytes, offset: int = 0, *, validate: bool = True) -> "FrameHeader":
        hdr = cls(**cls._parse_at(buf, offset, "frame header"))
        if validate:
            hdr.validate(offset)
        return hdr

    def validate(self, offset: int = 0) -> None:
        if self.base_qp > 63:
            raise BitstreamError(f"base_qp {self.base_qp} exceeds 63", offset + 28)
        if not (self.quant_matrix <= 3 or self.quant_matrix == 255):
            raise BitstreamError(
                f"quant_matrix {self.quant_matrix} must be 0..3 or 255", offset + 31
            )
        if self.frame_bytes < self.SIZE:
            raise BitstreamError(
                f"frame_bytes {self.frame_bytes} is smaller than the 40-byte header", offset + 36
            )
        if any(self.reserved):
            raise BitstreamError("frame header byte 35 must be zero", offset + 35)


# ------------------------------------------------------------- tile-row header


@dataclass
class TileRowHeader(_TableStruct):
    """The 12-byte tile-row header (SYNTAX.md 3.3)."""

    frame_number: int = 0
    row_index: int = 0
    tile_count: int = 0
    skip_bitmap: int = 0

    SIZE: ClassVar[int] = 12
    FIELDS: ClassVar[Sequence[tuple[str, int, str]]] = (
        ("frame_number", 0, "H"),
        ("row_index", 2, "B"),
        ("tile_count", 3, "B"),
        ("skip_bitmap", 4, "Q"),
    )

    reserved: bytes = b""

    def skipped_tiles(self, tiles_in_row: int) -> list[int]:
        return [i for i in range(tiles_in_row) if self.skip_bitmap & (1 << i)]

    @classmethod
    def parse(cls, buf: bytes, offset: int = 0) -> "TileRowHeader":
        return cls(**cls._parse_at(buf, offset, "tile-row header"))


# ----------------------------------------------------------------- tile header

#: word1 bitfields: (name, lsb, width, signed)
_TILE_WORD1: Sequence[tuple[str, int, int, bool]] = (
    ("mode", 0, 3, False),
    ("res_level", 3, 2, False),
    ("chroma444", 5, 1, False),
    ("alpha_mode", 6, 2, False),
    ("qp_delta", 8, 6, True),
    ("table_set", 14, 3, False),
    ("nsub_log2", 17, 3, False),
    ("mv_present", 20, 1, False),
    ("ref_sel", 21, 2, False),
    ("tskip", 23, 1, False),
    ("wgt", 24, 2, False),
    ("wm_id", 26, 2, False),
    ("word1_reserved", 28, 4, False),
)

#: word0 bitfields: (name, lsb, width, signed)
_TILE_WORD0: Sequence[tuple[str, int, int, bool]] = (
    ("layer", 0, 2, False),
    ("eye", 2, 1, False),
    ("word0_reserved", 3, 1, False),
    ("tile_index", 4, 12, False),
    ("payload_len", 16, 16, False),
)


#: Optional bytes that follow the fixed 8-byte tile header, in wire order:
#: ``(description, attribute names, struct format, present-predicate)``.  A
#: Phase 2 field (SYNTAX.md 4.1's warp parameters) is one more row here and
#: nothing else -- :attr:`TileHeader.header_size`, ``parse`` and ``pack`` are
#: all generated from this table.
_TILE_EXTRAS: Sequence[tuple[str, tuple[str, ...], str, Callable[[Any], bool]]] = (
    ("tile motion vector", ("mv_x", "mv_y"), "bb", lambda h: bool(h.mv_present)),
    ("tile constant alpha", ("alpha_value",), "B", lambda h: h.alpha_mode == 1),
)


def _get_bits(word: int, lsb: int, width: int, signed: bool) -> int:
    v = (word >> lsb) & ((1 << width) - 1)
    if signed and v & (1 << (width - 1)):
        v -= 1 << width
    return v


def _set_bits(word: int, lsb: int, width: int, value: int) -> int:
    mask = (1 << width) - 1
    return word | ((int(value) & mask) << lsb)


@dataclass
class TileHeader:
    """The 8-byte tile header and its optional MV / constant-alpha bytes (SYNTAX.md 4.1)."""

    layer: int = 0
    eye: int = 0
    tile_index: int = 0
    payload_len: int = 0
    mode: int = TileMode.INTRA
    res_level: int = 0
    chroma444: int = 0
    alpha_mode: int = 0
    qp_delta: int = 0
    table_set: int = 0
    nsub_log2: int = 3
    mv_present: int = 0
    ref_sel: int = 0
    tskip: int = 0
    wgt: int = 0
    wm_id: int = 0
    word0_reserved: int = 0
    word1_reserved: int = 0
    mv_x: int = 0
    mv_y: int = 0
    alpha_value: int = 0

    #: Fixed part only; the optional bytes are counted by :attr:`header_size`.
    SIZE: ClassVar[int] = 8

    @property
    def header_size(self) -> int:
        """Header bytes before the payload: 8, plus 2 for an MV, plus 1 for constant alpha."""
        return 8 + sum(
            struct.calcsize("<" + fmt)
            for _, _, fmt, present in _TILE_EXTRAS
            if present(self)
        )

    @property
    def total_size(self) -> int:
        return self.header_size + self.payload_len

    @property
    def mode_name(self) -> str:
        return TileMode.name(self.mode)

    @property
    def coded_size(self) -> int:
        """Luma edge actually coded for this tile, 64 >> res_level."""
        return 64 >> self.res_level

    def chroma_coded_size(self, stream_chroma444: bool) -> int:
        """Chroma edge coded for this tile (SYNTAX.md 4.2)."""
        full = 64 if (stream_chroma444 and self.chroma444) else 32
        return full >> self.res_level

    def resolved_qp(self, base_qp: int) -> int:
        """Luma QP after ``qp_delta``, clamped to the legal 0..63 range."""
        return max(0, min(63, base_qp + self.qp_delta))

    @classmethod
    def parse(cls, buf: bytes, offset: int = 0, *, validate: bool = True) -> "TileHeader":
        _need(buf, offset, 8, "tile header")
        word0, word1 = struct.unpack_from("<II", buf, offset)
        values: dict[str, int] = {}
        for name, lsb, width, signed in _TILE_WORD0:
            values[name] = _get_bits(word0, lsb, width, signed)
        for name, lsb, width, signed in _TILE_WORD1:
            values[name] = _get_bits(word1, lsb, width, signed)
        hdr = cls(**values)
        pos = offset + 8
        for what, names, fmt, present in _TILE_EXTRAS:
            if not present(hdr):
                continue
            n = struct.calcsize("<" + fmt)
            _need(buf, pos, n, what)
            for name, value in zip(names, struct.unpack_from("<" + fmt, buf, pos)):
                setattr(hdr, name, int(value))
            pos += n
        if validate:
            hdr.validate(offset)
        return hdr

    def pack(self) -> bytes:
        word0 = 0
        for name, lsb, width, _ in _TILE_WORD0:
            word0 = _set_bits(word0, lsb, width, getattr(self, name))
        word1 = 0
        for name, lsb, width, _ in _TILE_WORD1:
            word1 = _set_bits(word1, lsb, width, getattr(self, name))
        out = struct.pack("<II", word0, word1)
        for _, names, fmt, present in _TILE_EXTRAS:
            if present(self):
                out += struct.pack("<" + fmt, *(getattr(self, n) for n in names))
        return out

    def validate(self, offset: int = 0) -> None:
        if self.mode > 4:
            raise BitstreamError(f"mode {self.mode} is reserved", offset + 4)
        if self.res_level == 3:
            raise BitstreamError("res_level 3 is reserved", offset + 4)
        if self.alpha_mode == 3:
            raise BitstreamError("alpha_mode 3 is reserved", offset + 4)
        if self.nsub_log2 > 5:
            raise BitstreamError(f"nsub_log2 {self.nsub_log2} exceeds 5", offset + 4)
        if self.word0_reserved:
            raise BitstreamError("tile word0 bit 3 must be zero", offset)
        if self.word1_reserved:
            raise BitstreamError("tile word1 bits 28-31 must be zero", offset + 4)


# ------------------------------------------------------------- parsed entities


@dataclass
class Tile:
    """A tile header with its payload sliced out, and where it was found."""

    header: TileHeader
    payload: bytes
    offset: int

    @property
    def size(self) -> int:
        return self.header.total_size


@dataclass
class TileRow:
    header: TileRowHeader
    tiles: list[Tile]
    offset: int

    @property
    def size(self) -> int:
        return TileRowHeader.SIZE + sum(t.size for t in self.tiles)


@dataclass
class Frame:
    """One self-delimiting frame unit (SYNTAX.md 3)."""

    header: FrameHeader
    rows: list[TileRow]
    offset: int
    custom_matrix: bytes | None = None
    table_deltas: dict[int, bytes] = field(default_factory=dict)
    #: Every :data:`FRAME_SECTIONS` block that was present, by section name:
    #: ``{name: {key: bytes}}``.  ``custom_matrix`` and ``table_deltas`` above
    #: are the two named views onto it that v1 callers already use.
    sections: dict[str, dict[Any, bytes]] = field(default_factory=dict)

    @property
    def size(self) -> int:
        return self.header.frame_bytes

    @property
    def tiles(self) -> list[Tile]:
        """Every transmitted tile of the frame, in raster order."""
        return [t for row in self.rows for t in row.tiles]

    @property
    def payload_bytes(self) -> int:
        return sum(t.header.payload_len for t in self.tiles)


@dataclass
class Stream:
    """A whole ``.nxv`` file: one stream header followed by frames."""

    header: StreamHeader
    frames: list[Frame]

    @property
    def size(self) -> int:
        return self.header.total_size + sum(f.size for f in self.frames)


# ------------------------------------------------------------------ functions

#: Bytes of quantization matrix that follow a frame header with quant_matrix 255.
CUSTOM_MATRIX_BYTES = 128
#: Bytes of probability-table deltas per transmitted table set, v1 context model
#: (12 contexts x 16 symbols x 5 bits).
TABLE_SET_BYTES = 120
#: ... and under ``CTX_V2`` (16 x 16 x 5).  SYNTAX.md 3.1 and 9.4.
TABLE_SET_BYTES_V2 = 160


def table_set_bytes(tools: int) -> int:
    """Size of one transmitted probability table set for a stream's ``tools``.

    The stream header's tool mask is what selects it, so a frame cannot be
    parsed without it -- this is why :func:`parse_frame` takes the stream
    header rather than only the frame's own bytes.
    """
    return TABLE_SET_BYTES_V2 if (tools & Tool.CTX_V2) else TABLE_SET_BYTES


@dataclass(frozen=True)
class FrameSection:
    """One block of bytes between the frame header and the first tile row.

    ``keys`` returns the instances of the section present in this frame -- an
    empty list when it is absent, ``[None]`` for a singleton, one entry per
    repetition otherwise -- and ``size`` its byte length.  Both see the frame
    header and the stream header, which is everything that gates a section in
    SYNTAX.md 3.1.
    """

    name: str
    keys: Callable[["FrameHeader", "StreamHeader"], Sequence[Any]]
    size: Callable[["FrameHeader", "StreamHeader", Any], int]
    what: Callable[[Any], str]
    #: True when the section holds at most one instance, keyed ``None``.
    singleton: bool = True


#: The sections, **in wire order** (SYNTAX.md 3.1).  Phase 2's `warp_ext`, which
#: follows the frame header when a `flags` bit is set, is one more row here:
#: :func:`parse_frame` walks this table and nothing in it is hard-coded.
FRAME_SECTIONS: Sequence[FrameSection] = (
    FrameSection(
        name="custom_matrix",
        keys=lambda h, s: [None] if h.custom_matrix else [],
        size=lambda h, s, k: CUSTOM_MATRIX_BYTES,
        what=lambda k: "custom quantization matrices",
    ),
    FrameSection(
        name="table_deltas",
        keys=lambda h, s: h.table_sets,
        size=lambda h, s, k: table_set_bytes(s.tools),
        what=lambda k: f"probability table set {k}",
        singleton=False,
    ),
)


#: Cross-structure rules: a field of one header whose legality is decided by
#: another.  Each entry is ``(predicate, message)`` and each is a `BITSTREAM`
#: error, pinned by a rejection vector in ``tests/vectors``.
_FRAME_RULES: Sequence[tuple[Callable[[Any, Any], bool], str]] = (
    (
        lambda h, s: h.intra_dir_layer and not (s.tools & Tool.INTRA_DIR),
        "frame flags bit 2 (layered directional intra) without tool bit 17 INTRA_DIR",
    ),
)

_TILE_RULES: Sequence[tuple[Callable[[Any, Any, Any], bool], str]] = (
    (
        lambda t, h, s: t.wm_id != 0 and not (s.tools & Tool.WM_ID),
        "wm_id != 0 without tool bit 20 WM_ID",
    ),
    (
        lambda t, h, s: t.wm_id != 0 and h.quant_matrix == 255,
        "wm_id != 0 in a frame carrying custom quantization matrices",
    ),
    (
        lambda t, h, s: t.chroma444 and not s.chroma444,
        "chroma444 tile in a 4:2:0 stream",
    ),
    (
        lambda t, h, s: t.alpha_mode != 0 and not s.alpha_present,
        "alpha_mode != 0 in a stream with no alpha plane",
    ),
)


def _validate_frame_against_stream(
    hdr: "FrameHeader", stream: "StreamHeader", offset: int
) -> None:
    for predicate, message in _FRAME_RULES:
        if predicate(hdr, stream):
            raise BitstreamError(message, offset + 34)


def _validate_tile_against_stream(
    tile: "TileHeader", hdr: "FrameHeader", stream: "StreamHeader", offset: int
) -> None:
    for predicate, message in _TILE_RULES:
        if predicate(tile, hdr, stream):
            raise BitstreamError(f"tile {tile.tile_index}: {message}", offset + 4)


def parse_stream_header(buf: bytes, offset: int = 0, *, validate: bool = True) -> StreamHeader:
    """Parse the 64-byte stream header and its TLV area."""
    return StreamHeader.parse(buf, offset, validate=validate)


def parse_frame(
    buf: bytes, offset: int, stream: StreamHeader, *, validate: bool = True
) -> Frame:
    """Parse one frame unit starting at *offset*.

    *stream* supplies the geometry (tile size, rows per frame) the frame header
    does not repeat.
    """
    hdr = FrameHeader.parse(buf, offset, validate=validate)
    _need(buf, offset, hdr.frame_bytes, "frame unit")
    end = offset + hdr.frame_bytes
    pos = offset + FrameHeader.SIZE

    if validate:
        _validate_frame_against_stream(hdr, stream, offset)

    sections: dict[str, dict[Any, bytes]] = {}
    for section in FRAME_SECTIONS:
        for key in section.keys(hdr, stream):
            n = section.size(hdr, stream, key)
            what = section.what(key)
            _need(buf, pos, n, what)
            if pos + n > end:
                raise BitstreamError(
                    f"{what} runs past frame_bytes {hdr.frame_bytes}", pos
                )
            sections.setdefault(section.name, {})[key] = bytes(buf[pos : pos + n])
            pos += n

    custom_matrix = sections.get("custom_matrix", {}).get(None)
    table_deltas = dict(sections.get("table_deltas", {}))

    rows: list[TileRow] = []
    for row_index in range(stream.tiles_y):
        row_off = pos
        if pos + TileRowHeader.SIZE > end:
            raise BitstreamError(
                f"tile row {row_index} runs past frame_bytes {hdr.frame_bytes}", pos
            )
        rh = TileRowHeader.parse(buf, pos)
        pos += TileRowHeader.SIZE
        if validate:
            if rh.frame_number != hdr.frame_number:
                raise BitstreamError(
                    f"tile-row frame_number {rh.frame_number} != frame header "
                    f"{hdr.frame_number}",
                    row_off,
                )
            if rh.row_index != row_index:
                raise BitstreamError(
                    f"tile-row row_index {rh.row_index} != {row_index}", row_off + 2
                )
            skipped = bin(rh.skip_bitmap & ((1 << stream.tiles_x) - 1)).count("1")
            if rh.tile_count != stream.tiles_x - skipped:
                raise BitstreamError(
                    f"tile_count {rh.tile_count} != {stream.tiles_x} tiles minus "
                    f"{skipped} skipped",
                    row_off + 3,
                )
            if rh.skip_bitmap >> stream.tiles_x:
                raise BitstreamError(
                    f"skip_bitmap has bits set above tile {stream.tiles_x - 1}", row_off + 4
                )

        tiles: list[Tile] = []
        for _ in range(rh.tile_count):
            th = TileHeader.parse(buf, pos, validate=validate)
            if validate:
                _validate_tile_against_stream(th, hdr, stream, pos)
            if pos + th.total_size > end:
                raise BitstreamError(
                    f"tile {th.tile_index} of row {row_index} runs past frame_bytes", pos
                )
            payload_off = pos + th.header_size
            tiles.append(
                Tile(th, bytes(buf[payload_off : payload_off + th.payload_len]), pos)
            )
            pos += th.total_size
        rows.append(TileRow(rh, tiles, row_off))

    if validate and pos != end:
        raise BitstreamError(
            f"frame consumed {pos - offset} bytes, frame_bytes says {hdr.frame_bytes}", pos
        )
    return Frame(hdr, rows, offset, custom_matrix, table_deltas, sections)


def iter_frames(
    buf: bytes, stream: StreamHeader, offset: int | None = None, *, validate: bool = True
) -> Iterator[Frame]:
    """Walk the frames of *buf*, starting after the stream header by default."""
    pos = stream.total_size if offset is None else offset
    while pos < len(buf):
        frame = parse_frame(buf, pos, stream, validate=validate)
        yield frame
        pos += frame.header.frame_bytes


def parse_stream(buf: bytes, *, validate: bool = True, max_frames: int | None = None) -> Stream:
    """Parse a whole ``.nxv`` buffer into a :class:`Stream`."""
    header = parse_stream_header(buf, 0, validate=validate)
    frames: list[Frame] = []
    for frame in iter_frames(buf, header, validate=validate):
        frames.append(frame)
        if max_frames is not None and len(frames) >= max_frames:
            break
    return Stream(header, frames)


def phase1_reject_reason(stream: StreamHeader, frame: Frame | None = None) -> str | None:
    """Why a Phase 1 (intra-only) decoder must refuse this stream, or None.

    Mirrors SYNTAX.md 12.  Purely advisory: the reference decoder's status is
    the authority, this only explains it without linking against the codec.
    """
    unsupported = stream.non_phase1_tools()
    if unsupported:
        return f"tool bits outside the Phase 1 set: {Tool.names(unsupported)}"
    if stream.eyes != 1:
        return f"eyes == {stream.eyes}, Phase 1 is monoscopic"
    if stream.num_layers != 1:
        return f"num_layers == {stream.num_layers}, Phase 1 is single-layer"
    if stream.bit_depth != 8:
        return f"bit_depth == {stream.bit_depth}, Phase 1 is 8-bit"
    if frame is None:
        return None
    for row in frame.rows:
        if row.header.skip_bitmap:
            return "non-zero skip_bitmap: a skip references a frame Phase 1 cannot have"
    for tile in frame.tiles:
        if tile.header.mode != TileMode.INTRA:
            return f"tile {tile.header.tile_index} is {tile.header.mode_name}, not INTRA"
        if tile.header.layer != 0 or tile.header.eye != 0:
            return f"tile {tile.header.tile_index} has layer/eye != 0"
    return None


#: Re-exported for callers that want to name a status without importing _ffi.
STATUS = Status
