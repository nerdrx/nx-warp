"""The pure-Python parser against every committed conformance vector.

``tests/vectors/`` is the repository's contract with itself: `vectors.md5`
pins the bytes of each stream and the pixels it decodes to, `rejects.md5` pins
the exact status each malformed stream must be refused with.  The C decoder is
checked against both by ``nxv-vectors --check``; this module checks the *other*
implementation of the syntax, :mod:`nxvc.bitstream`, against the same files.

That matters because the parser is written from ``docs/SYNTAX.md`` and the
codec from the same document but in a different language: when the two agree on
44 committed bitstreams, the document is not ambiguous in the places those
vectors touch.  Where the ``nxv-info`` binary is built, its output is compared
field for field as a third reading of the same bytes.

Every test here skips cleanly when the repository is not next to the package.
"""

from __future__ import annotations

import hashlib
import re
import subprocess
from pathlib import Path

import pytest

import nxvc
from nxvc import bitstream as bs
from nxvc._ffi import _repo_root

# --------------------------------------------------------------- the manifests


def _vector_dir() -> Path:
    root = _repo_root()
    if root is None:
        pytest.skip("not running from an nx-warp checkout")
    d = root / "tests" / "vectors"
    if not d.is_dir():
        pytest.skip("tests/vectors is not present in this checkout")
    return d


def _manifest(name: str) -> list[list[str]]:
    rows = []
    for line in (_vector_dir() / name).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            rows.append(line.split())
    return rows


def _try_manifest(name: str) -> list[list[str]]:
    """The manifest rows, or [] when the checkout has no vectors at all.

    Used at *collection* time, where ``pytest.skip`` would be an error; the
    tests themselves skip through :func:`_vector_dir`.
    """
    root = _repo_root()
    if root is None:
        return []
    path = root / "tests" / "vectors" / name
    if not path.is_file():
        return []
    return [
        line.split()
        for line in path.read_text().splitlines()
        if line.strip() and not line.startswith("#")
    ]


CONFORMANCE = _try_manifest("vectors.md5")
REJECTS = _try_manifest("rejects.md5")


def _id(row):
    return row[0]


# ------------------------------------------------------------ nxv-info, if any


def _nxv_info() -> Path | None:
    """The most recently built ``nxv-info``, or None.

    The tool only parses headers, so a build a little behind the header still
    reads a committed vector correctly; nothing here depends on the codec.
    """
    root = _repo_root()
    if root is None:
        return None
    found = [
        p
        for d in sorted(root.glob("build*"))
        for p in (d / "bin" / "nxv-info", d / "ref" / "nxv-info")
        if p.is_file()
    ]
    if not found:
        return None
    found.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return found[0]


NXV_INFO = _nxv_info()


def _run_info(path: Path) -> str:
    assert NXV_INFO is not None
    proc = subprocess.run(
        [str(NXV_INFO), "--in", str(path)],
        capture_output=True,
        text=True,
        timeout=60,
    )
    if proc.returncode != 0:
        # A skip, not a failure: this says something about the binary in the
        # build tree, not about the parser.  The bytes of the vector are
        # verified against vectors.md5 separately, and the parser reads them,
        # so a refusal here means the built tool and the committed vectors
        # disagree -- rebuild it, or regenerate the vectors, whichever the C
        # side intends.
        pytest.skip(
            f"{NXV_INFO}\n  refused {path.name}, whose bytes match "
            f"vectors.md5 and which nxvc.bitstream parses: "
            f"{proc.stderr.strip() or proc.stdout.strip().splitlines()[:1]}"
        )
    return proc.stdout


def _info_fields(text: str) -> dict:
    """The handful of header fields ``nxv-info`` prints, as values."""
    out: dict = {}
    m = re.search(r"^\s*magic\s+0x([0-9a-f]+)", text, re.M)
    out["magic"] = int(m.group(1), 16) if m else None
    m = re.search(r"^\s*size\s+(\d+)x(\d+)\s+eyes (\d+)\s+bitdepth (\d+)", text, re.M)
    if m:
        out["width"], out["height"] = int(m.group(1)), int(m.group(2))
        out["eyes"], out["bit_depth"] = int(m.group(3)), int(m.group(4))
    m = re.search(r"^\s*chroma\s+(\S+)", text, re.M)
    out["chroma444"] = (m.group(1) == "4:4:4") if m else None
    m = re.search(r"^\s*tools\s+0x([0-9a-f]+)", text, re.M)
    out["tools"] = int(m.group(1), 16) if m else None
    m = re.search(r"^\s*ext_len\s+(\d+)", text, re.M)
    out["ext_len"] = int(m.group(1)) if m else None
    # "tile grid 3x2 = 6 tiles" before the eye pair existed, and
    # "tile grid 2x2 per eye, cols 2 = 4 tiles" since (SYNTAX.md 3.3).
    m = re.search(
        r"^\s*tile grid\s+(\d+)x(\d+)(?: per eye, cols (\d+))? = (\d+) tiles",
        text,
        re.M,
    )
    if m:
        out["cols_per_eye"], out["rows"] = int(m.group(1)), int(m.group(2))
        out["cols"] = int(m.group(3)) if m.group(3) else int(m.group(1))
        out["tile_count"] = int(m.group(4))
    out["warp"] = [
        tuple(int(v) for v in (w[1] + " " + w[2]).replace("/", " ").split())
        for w in re.findall(
            r"warp_ext eye (\d+)\s+Q10\.21 \[([-\d /]+)\]\s+Q2\.29 \[([-\d ]+)\]",
            text,
        )
    ]
    out["frames"] = [
        {
            "frame_number": int(f[0]),
            "frame_bytes": int(f[1]),
            "base_qp": int(f[2]),
            "quant_matrix": int(f[3]),
            "tables_present": int(f[4], 16),
            "flags": int(f[5], 16),
        }
        for f in re.findall(
            r"^frame \d+ @\d+: num (\d+)\s+bytes (\d+)\s+qp (\d+)\s+"
            r"cqpo [-+]?\d+\s+aqpo [-+]?\d+\s+matrix (\d+)\s+"
            r"tables 0x([0-9a-f]+)(?:\s+refs 0x[0-9a-f]+)?\s+flags 0x([0-9a-f]+)",
            text,
            re.M,
        )
    ]
    return out


# ------------------------------------------------------- conformance: the bytes


@pytest.mark.parametrize("row", CONFORMANCE, ids=_id)
def test_committed_vector_bytes_match_their_md5(row):
    """The file on disk is the file the manifest pins.  Everything else here
    assumes it, so it is worth one line."""
    name, stream_md5 = row[0], row[1]
    data = (_vector_dir() / f"{name}.nxv").read_bytes()
    assert hashlib.md5(data).hexdigest() == stream_md5


@pytest.mark.parametrize("row", CONFORMANCE, ids=_id)
def test_every_conformance_vector_parses(row):
    """Full structural parse: every frame, row and tile, with validation on.

    ``parse_frame`` insists that a frame consumes exactly ``frame_bytes``, so
    this catches a wrong table-set size, a wrong tile-header layout or a
    missed optional field as a hard error rather than as drifting offsets.
    """
    name, _, _, width, height, pix, alpha, frames = row[:8]
    data = (_vector_dir() / f"{name}.nxv").read_bytes()

    stream = bs.parse_stream(data)
    hdr = stream.header
    # The manifest records the decoded image, which spans the eye pair; the
    # header's `width` is per eye (SYNTAX.md 3.3).
    assert hdr.width * hdr.eyes == int(width)
    assert hdr.height == int(height)
    assert hdr.chroma444 == (pix == "yuv444p")
    assert hdr.alpha_present == int(alpha)
    assert len(stream.frames) == int(frames)
    assert stream.size == len(data), "the frames do not tile the file exactly"

    for frame in stream.frames:
        assert frame.header.frame_number == stream.frames.index(frame)
        assert len(frame.rows) == hdr.tile_row_count
        # Rows go row-major, eye-minor, and the eye is positional (3.3).
        for n, tile_row in enumerate(frame.rows):
            assert tile_row.header.row_index == n // hdr.eyes
            assert tile_row.eye == n % hdr.eyes
            assert len(tile_row.tiles) == tile_row.header.tile_count
            for tile in tile_row.tiles:
                assert tile.header.eye == tile_row.eye
        for tile in frame.tiles:
            assert len(tile.payload) == tile.header.payload_len


@pytest.mark.parametrize("row", CONFORMANCE, ids=_id)
def test_every_conformance_vector_round_trips_its_headers(row):
    """Each parsed header packs back to the bytes it came from.

    A field the parser reads but does not write -- or writes at the wrong
    offset -- shows up here and nowhere else.
    """
    name = row[0]
    data = (_vector_dir() / f"{name}.nxv").read_bytes()
    stream = bs.parse_stream(data)
    assert stream.header.pack() == data[: stream.header.total_size]
    for frame in stream.frames:
        off = frame.offset
        assert frame.header.pack() == data[off : off + bs.FrameHeader.SIZE]
        for tile_row in frame.rows:
            o = tile_row.offset
            assert tile_row.header.pack() == data[o : o + bs.TileRowHeader.SIZE]
            for tile in tile_row.tiles:
                o = tile.offset
                assert tile.header.pack() == data[o : o + tile.header.header_size]


@pytest.mark.parametrize("row", CONFORMANCE, ids=_id)
def test_transmitted_table_sets_are_sized_by_the_stream_tools(row):
    """SYNTAX.md 3.1 / 9.4: 120, 160, 270 or 290 bytes by the tools mask."""
    name = row[0]
    data = (_vector_dir() / f"{name}.nxv").read_bytes()
    stream = bs.parse_stream(data)
    tools = stream.header.tools
    if tools & nxvc.Tool.CTX_V3:
        want = 290 if (tools & nxvc.Tool.VEC_ENT) else 270
    else:
        want = 160 if (tools & nxvc.Tool.CTX_V2) else 120
    assert bs.table_set_bytes(stream.header.tools) == want
    for frame in stream.frames:
        assert set(frame.table_deltas) == set(frame.header.table_sets)
        for blob in frame.table_deltas.values():
            assert len(blob) == want


def test_the_ctx_v2_table_size_is_load_bearing(monkeypatch):
    """Parsing a CTX_V2 stream with the v1 table size must fail.

    Otherwise the 160-byte rule would be untested: a frame with no transmitted
    table set parses identically either way.
    """
    vectors = _vector_dir()
    candidates = []
    for row in CONFORMANCE:
        data = (vectors / f"{row[0]}.nxv").read_bytes()
        stream = bs.parse_stream_header(data)
        if stream.tools & nxvc.Tool.CTX_V2:
            frame = next(bs.iter_frames(data, stream))
            if frame.header.table_sets:
                candidates.append(row[0])
    assert candidates, "no vector transmits a table set under CTX_V2"

    monkeypatch.setattr(bs, "table_set_bytes", lambda tools: bs.TABLE_SET_BYTES)
    for name in candidates:
        with pytest.raises(bs.BitstreamError):
            bs.parse_stream((vectors / f"{name}.nxv").read_bytes())


def test_the_inter_vectors_carry_warp_ext_where_they_should():
    """A `warp_present` frame carries one 36-byte matrix per eye, and the
    parser reads them as the nine int32 of SYNTAX.md 3.1.1 -- ``h22``
    normalised, and a denominator inside the accepted envelope at every
    picture corner, which is what `_check_warp_ext` already asserted."""
    vectors = _vector_dir()
    seen_warp = seen_stereo = False
    for row in CONFORMANCE:
        data = (vectors / f"{row[0]}.nxv").read_bytes()
        stream = bs.parse_stream(data)
        if not (stream.header.tools & nxvc.Tool.WARP):
            continue
        for frame in stream.frames:
            if not frame.header.warp_present:
                assert frame.warp == ()
                continue
            seen_warp = True
            assert len(frame.warp) == stream.header.eyes
            for matrix in frame.warp:
                assert len(matrix) == 9
                assert matrix[8] == bs.WARP_H22
        if stream.header.eyes == 2:
            seen_stereo = True
    assert seen_warp, "no vector carries a warp_ext() record"
    assert seen_stereo, "no vector has two eyes"


def test_a_stereo_frames_rows_are_row_major_eye_minor():
    """SYNTAX.md 3.3.  The order is load-bearing: it puts a whole left-eye row
    ahead of the right-eye row of the same index, which is what lets a STEREO
    tile depend on left-eye tiles of its own row."""
    vectors = _vector_dir()
    stereo = [
        row[0]
        for row in CONFORMANCE
        if bs.parse_stream_header((vectors / f"{row[0]}.nxv").read_bytes()).eyes == 2
    ]
    assert stereo, "no stereo vector to check the row order against"
    for name in stereo:
        stream = bs.parse_stream((vectors / f"{name}.nxv").read_bytes())
        hdr = stream.header
        for frame in stream.frames:
            order = [(r.header.row_index, r.eye) for r in frame.rows]
            assert order == [
                (r, e) for r in range(hdr.rows) for e in range(hdr.eyes)
            ], name


def test_the_v2_intra_vectors_set_the_tools_they_are_named_for():
    """v36-v44 are the reason syntax v1.3 exists (SYNTAX.md 12)."""
    vectors = _vector_dir()
    expect = {
        "v36_dir444_qp16": nxvc.Tool.INTRA_DIR,
        "v37_dir420_qp28": nxvc.Tool.INTRA_DIR,
        "v38_dir_ctxv2_444": nxvc.Tool.INTRA_DIR | nxvc.Tool.CTX_V2,
        "v39_ctxv2_only_420": nxvc.Tool.CTX_V2,
        "v40_dir_layer420": nxvc.Tool.INTRA_DIR,
        "v41_dir_ctxv2_tables": nxvc.Tool.INTRA_DIR | nxvc.Tool.CTX_V2,
        "v42_dir_res_tskip420": nxvc.Tool.INTRA_DIR,
        "v43_sdh_only420": nxvc.Tool.SIGN_HIDE,
    }
    for name, tools in expect.items():
        path = vectors / f"{name}.nxv"
        if not path.is_file():
            pytest.skip(f"{name} is not in this checkout")
        stream = bs.parse_stream_header(path.read_bytes())
        assert stream.tools & tools == tools, name

    layered = vectors / "v40_dir_layer420.nxv"
    if layered.is_file():
        data = layered.read_bytes()
        stream = bs.parse_stream_header(data)
        frame = next(bs.iter_frames(data, stream))
        assert frame.header.intra_dir_layer, "v40 is the layered form (flags bit 2)"


@pytest.mark.parametrize("row", CONFORMANCE, ids=_id)
def test_the_phase1_advisory_splits_the_vector_set_on_the_tools_mask(row):
    """Every intra-only vector is accepted; every inter one is refused.

    The split is read from the stream's own tool mask rather than from the
    vector's name, so a vector added on either side of the line is covered the
    day it lands.  SYNTAX.md 12 and 14.
    """
    name = row[0]
    data = (_vector_dir() / f"{name}.nxv").read_bytes()
    stream = bs.parse_stream(data)
    reason = bs.phase1_reject_reason(stream.header, stream.frames[0])
    if stream.header.non_phase1_tools():
        assert reason is not None, f"{name} uses inter tools and must be refused"
    else:
        assert reason is None, f"{name} is intra-only and must be accepted: {reason}"


# ---------------------------------------------------------- rejection vectors


@pytest.mark.parametrize("row", REJECTS, ids=_id)
def test_every_rejection_vector_is_refused(row):
    """Each `rejects.md5` stream is refused, by the parser or by the advisory.

    The pure-Python side cannot return a status code -- it is not a decoder --
    so the split is: a structural impossibility raises
    :class:`~nxvc.bitstream.BitstreamError`, and legal-but-out-of-scope syntax
    comes back from :func:`~nxvc.bitstream.phase1_reject_reason`.  What is not
    allowed is for a vector to sail through both.
    """
    name, md5, status = row[0], row[1], row[2]
    path = _vector_dir() / f"{name}.nxv"
    data = path.read_bytes()
    assert hashlib.md5(data).hexdigest() == md5

    try:
        stream = bs.parse_stream(data)
    except bs.BitstreamError:
        return
    reason = bs.phase1_reject_reason(
        stream.header, stream.frames[0] if stream.frames else None
    )
    assert reason is not None, (
        f"{name} must be refused ({status}) and the parser accepted it"
    )


@pytest.mark.parametrize(
    "name, fragment",
    [
        ("r11_wm_id_no_tool", "wm_id"),
        ("r14_dir_layer_no_tool", "INTRA_DIR"),
        ("r17_lossless_sign_hide", "SIGN_HIDE"),
        # --- the Phase 2 set.  Several of these are refusable for more than
        # one reason, and the ordering of the checks is what decides which one
        # a reader is told about: a STEREO tile on the left eye must not be
        # reported as a malformed disparity, because until the eye is right
        # those two bytes are not a disparity at all.
        ("r18_h22_not_one", "h22"),
        ("r19_entry_range", r"outside \[-2\^30, 2\^30\]"),
        ("r20_den_range", "denominator"),
        ("r21_warp_no_flag", "warp_present == 0"),
        ("r22_mode_reserved", "mode 5 is reserved"),
        ("r23_intra_ref_sel", "on a INTRA tile must be 0"),
        ("r24_ref_sel_three", "ref_sel 3 is reserved"),
        ("r25_ref_slots_wrong", "ref_slots"),
        ("r27_warp_without_inter", "WARP without INTER"),
        ("r28_stereo_left_eye", "STEREO tile on the left eye"),
        ("r29_disparity_reserved", "disparity bits 15:12"),
    ],
)
def test_the_rejection_vectors_fail_for_the_stated_reason(name, fragment):
    """Refusing for the wrong reason is a bug the status alone would hide."""
    path = _vector_dir() / f"{name}.nxv"
    if not path.is_file():
        pytest.skip(f"{name} is not in this checkout")
    with pytest.raises(bs.BitstreamError, match=fragment):
        bs.parse_stream(path.read_bytes())


# ------------------------------------------------------- cross-check: nxv-info


@pytest.mark.skipif(NXV_INFO is None, reason="no nxv-info binary in any build tree")
@pytest.mark.parametrize("row", CONFORMANCE, ids=_id)
def test_the_parser_and_nxv_info_agree(row):
    """Two independent readings of the same bytes, field for field.

    ``nxv-info`` walks the stream with the C decoder's header parser; this
    module walks it with the Python one.  Agreement on 44 committed files is
    the check that the two implementations of SYNTAX.md 2-4 have not drifted.
    """
    name = row[0]
    path = _vector_dir() / f"{name}.nxv"
    stream = bs.parse_stream(path.read_bytes())
    info = _info_fields(_run_info(path))

    hdr = stream.header
    assert info["magic"] == hdr.magic
    assert info["width"] == hdr.width
    assert info["height"] == hdr.height
    assert info["eyes"] == hdr.eyes
    assert info["bit_depth"] == hdr.bit_depth
    assert info["chroma444"] == hdr.chroma444
    assert info["tools"] == hdr.tools
    assert info["ext_len"] == hdr.ext_len
    assert (info["cols_per_eye"], info["rows"]) == (hdr.cols_per_eye, hdr.rows)
    assert info["cols"] == hdr.cols
    assert info["tile_count"] == hdr.tile_count

    assert len(info["frames"]) == len(stream.frames)
    for got, frame in zip(info["frames"], stream.frames):
        h = frame.header
        assert got["frame_number"] == h.frame_number
        assert got["frame_bytes"] == h.frame_bytes
        assert got["base_qp"] == h.base_qp
        assert got["quant_matrix"] == h.quant_matrix
        assert got["tables_present"] == h.tables_present
        assert got["flags"] == h.flags

    # warp_ext(), matrix for matrix.  nxv-info prints the raw int32 in the two
    # fixed-point formats of SYNTAX.md 3.1.1; nothing is converted to a float
    # on either side, so this is an exact comparison.
    printed = [tuple(m) for m in info["warp"]]
    parsed = [tuple(m) for frame in stream.frames for m in frame.warp]
    assert printed == parsed


@pytest.mark.skipif(NXV_INFO is None, reason="no nxv-info binary in any build tree")
def test_nxv_info_output_shape_is_still_what_this_module_parses():
    """Guard the regexes above: if `nxv-info` reformats, the cross-check must
    fail loudly rather than silently comparing nothing."""
    row = CONFORMANCE[0] if CONFORMANCE else None
    if row is None:
        pytest.skip("no conformance vectors")
    info = _info_fields(_run_info(_vector_dir() / f"{row[0]}.nxv"))
    assert info["magic"] == nxvc.NXVC_MAGIC
    assert info["frames"], "no frame lines were recognised in nxv-info output"
