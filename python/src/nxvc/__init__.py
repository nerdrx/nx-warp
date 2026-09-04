"""Python bindings for NX Warp's ``nxvc`` reference codec.

Two halves that can be used independently:

* :mod:`nxvc.bitstream` and :mod:`nxvc.metrics` are **pure Python** (numpy for
  the metrics) and work everywhere.  You can inspect and validate a ``.nxv``
  file, and measure quality, on a machine where the codec is not built.
* :mod:`nxvc.codec` (:class:`Encoder`, :class:`Decoder`) is a ctypes binding
  over the C ABI in ``include/nxvc/nxvc.h`` and needs the shared library.

:data:`NXVC_AVAILABLE` says whether the library was found.  When it is False,
:data:`NXVC_LOAD_ERROR` holds the full explanation, including every directory
that was searched, and calling into the codec raises
:class:`LibraryNotFound` carrying that same text.

    >>> import nxvc
    >>> nxvc.NXVC_AVAILABLE                     # doctest: +SKIP
    True
    >>> nxvc.require_library()                  # doctest: +SKIP

Set ``NXVC_LIBRARY`` to a library file, or ``NXVC_LIBRARY_PATH`` /
``NXVC_BUILD_DIR`` to a directory holding one, to point the loader at a
specific build.
"""

from __future__ import annotations

from . import bitstream, metrics
from ._ffi import (
    NXVC_MAGIC,
    NXVC_TILE_SIZE,
    NXVC_VERSION,
    TOOLS_SUPPORTED,
    Chroma,
    ColorSpace,
    ColorTransform,
    LibraryNotFound,
    NxvcError,
    Status,
    TileMode,
    Tool,
)
from ._ffi import is_available as _is_available
from ._ffi import library_path as _library_path
from ._ffi import load_error as _load_error
from ._ffi import search_paths, status_string
from ._version import __version__

#: True when the nxvc shared library was found and every symbol bound.
NXVC_AVAILABLE: bool = _is_available()

#: Path of the loaded library, or None.
NXVC_LIBRARY_PATH: str | None = _library_path()

#: When :data:`NXVC_AVAILABLE` is False, why -- with the directories searched
#: and how to build a shared library.  None when the library loaded.
NXVC_LOAD_ERROR: str | None = _load_error()

__all__ = [
    "__version__",
    "NXVC_AVAILABLE",
    "NXVC_LIBRARY_PATH",
    "NXVC_LOAD_ERROR",
    "NXVC_VERSION",
    "NXVC_MAGIC",
    "NXVC_TILE_SIZE",
    "TOOLS_SUPPORTED",
    "Chroma",
    "ColorSpace",
    "ColorTransform",
    "Status",
    "TileMode",
    "Tool",
    "LibraryNotFound",
    "NxvcError",
    "require_library",
    "search_paths",
    "status_string",
    "bitstream",
    "metrics",
]

if NXVC_AVAILABLE:
    from .codec import (  # noqa: F401
        Decoder,
        EncodeStats,
        Encoder,
        EncoderConfig,
        FrameInfo,
        StreamInfo,
        TileInfo,
        TileLayout,
        plane_shapes,
        read_planar_yuv,
        tile_layout,
        write_planar_yuv,
        ycocgr_forward,
        ycocgr_inverse,
    )

    __all__ += [
        "Encoder",
        "Decoder",
        "EncoderConfig",
        "EncodeStats",
        "TileLayout",
        "TileInfo",
        "StreamInfo",
        "FrameInfo",
        "tile_layout",
        "plane_shapes",
        "read_planar_yuv",
        "write_planar_yuv",
        "ycocgr_forward",
        "ycocgr_inverse",
    ]
else:  # pragma: no cover - exercised on machines without the library
    # plane_shapes and read/write_planar_yuv are pure arithmetic and stay
    # available; only the codec classes need the library.
    from .codec import plane_shapes, read_planar_yuv, write_planar_yuv  # noqa: F401

    __all__ += ["plane_shapes", "read_planar_yuv", "write_planar_yuv"]


def require_library():
    """Return the loaded ``ctypes.CDLL``, or raise a helpful error.

    Call this at the top of anything that needs the codec so the failure is a
    clear message about how to build or locate the library rather than an
    ``AttributeError`` three frames deeper.
    """
    from ._ffi import load

    return load()
