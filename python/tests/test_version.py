"""The package version must track the C project it binds to.

The bindings carry a static version (there is no version symbol in the C ABI
to read).  These tests are the mechanism that keeps it honest: they compare it
against the root ``CMakeLists.txt`` and against ``NXVC_VERSION`` in
``include/nxvc/nxvc.h`` whenever the repository is next to the package, and
skip cleanly when only the wheel is installed.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

import nxvc
from nxvc._ffi import _repo_root


def repo() -> Path:
    root = _repo_root()
    if root is None:
        pytest.skip("not running from an nx-warp checkout")
    return root


def test_version_is_a_release_number():
    assert re.fullmatch(r"\d+\.\d+\.\d+(?:[.-]\w+)?", nxvc.__version__)


def test_version_matches_the_root_cmakelists():
    text = (repo() / "CMakeLists.txt").read_text()
    m = re.search(r"project\s*\(\s*nxwarp\s+VERSION\s+([0-9.]+)", text)
    assert m, "root CMakeLists.txt has no project(nxwarp VERSION ...)"
    assert nxvc.__version__ == m.group(1), (
        "python/src/nxvc/_version.py is out of step with the root CMakeLists.txt; "
        "update __version__ to " + m.group(1)
    )


def test_bitstream_version_matches_the_header_macro():
    text = (repo() / "include" / "nxvc" / "nxvc.h").read_text()
    m = re.search(r"#define\s+NXVC_VERSION\s+(\d+)", text)
    assert m, "nxvc.h has no NXVC_VERSION"
    assert nxvc.NXVC_VERSION == int(m.group(1))


def test_magic_matches_the_syntax_document():
    # SYNTAX.md 2: magic is 0x3156584E, the bytes 'N' 'X' 'V' '1'.
    assert nxvc.NXVC_MAGIC == 0x3156584E
    assert nxvc.NXVC_MAGIC.to_bytes(4, "little") == b"NXV1"


def test_supported_tool_mask_matches_the_header():
    text = (repo() / "include" / "nxvc" / "nxvc.h").read_text()
    block = text.split("#define NXVC_TOOLS_SUPPORTED", 1)[1].split("/*", 1)[0]
    names = set(re.findall(r"NXVC_TOOL_([A-Z0-9_]+)", block))
    ours = set(nxvc.Tool.names(nxvc.TOOLS_SUPPORTED))
    assert names == ours, (
        "NXVC_TOOLS_SUPPORTED in nxvc.h and TOOLS_SUPPORTED in nxvc/_ffi.py disagree"
    )


def test_tool_bit_numbers_match_the_header():
    text = (repo() / "include" / "nxvc" / "nxvc.h").read_text()
    for name, shift in re.findall(r"#define NXVC_TOOL_([A-Z0-9_]+)\s+\(1ull << (\d+)\)", text):
        assert hasattr(nxvc.Tool, name), f"nxvc.Tool is missing {name}"
        assert getattr(nxvc.Tool, name) == 1 << int(shift), name


# --------------------------------------------------- ABI drift, mechanically


_C_TO_CTYPES = {
    "uint8_t": "c_ubyte",
    "int8_t": "c_byte",
    "uint16_t": "c_ushort",
    "int16_t": "c_short",
    "uint32_t": "c_uint",
    "int32_t": "c_int",
    "uint64_t": "c_ulong",
    "int64_t": "c_long",
}


def _c_struct_fields(header: str, name: str) -> list[tuple[str, str]]:
    """(type, field) pairs of a struct in nxvc.h, in declaration order.

    Handles the header's ``uint8_t layer, eye, mode, res_level;`` style and
    array members; skips comments and the pointer member, which is checked
    separately.
    """
    body = header.split(f"typedef struct {name} {{", 1)[1].split("}", 1)[0]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    out: list[tuple[str, str]] = []
    for decl in body.split(";"):
        decl = " ".join(decl.split())
        if not decl:
            continue
        m = re.match(r"(?:const\s+)?(\w+)\s+(.*)$", decl)
        assert m, decl
        ctype, names = m.group(1), m.group(2)
        for n in names.split(","):
            n = n.strip()
            arr = re.match(r"\*?(\w+)\s*\[(\d+)\]$", n)
            if arr:
                # `uint8_t *plane[4]` is an array of pointers; `uint8_t pose[26]`
                # an array of scalars.  The star stays in the type either way so
                # the scalar check skips pointer members.
                out.append((ctype + ("*" if n.startswith("*") else ""), arr.group(1)))
            elif n.startswith("*"):
                out.append((ctype + "*", n.lstrip("*")))
            else:
                out.append((ctype, n))
    return out


@pytest.mark.parametrize(
    "c_name, py_name",
    [
        ("nxvc_config", "nxvc_config"),
        ("nxvc_tile_info", "nxvc_tile_info"),
        ("nxvc_stream_info", "nxvc_stream_info"),
        ("nxvc_frame_info", "nxvc_frame_info"),
        ("nxvc_tile_layout", "nxvc_tile_layout"),
        ("nxvc_encode_stats", "nxvc_encode_stats"),
        ("nxvc_image", "nxvc_image"),
    ],
)
def test_ctypes_structs_match_the_header_field_for_field(c_name, py_name):
    """The single check that catches a C ABI change before it corrupts memory.

    A field inserted in the middle of a struct is invisible to every other
    test -- the calls still succeed and the values are silently wrong -- so
    the field *order and names* are compared against the header text itself.
    """
    from nxvc import _ffi

    text = (repo() / "include" / "nxvc" / "nxvc.h").read_text()
    expected = [name for _, name in _c_struct_fields(text, c_name)]
    actual = [name for name, _ in getattr(_ffi, py_name)._fields_]
    assert actual == expected, (
        f"{c_name} in nxvc.h and {py_name} in nxvc/_ffi.py have diverged.\n"
        f"  header: {expected}\n  binding: {actual}"
    )


def test_ctypes_scalar_types_match_the_header():
    """Widths and signedness, for every plain integer member."""
    from nxvc import _ffi

    text = (repo() / "include" / "nxvc" / "nxvc.h").read_text()
    for c_name in (
        "nxvc_config",
        "nxvc_tile_info",
        "nxvc_stream_info",
        "nxvc_frame_info",
        "nxvc_tile_layout",
        "nxvc_encode_stats",
    ):
        declared = dict(
            (name, ctype) for ctype, name in _c_struct_fields(text, c_name)
        )
        for name, pytype in getattr(_ffi, c_name)._fields_:
            want = _C_TO_CTYPES.get(declared[name])
            if want is None:  # a pointer member; not a scalar
                continue
            # An array member (pose[26], layer_desc[4], tiles_res[3]) is
            # checked on its element type.
            if hasattr(pytype, "_length_"):
                pytype = pytype._type_
            got = pytype.__name__
            # ctypes aliases: c_uint32 IS c_uint, c_uint64 IS c_ulong on LP64
            # and c_ulonglong elsewhere.
            assert got.replace("c_ulonglong", "c_ulong") == want, (
                f"{c_name}.{name}: header says {declared[name]}, binding has {got}"
            )


def test_every_public_c_function_is_bound():
    """A new entry point in nxvc.h must reach the binding, not be forgotten."""
    from nxvc import _ffi

    text = (repo() / "include" / "nxvc" / "nxvc.h").read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    declared = set(re.findall(r"\b(nxvc_[a-z0-9_]+)\s*\(", text))
    declared -= {"nxvc_config", "nxvc_status", "nxvc_image"}  # struct/enum names

    source = (
        Path(_ffi.__file__).read_text()
        if hasattr(_ffi, "__file__")
        else ""
    )
    bound = set(re.findall(r"lib\.(nxvc_[a-z0-9_]+)\.argtypes", source))
    missing = sorted(n for n in declared if n not in bound)
    assert not missing, (
        "these functions are declared in nxvc.h but not bound in nxvc/_ffi.py: "
        + ", ".join(missing)
    )
