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
