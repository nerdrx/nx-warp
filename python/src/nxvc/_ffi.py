"""ctypes binding for the NX Warp reference codec C ABI (``include/nxvc/nxvc.h``).

Everything that touches the C library lives in this one module: the structure
definitions, the enums, the prototypes and the library search.  If the C ABI
moves, this is the only file that has to move with it.

Nothing here imports numpy or any other third-party package, and importing this
module never raises because the library is missing -- call :func:`load` (or
touch :data:`lib`) to find out.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import re
import sys
from ctypes import (
    POINTER,
    Structure,
    c_char_p,
    c_double,
    c_int,
    c_int8,
    c_int32,
    c_size_t,
    c_uint8,
    c_uint16,
    c_uint32,
    c_uint64,
    c_void_p,
)
from pathlib import Path
from typing import Iterator

__all__ = [
    "NXVC_VERSION",
    "NXVC_BITSTREAM_MINOR",
    "NXVC_TILE_SIZE",
    "NXVC_MAX_TILES_PER_ROW",
    "Status",
    "Chroma",
    "ColorTransform",
    "ColorSpace",
    "TileMode",
    "Tool",
    "TOOLS_SUPPORTED",
    "TOOLS_PHASE1",
    "nxvc_image",
    "nxvc_view",
    "nxvc_config",
    "nxvc_encode_stats",
    "nxvc_tile_layout",
    "nxvc_tile_info",
    "nxvc_stream_info",
    "nxvc_frame_info",
    "LibraryNotFound",
    "NxvcError",
    "load",
    "is_available",
    "library_path",
    "load_error",
    "search_paths",
    "check",
    "status_string",
    "version_string",
    "library_minor",
]

# --------------------------------------------------------------------- consts

NXVC_VERSION = 1
NXVC_MAX_TILES_PER_ROW = 64
NXVC_TILE_SIZE = 64

#: The revision of ``docs/SYNTAX.md`` that :mod:`nxvc.bitstream` parses.  It is
#: not carried in the bitstream (forward compatibility is the ``tools`` mask
#: plus the TLV area); it exists so a build, a conformance-vector set and a
#: spec revision can be pinned to each other.  4 = the Phase 2 inter path.  The C
#: library reports its own with :func:`library_minor`, and it may be **ahead**
#: of this one while a syntax revision is landing -- the parser then still
#: reads every structure it knows, and refuses what it does not.
NXVC_BITSTREAM_MINOR = 6

#: The four-byte magic at the head of every stream: the ASCII bytes ``NXV1``.
NXVC_MAGIC = 0x3156584E


class Status:
    """``nxvc_status``.  Zero is success, everything else is negative."""

    OK = 0
    ERR_ARG = -1
    ERR_UNSUPPORTED = -2
    ERR_BITSTREAM = -3
    ERR_TRUNCATED = -4
    ERR_NOMEM = -5
    ERR_VERSION = -6

    _NAMES = {
        0: "OK",
        -1: "ERR_ARG",
        -2: "ERR_UNSUPPORTED",
        -3: "ERR_BITSTREAM",
        -4: "ERR_TRUNCATED",
        -5: "ERR_NOMEM",
        -6: "ERR_VERSION",
    }

    @classmethod
    def name(cls, value: int) -> str:
        return cls._NAMES.get(value, f"UNKNOWN({value})")


class Chroma:
    """``nxvc_chroma``."""

    C420 = 0
    C444 = 1


class ColorTransform:
    """``nxvc_color_transform``."""

    NONE = 0
    YCOCGR = 1


class ColorSpace:
    """``nxvc_color_space``.  Descriptive only (SYNTAX.md 2.2): the transform,
    quantizer and entropy coder are byte for byte identical for every value."""

    UNSPECIFIED = 0
    YCBCR_709_LIMITED = 1
    YCBCR_709_FULL = 2
    RGB = 3

    _NAMES = {
        0: "unspecified",
        1: "YCbCr BT.709 limited",
        2: "YCbCr BT.709 full",
        3: "RGB",
    }

    @classmethod
    def name(cls, value: int) -> str:
        return cls._NAMES.get(value, f"reserved{value}")


class TileMode:
    """``nxvc_tile_mode``."""

    WARP_SKIP = 0
    STATIC_MV = 1
    WARP_MV = 2
    INTRA = 3
    STEREO = 4

    _NAMES = {0: "WARP_SKIP", 1: "STATIC_MV", 2: "WARP_MV", 3: "INTRA", 4: "STEREO"}

    @classmethod
    def name(cls, value: int) -> str:
        return cls._NAMES.get(value, f"reserved{value}")


class Tool:
    """Tool bits of the stream header ``tools`` u64 (SYNTAX.md 2.2)."""

    INTRA_DC_PLANE = 1 << 0
    TRANSFORM_SKIP = 1 << 1
    RES_LEVEL = 1 << 2
    CHROMA444 = 1 << 3
    ALPHA = 1 << 4
    LOSSLESS = 1 << 5
    CUSTOM_TABLES = 1 << 6
    NSUB_VAR = 1 << 7
    PER_TILE_CHROMA = 1 << 8
    YCOCGR = 1 << 9
    INTER = 1 << 10
    WARP = 1 << 11
    STEREO = 1 << 12
    LAYERS = 1 << 13
    BITDEPTH10 = 1 << 14
    ENT_OFFSET_TAB = 1 << 15
    ENT_BITPLANE = 1 << 16
    INTRA_DIR = 1 << 17
    XFORM_WAVELET = 1 << 18
    XFORM_4X4_SPLIT = 1 << 19
    WM_ID = 1 << 20
    CTX_V2 = 1 << 21
    SIGN_HIDE = 1 << 22
    #: Annex D D-5 names this "tool bit 20"; bit 20 was already WM_ID in
    #: syntax v1.2, so the reference places it at the first free bit.
    FILTER_CATMULLROM = 1 << 23
    #: Chroma predicted from the co-located reconstructed luma (7.7).
    INTRA_CFL = 1 << 24
    #: The 27-context neighbour-conditioned entropy model (9.9).
    CTX_V3 = 1 << 25
    #: The variable-length transmitted table set (9.4.1).
    TAB_V2 = 1 << 26
    #: Per-tile 16x16 and 32x32 transforms (6.7).
    XFORM_LARGE = 1 << 27
    #: The near-skip correction, in the TILE-ROW header (3.3, 13.9).
    NEAR_SKIP = 1 << 28
    #: Four motion vectors per tile, one per 32x32 quadrant (13.10).
    QUAD_MV = 1 << 29

    _NAMES = [
        (1 << 0, "INTRA_DC_PLANE"),
        (1 << 1, "TRANSFORM_SKIP"),
        (1 << 2, "RES_LEVEL"),
        (1 << 3, "CHROMA444"),
        (1 << 4, "ALPHA"),
        (1 << 5, "LOSSLESS"),
        (1 << 6, "CUSTOM_TABLES"),
        (1 << 7, "NSUB_VAR"),
        (1 << 8, "PER_TILE_CHROMA"),
        (1 << 9, "YCOCGR"),
        (1 << 10, "INTER"),
        (1 << 11, "WARP"),
        (1 << 12, "STEREO"),
        (1 << 13, "LAYERS"),
        (1 << 14, "BITDEPTH10"),
        (1 << 15, "ENT_OFFSET_TAB"),
        (1 << 16, "ENT_BITPLANE"),
        (1 << 17, "INTRA_DIR"),
        (1 << 18, "XFORM_WAVELET"),
        (1 << 19, "XFORM_4X4_SPLIT"),
        (1 << 20, "WM_ID"),
        (1 << 21, "CTX_V2"),
        (1 << 22, "SIGN_HIDE"),
        (1 << 23, "FILTER_CATMULLROM"),
        (1 << 24, "INTRA_CFL"),
        (1 << 25, "CTX_V3"),
        (1 << 26, "TAB_V2"),
        (1 << 27, "XFORM_LARGE"),
        (1 << 28, "NEAR_SKIP"),
        (1 << 29, "QUAD_MV"),
    ]

    #: The first tool bit that is reserved and must be zero (SYNTAX.md 2.3).
    RESERVED_FROM = 30

    @classmethod
    def names(cls, mask: int) -> list[str]:
        """Decode a ``tools`` mask into names, plus ``bit<n>`` for reserved bits."""
        out = [name for bit, name in cls._NAMES if mask & bit]
        known = 0
        for bit, _ in cls._NAMES:
            known |= bit
        rest = mask & ~known
        for i in range(64):
            if rest & (1 << i):
                out.append(f"bit{i}")
        return out


#: ``NXVC_TOOLS_SUPPORTED`` -- what the reference decoder implements.
TOOLS_SUPPORTED = (
    Tool.INTRA_DC_PLANE
    | Tool.TRANSFORM_SKIP
    | Tool.RES_LEVEL
    | Tool.CHROMA444
    | Tool.ALPHA
    | Tool.LOSSLESS
    | Tool.CUSTOM_TABLES
    | Tool.NSUB_VAR
    | Tool.PER_TILE_CHROMA
    | Tool.YCOCGR
    | Tool.WM_ID
    | Tool.INTRA_DIR
    | Tool.CTX_V2
    | Tool.SIGN_HIDE
    | Tool.INTER
    | Tool.WARP
    | Tool.STEREO
    | Tool.XFORM_4X4_SPLIT
    | Tool.INTRA_CFL
    | Tool.CTX_V3
    | Tool.TAB_V2
    | Tool.XFORM_LARGE
    | Tool.NEAR_SKIP
    | Tool.QUAD_MV
)

#: The Phase 1 (intra-only) subset of :data:`TOOLS_SUPPORTED`.  Kept separate
#: because ``TOOLS_SUPPORTED`` tracks the reference decoder, which grew the
#: inter tools in syntax v1.4, while :func:`nxvc.bitstream.phase1_reject_reason`
#: still has to answer "would an intra-only decoder take this stream?".
TOOLS_PHASE1 = TOOLS_SUPPORTED & ~(
    Tool.INTER | Tool.WARP | Tool.STEREO
    # The inter-efficiency tools describe INTER tiles and their modes, so an
    # intra-only decoder cannot take a stream that sets them either.
    | Tool.NEAR_SKIP | Tool.QUAD_MV
)

# ----------------------------------------------------------------- structures

u8p = POINTER(c_uint8)
u16p = POINTER(c_uint16)


class nxvc_image(Structure):
    """8-bit planar image.  ``plane[0]=Y/R'``, ``[1]=Co/G'``, ``[2]=Cg/B'``, ``[3]=A``."""

    _fields_ = [
        ("plane", u8p * 4),
        ("stride", c_int32 * 4),
    ]


class nxvc_view(Structure):
    """``nxvc_view``: one eye's pose and FOV, the encoder's only float input."""

    _fields_ = [
        ("qx", c_double),
        ("qy", c_double),
        ("qz", c_double),
        ("qw", c_double),
        ("fov_left", c_double),
        ("fov_right", c_double),
        ("fov_up", c_double),
        ("fov_down", c_double),
    ]


class nxvc_config(Structure):
    _fields_ = [
        ("width", c_uint32),
        ("height", c_uint32),
        ("chroma", c_uint32),
        ("bit_depth", c_uint32),
        ("color_transform", c_uint32),
        ("color_space", c_uint32),
        ("alpha", c_uint32),
        ("base_qp", c_uint32),
        ("chroma_qp_off", c_int32),
        ("alpha_qp_off", c_int32),
        ("quant_matrix", c_uint32),
        ("custom_matrix", u8p),
        ("lossless", c_uint32),
        ("transform_skip", c_uint32),
        ("nsub_log2", c_uint32),
        ("tile_chroma420", c_uint32),
        ("custom_tables", c_uint32),
        ("profile", c_uint32),
        ("level", c_uint32),
        ("collect_stats", c_uint32),
        # --- additive since syntax v1.2: encoder-side tuning only.  They pick
        # different levels and per-tile parameters; they never change how a
        # stream decodes.
        ("rdo", c_uint32),
        ("rdo_lambda_q8", c_uint32),
        ("qp_search", c_uint32),
        ("wm_id", c_uint32),
        # --- additive since syntax v1.3.  intra_dir, ctx_v2 and sign_hide DO
        # change the bitstream: each sets a tool bit in the stream header.
        ("intra_dir", c_uint32),
        ("intra_dir_layer", c_uint32),
        ("ctx_v2", c_uint32),
        ("intra_dir_cand", c_uint32),
        ("sign_hide", c_uint32),
        # --- additive since syntax v1.4: the Phase 2 inter path.  width/height
        # are PER EYE; with eyes == 2 the nxvc_image is `eyes * width` wide,
        # one picture per eye, eye 0 first.
        ("eyes", c_uint32),
        ("inter", c_uint32),
        ("stereo", c_uint32),
        ("intra_period", c_uint32),
        ("ref_sel", c_uint32),
        ("mv_range", c_uint32),
        ("skip_thresh", c_uint32),
        ("mode_lambda_q8", c_uint32),
        # --- additive since syntax v1.6, in merge order.  These MIRROR the
        # append order of nxvc_config in include/nxvc/nxvc.h; a field out of
        # order here silently reads a different one.
        ("split4x4", c_uint32),
        ("chroma_from_luma", c_uint32),
        ("ctx_v3", c_uint32),
        ("tab_v2", c_uint32),
        ("table_iters", c_uint32),
        ("table_iters_set", c_uint32),
        # the inter efficiency package
        ("near_skip", c_uint32),
        ("quad_mv", c_uint32),
        ("drift_refresh", c_uint32),
        ("drift_gate_q8", c_uint32),
        # the transform package
        ("xform_size", c_uint32),
        # the rate-distortion package (encoder-only effort knobs)
        ("preset", c_uint32),
        ("rdoq_effort", c_uint32),
        ("me_effort", c_uint32),
        ("lambda_class_off", c_uint32),
        ("lambda_class_q8", c_uint32 * 4),
        ("dc_lambda_q8", c_uint32),
        ("dc_rdoq_off", c_uint32),
        ("qp_search_step", c_uint32),
        ("chroma_weight_q8", c_uint32),
    ]


class nxvc_encode_stats(Structure):
    """``nxvc_encode_stats``: where the bits went in the last encoded frame."""

    _fields_ = [
        ("bytes_total", c_uint64),
        ("bytes_frame_header", c_uint64),
        ("bytes_tables", c_uint64),
        ("bytes_row_headers", c_uint64),
        ("bytes_tile_headers", c_uint64),
        ("bytes_payload", c_uint64),
        ("bytes_rans_init", c_uint64),
        ("bits_dc_plane", c_uint64),
        ("bits_luma_blocks", c_uint64),
        ("bits_chroma_blocks", c_uint64),
        ("bits_alpha_blocks", c_uint64),
        ("tiles", c_uint64),
        ("tiles_tskip", c_uint64),
        ("tiles_res", c_uint64 * 3),
        ("lanes_total", c_uint64),
        # the rate-distortion package: the rate model's own prediction, so it
        # can be checked against the payload it produced.
        ("bits_predicted_q10", c_uint64),
    ]


class nxvc_tile_layout(Structure):
    _fields_ = [
        ("tiles_x", c_uint32),
        ("tiles_y", c_uint32),
        ("tile_count", c_uint32),
        ("tile_size", c_uint32),
    ]


class nxvc_tile_info(Structure):
    _fields_ = [
        ("tile_index", c_uint16),
        ("payload_len", c_uint16),
        ("layer", c_uint8),
        ("eye", c_uint8),
        ("mode", c_uint8),
        ("res_level", c_uint8),
        ("chroma444", c_uint8),
        ("alpha_mode", c_uint8),
        ("table_set", c_uint8),
        ("nsub_log2", c_uint8),
        ("tskip", c_uint8),
        ("wgt", c_uint8),
        ("ref_sel", c_uint8),
        ("mv_present", c_uint8),
        ("qp_delta", c_int8),
        ("mv_x", c_int8),
        ("mv_y", c_int8),
        ("alpha_value", c_uint8),
        ("qp", c_uint8),
        ("wm_id", c_uint8),
        ("intra_dir", c_uint8),
        ("skipped", c_uint8),
        ("concealed", c_uint8),
        ("disparity", c_uint16),
        ("ref_delta", c_uint8),
        ("age_since_coded", c_uint16),
        # --- appended for syntax v1.6, in merge order (see nxvc_tile_info).
        ("split4x4", c_uint8),
        ("xform_size", c_uint8),
        ("near_skip", c_uint8),
        ("quad_mv", c_uint8),
        ("corr", c_int8 * 9),
        ("qmv", c_int8 * 8),
        ("warp_mad_q8", c_uint16),
    ]


class nxvc_stream_info(Structure):
    _fields_ = [
        ("magic", c_uint32),
        ("version", c_uint32),
        ("profile", c_uint32),
        ("level", c_uint32),
        ("tile_size", c_uint32),
        ("width", c_uint32),
        ("height", c_uint32),
        ("eyes", c_uint32),
        ("bit_depth", c_uint32),
        ("num_layers", c_uint32),
        ("chroma", c_uint32),
        ("color_transform", c_uint32),
        ("color_space", c_uint32),
        ("alpha", c_uint32),
        ("tools", c_uint64),
        ("layer_desc", c_uint32 * 4),
        ("ext_len", c_uint32),
        ("ext_tlv_count", c_uint32),
        ("ext_unknown_count", c_uint32),
    ]


class nxvc_frame_info(Structure):
    _fields_ = [
        ("frame_number", c_uint32),
        ("base_qp", c_uint32),
        ("chroma_qp_off", c_int32),
        ("alpha_qp_off", c_int32),
        ("quant_matrix", c_uint32),
        ("tables_present", c_uint32),
        ("ref_slots", c_uint32),
        ("flags", c_uint32),
        ("frame_bytes", c_uint32),
        ("pose", c_uint8 * 26),
        ("tile_count", c_uint32),
        ("warp_present", c_uint32),
        ("warp", (c_int32 * 9) * 2),
    ]


class _nxvc_encoder(Structure):
    pass


class _nxvc_decoder(Structure):
    pass


encoder_p = POINTER(_nxvc_encoder)
decoder_p = POINTER(_nxvc_decoder)

# --------------------------------------------------------------------- errors


class NxvcError(RuntimeError):
    """A non-zero ``nxvc_status`` returned by the C library."""

    def __init__(self, status: int, what: str = "") -> None:
        self.status = int(status)
        self.status_name = Status.name(self.status)
        detail = status_string(self.status)
        msg = f"{what}: " if what else ""
        super().__init__(f"{msg}{self.status_name} ({self.status}) -- {detail}")


class LibraryNotFound(ImportError):
    """The nxvc shared library could not be located or loaded."""


# ------------------------------------------------------------- library search

_LIB_BASENAMES = ("nxvc_ref", "nxvc")


def _platform_names(base: str) -> tuple[str, ...]:
    if sys.platform == "win32":
        return (f"{base}.dll", f"lib{base}.dll")
    if sys.platform == "darwin":
        return (f"lib{base}.dylib", f"{base}.dylib")
    return (f"lib{base}.so", f"{base}.so")


def _repo_root() -> Path | None:
    """Find the nx-warp checkout this package was installed from, if any.

    Works for an editable install (``src/nxvc`` inside the repo) and for a
    source checkout sitting next to the current working directory.
    """
    candidates = [Path(__file__).resolve()]
    cwd = Path.cwd().resolve()
    candidates.append(cwd / "x")
    for start in candidates:
        for parent in start.parents:
            if (parent / "include" / "nxvc" / "nxvc.h").is_file() and (
                parent / "ref" / "CMakeLists.txt"
            ).is_file():
                return parent
    return None


#: Build trees whose library is built with a sanitizer or a fuzzing runtime.
#: Loading one of those from ctypes aborts the interpreter before main ("ASan
#: runtime does not come first in initial library list"), so they are searched
#: last: an ordinary build in the same checkout must win, and NXVC_LIBRARY is
#: still the way to ask for one of these deliberately.
_INSTRUMENTED = re.compile(r"asan|ubsan|msan|tsan|lsan|sanitiz|fuzz|coverage")


def _build_dirs(root: Path) -> Iterator[Path]:
    """Plausible CMake build trees under an nx-warp checkout, newest first."""
    seen: set[Path] = set()
    dirs: list[Path] = []
    for entry in sorted(root.iterdir() if root.is_dir() else []):
        if not entry.is_dir():
            continue
        if entry.name == "build" or entry.name.startswith("build-") or entry.name.startswith("build_"):
            dirs.append(entry)
    # Most recently touched build tree first: that is nearly always the one the
    # developer means -- but never an instrumented one ahead of a plain one.
    dirs.sort(key=lambda p: (bool(_INSTRUMENTED.search(p.name)), -p.stat().st_mtime))
    for d in dirs:
        for sub in (d / "ref", d, d / "lib"):
            if sub not in seen:
                seen.add(sub)
                yield sub


def search_paths() -> list[str]:
    """The directories that :func:`load` will search, in order.

    Useful in an error message, and printed by ``python -m nxvc info --probe``.
    """
    out: list[str] = []

    env_dir = os.environ.get("NXVC_LIBRARY_PATH")
    if env_dir:
        out.extend(p for p in env_dir.split(os.pathsep) if p)

    build_dir = os.environ.get("NXVC_BUILD_DIR")
    if build_dir:
        out.extend([str(Path(build_dir) / "ref"), build_dir])

    root = _repo_root()
    if root is not None:
        out.extend(str(p) for p in _build_dirs(root))

    return out


#: Symbols a sanitizer runtime exports.  Their presence in the file means
#: loading it with ctypes will abort the interpreter before main -- the runtime
#: has to be the first library in the initial list, which it cannot be when it
#: arrives through dlopen.  There is no way to recover from that, so such a
#: library is never picked up by the automatic search; NXVC_LIBRARY still
#: reaches it for anyone who means it.
_SANITIZER_MARKERS = (b"__asan_init", b"__tsan_init", b"__msan_init", b"__hwasan_init")


def _looks_instrumented(path: Path) -> bool:
    """True when *path* is linked against, or contains, a sanitizer runtime."""
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except OSError:
        return False
    return any(marker in blob for marker in _SANITIZER_MARKERS)


def _candidate_files() -> Iterator[str]:
    explicit = os.environ.get("NXVC_LIBRARY")
    if explicit:
        yield explicit
        return

    for directory in search_paths():
        d = Path(directory)
        if not d.is_dir():
            continue
        for base in _LIB_BASENAMES:
            for name in _platform_names(base):
                candidate = d / name
                if candidate.is_file() and not _looks_instrumented(candidate):
                    yield str(candidate)

    # System search last: plain soname, then whatever the loader knows about.
    for base in _LIB_BASENAMES:
        for name in _platform_names(base):
            yield name
        found = ctypes.util.find_library(base)
        if found:
            yield found


_lib: ctypes.CDLL | None = None
_lib_path: str | None = None
_load_error: str | None = None
_tried: list[str] = []


def load() -> ctypes.CDLL:
    """Load the nxvc shared library, binding every prototype.  Cached.

    Raises :class:`LibraryNotFound` with the full search list if it is missing.
    """
    global _lib, _lib_path, _load_error, _tried
    if _lib is not None:
        return _lib
    if _load_error is not None:
        raise LibraryNotFound(_load_error)

    errors: list[str] = []
    tried: list[str] = []
    for cand in _candidate_files():
        tried.append(cand)
        try:
            lib = ctypes.CDLL(cand)
        except OSError as exc:  # not there, or there but unloadable
            errors.append(f"  {cand}: {exc}")
            continue
        try:
            _bind(lib)
        except AttributeError as exc:
            errors.append(f"  {cand}: loaded but missing a symbol: {exc}")
            continue
        _lib = lib
        _lib_path = cand
        _tried = tried
        return lib

    _tried = tried
    _load_error = _not_found_message(errors)
    raise LibraryNotFound(_load_error)


def _not_found_message(errors: list[str]) -> str:
    paths = search_paths()
    lines = [
        "the nxvc shared library could not be loaded.",
        "",
        "Set NXVC_LIBRARY to the library file, or NXVC_LIBRARY_PATH / NXVC_BUILD_DIR",
        "to a directory containing it.  Names tried: "
        + ", ".join(_platform_names(b)[0] for b in _LIB_BASENAMES)
        + ".",
        "",
        "Directories searched:",
    ]
    lines.extend(f"  {p}" for p in paths) if paths else lines.append("  (none found)")
    lines += [
        "",
        "NOTE: ref/CMakeLists.txt has an `nxvc_ref_shared` target producing",
        "libnxvc_ref.so, but a build tree only contains it once it is built:",
        "",
        "  cmake --build <build-dir> --target nxvc_ref_shared",
        "",
        "ctypes cannot load the static libnxvc_ref.a that the default target",
        "produces.  A library that loads but is missing a symbol these",
        "bindings declare is REFUSED, not used: it was built from an older",
        "include/nxvc/nxvc.h, and its structure layouts would disagree with",
        "these ones silently and corrupt every per-tile record.  Rebuild it.",
        "",
        "Everything that does not need the codec itself -- the bitstream parser",
        "(nxvc.bitstream) and the metrics (nxvc.metrics) -- works without it.",
    ]
    if errors:
        lines += ["", "Load attempts:"] + errors
    return "\n".join(lines)


def is_available() -> bool:
    """True if the shared library can be loaded.  Never raises."""
    try:
        load()
    except LibraryNotFound:
        return False
    return True


def library_path() -> str | None:
    """Path of the loaded library, or None if it has not been loaded."""
    return _lib_path


def load_error() -> str | None:
    """The full 'library not found' message, or None if the library loaded."""
    if _lib is not None:
        return None
    try:
        load()
    except LibraryNotFound as exc:
        return str(exc)
    return None


# ------------------------------------------------------------------ bindings


def _bind(lib: ctypes.CDLL) -> None:
    """Attach argtypes/restypes for every function in nxvc.h."""

    # config / layout -----------------------------------------------------
    lib.nxvc_config_default.argtypes = [POINTER(nxvc_config)]
    lib.nxvc_config_default.restype = None

    lib.nxvc_tile_layout_get.argtypes = [c_uint32, c_uint32, POINTER(nxvc_tile_layout)]
    lib.nxvc_tile_layout_get.restype = None

    lib.nxvc_tile_layout_get_ex.argtypes = [
        c_uint32,
        c_uint32,
        c_uint32,
        POINTER(nxvc_tile_layout),
    ]
    lib.nxvc_tile_layout_get_ex.restype = None

    lib.nxvc_status_string.argtypes = [c_int]
    lib.nxvc_status_string.restype = c_char_p

    lib.nxvc_version_string.argtypes = []
    lib.nxvc_version_string.restype = c_char_p

    # encoder -------------------------------------------------------------
    lib.nxvc_encoder_create.argtypes = [POINTER(nxvc_config), POINTER(c_int)]
    lib.nxvc_encoder_create.restype = encoder_p

    lib.nxvc_encoder_destroy.argtypes = [encoder_p]
    lib.nxvc_encoder_destroy.restype = None

    lib.nxvc_encoder_stream_header.argtypes = [
        encoder_p,
        u8p,
        c_size_t,
        POINTER(c_size_t),
    ]
    lib.nxvc_encoder_stream_header.restype = c_int

    lib.nxvc_encoder_add_tlv.argtypes = [encoder_p, c_uint16, u8p, c_uint16]
    lib.nxvc_encoder_add_tlv.restype = c_int

    lib.nxvc_encoder_set_pose.argtypes = [encoder_p, u8p]
    lib.nxvc_encoder_set_pose.restype = None

    lib.nxvc_encoder_set_views.argtypes = [encoder_p, POINTER(nxvc_view), c_uint32]
    lib.nxvc_encoder_set_views.restype = c_int

    lib.nxvc_encoder_tile_count.argtypes = [encoder_p]
    lib.nxvc_encoder_tile_count.restype = c_uint32

    lib.nxvc_encoder_set_skip_map.argtypes = [encoder_p, u8p, c_uint32]
    lib.nxvc_encoder_set_skip_map.restype = c_int
    lib.nxvc_encoder_set_wm_map.argtypes = [encoder_p, u8p, c_uint32]
    lib.nxvc_encoder_set_wm_map.restype = c_int

    lib.nxvc_encoder_set_received_tiles.argtypes = [encoder_p, u8p, c_uint32]
    lib.nxvc_encoder_set_received_tiles.restype = c_int

    lib.nxvc_encoder_shadow_image.argtypes = [encoder_p, POINTER(nxvc_image)]
    lib.nxvc_encoder_shadow_image.restype = c_int

    lib.nxvc_encoder_encode_frame.argtypes = [
        encoder_p,
        POINTER(nxvc_image),
        u8p,
        u8p,
        u8p,
        c_size_t,
        POINTER(c_size_t),
    ]
    lib.nxvc_encoder_encode_frame.restype = c_int

    lib.nxvc_encoder_tiles.argtypes = [encoder_p, POINTER(c_uint32)]
    lib.nxvc_encoder_tiles.restype = POINTER(nxvc_tile_info)

    lib.nxvc_encoder_stats.argtypes = [encoder_p, POINTER(nxvc_encode_stats)]
    lib.nxvc_encoder_stats.restype = c_int

    # decoder -------------------------------------------------------------
    lib.nxvc_decoder_create.argtypes = [POINTER(c_int)]
    lib.nxvc_decoder_create.restype = decoder_p

    lib.nxvc_decoder_destroy.argtypes = [decoder_p]
    lib.nxvc_decoder_destroy.restype = None

    lib.nxvc_decoder_parse_stream_header.argtypes = [
        decoder_p,
        u8p,
        c_size_t,
        POINTER(c_size_t),
    ]
    lib.nxvc_decoder_parse_stream_header.restype = c_int

    lib.nxvc_decoder_stream_info.argtypes = [decoder_p, POINTER(nxvc_stream_info)]
    lib.nxvc_decoder_stream_info.restype = c_int

    lib.nxvc_decoder_decode_frame.argtypes = [
        decoder_p,
        u8p,
        c_size_t,
        POINTER(nxvc_image),
        POINTER(c_size_t),
    ]
    lib.nxvc_decoder_decode_frame.restype = c_int

    lib.nxvc_decoder_scan_frame.argtypes = [
        decoder_p,
        u8p,
        c_size_t,
        POINTER(nxvc_frame_info),
        POINTER(c_size_t),
    ]
    lib.nxvc_decoder_scan_frame.restype = c_int

    lib.nxvc_decoder_plane_size.argtypes = [
        decoder_p,
        c_int,
        POINTER(c_uint32),
        POINTER(c_uint32),
    ]
    lib.nxvc_decoder_plane_size.restype = c_int

    lib.nxvc_decoder_set_lost_tiles.argtypes = [decoder_p, u8p, c_uint32]
    lib.nxvc_decoder_set_lost_tiles.restype = c_int

    lib.nxvc_decoder_tile_count.argtypes = [decoder_p]
    lib.nxvc_decoder_tile_count.restype = c_uint32

    lib.nxvc_decoder_tiles.argtypes = [decoder_p, POINTER(c_uint32)]
    lib.nxvc_decoder_tiles.restype = POINTER(nxvc_tile_info)

    lib.nxvc_decoder_frame_info.argtypes = [decoder_p, POINTER(nxvc_frame_info)]
    lib.nxvc_decoder_frame_info.restype = c_int

    # utility -------------------------------------------------------------
    lib.nxvc_ycocgr_forward.argtypes = [u8p, u8p, u8p, u8p, u16p, u16p, c_size_t]
    lib.nxvc_ycocgr_forward.restype = None

    lib.nxvc_ycocgr_inverse.argtypes = [u8p, u16p, u16p, u8p, u8p, u8p, c_size_t]
    lib.nxvc_ycocgr_inverse.restype = None


# ------------------------------------------------------------------ helpers


def status_string(status: int) -> str:
    """``nxvc_status_string`` if the library is loaded, else a built-in string."""
    if _lib is not None:
        s = _lib.nxvc_status_string(int(status))
        if s:
            return s.decode("utf-8", "replace")
    return {
        Status.OK: "ok",
        Status.ERR_ARG: "bad argument",
        Status.ERR_UNSUPPORTED: "legal v1 syntax outside Phase 1 scope",
        Status.ERR_BITSTREAM: "malformed bitstream",
        Status.ERR_TRUNCATED: "truncated bitstream",
        Status.ERR_NOMEM: "output buffer too small or allocation failed",
        Status.ERR_VERSION: "magic, version or tool mask refused",
    }.get(int(status), "unknown status")


def version_string() -> str | None:
    """``nxvc_version_string()``, or None when the library is not loaded.

    The C string is ``"nxvc_ref <major>.<minor> (syntax v<major>.<minor>)"``.
    """
    if not is_available():
        return None
    assert _lib is not None
    s = _lib.nxvc_version_string()
    return s.decode("utf-8", "replace") if s else None


def library_minor() -> int | None:
    """``NXVC_BITSTREAM_MINOR`` of the loaded library, parsed out of
    :func:`version_string`, or None if it is unavailable or unparsable."""
    text = version_string()
    if not text:
        return None
    m = re.search(r"syntax v(\d+)\.(\d+)", text)
    return int(m.group(2)) if m else None


def check(status: int, what: str = "") -> int:
    """Raise :class:`NxvcError` unless *status* is ``NXVC_OK``."""
    if status != Status.OK:
        raise NxvcError(status, what)
    return status


def buffer_ptr(buf) -> "ctypes._Pointer":
    """A ``uint8_t *`` onto a writable ctypes buffer or bytearray."""
    return ctypes.cast(buf, u8p)


def bytes_ptr(data: bytes) -> "ctypes._Pointer":
    """A ``uint8_t *`` onto immutable bytes (the C side must not write it)."""
    return ctypes.cast(ctypes.c_char_p(data), u8p)
