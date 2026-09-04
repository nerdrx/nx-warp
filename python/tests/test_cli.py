"""``python -m nxvc`` -- the parts that run without the library, and the rest."""

from __future__ import annotations

import json

import numpy as np
import pytest

import nxvc
from nxvc.cli import build_parser, main

from conftest import make_bitstream, requires_library


def test_help_without_a_command_is_not_a_crash(capsys):
    assert main([]) == 2
    assert "usage" in capsys.readouterr().out.lower()


def test_probe_reports_the_backends(capsys):
    code = main(["probe"])
    out = capsys.readouterr().out
    assert "library available" in out
    assert "metrics backend" in out
    assert code == (0 if nxvc.NXVC_AVAILABLE else 1)


def test_info_runs_without_the_library(tmp_path, capsys):
    path = tmp_path / "hand.nxv"
    path.write_bytes(make_bitstream(width=128, height=128, frames=2))
    assert main(["info", str(path), "--tiles"]) == 0
    out = capsys.readouterr().out
    assert "stream header (64 bytes)" in out
    assert "tile grid     2x2 = 4 tiles" in out
    assert "2 frame(s)" in out
    assert out.count("INTRA") >= 8


def test_info_accepts_the_nxv_info_spelling(tmp_path, capsys):
    path = tmp_path / "hand.nxv"
    path.write_bytes(make_bitstream())
    assert main(["info", "--in", str(path)]) == 0
    assert "1 frame(s)" in capsys.readouterr().out


def test_info_json(tmp_path, capsys):
    path = tmp_path / "hand.nxv"
    path.write_bytes(make_bitstream(width=128, height=64))
    assert main(["info", str(path), "--json"]) == 0
    doc = json.loads(capsys.readouterr().out)
    assert doc["stream"]["width"] == 128
    assert doc["stream"]["tile_count"] == 2
    assert doc["frames"][0]["tile_count"] == 2
    assert doc["error"] is None


def test_info_on_junk_fails_cleanly(tmp_path, capsys):
    path = tmp_path / "junk.nxv"
    path.write_bytes(b"not a stream header at all" * 4)
    assert main(["info", str(path)]) == 2
    assert "bad magic" in capsys.readouterr().err


def test_info_reports_a_phase2_stream_as_refusable(tmp_path, capsys):
    from nxvc import bitstream as bs

    hdr = bs.StreamHeader(
        width=64, height=64, tools=nxvc.Tool.INTRA_DC_PLANE | nxvc.Tool.WARP
    )
    path = tmp_path / "phase2.nxv"
    path.write_bytes(hdr.pack())
    assert main(["info", str(path)]) == 0
    out = capsys.readouterr().out
    assert "unsupported by the Phase 1 decoder" in out
    assert "would be refused" in out


def test_info_frames_limit(tmp_path, capsys):
    path = tmp_path / "hand.nxv"
    path.write_bytes(make_bitstream(frames=5))
    assert main(["info", str(path), "--frames", "2"]) == 0
    assert "2 frame(s)" in capsys.readouterr().out


def test_encode_without_the_library_says_how_to_get_one(tmp_path):
    if nxvc.NXVC_AVAILABLE:
        pytest.skip("the library is present, so this path cannot be exercised")
    with pytest.raises(SystemExit) as exc:
        main(["encode", "--in", "x.yuv", "--out", "y.nxv", "--w", "64", "--h", "64"])
    assert "shared library" in str(exc.value)


# ------------------------------------------------------------ with the library


@requires_library
def test_encode_decode_round_trip_through_the_cli(tmp_path, capsys):
    w = h = 128
    rng = np.random.default_rng(0)
    yy, xx = np.mgrid[0:h, 0:w]
    y = ((xx + yy) % 256).astype(np.uint8)
    u = np.full((h // 2, w // 2), 110, np.uint8)
    v = np.full((h // 2, w // 2), 140, np.uint8)
    src = tmp_path / "src.yuv"
    nxvc.write_planar_yuv(src, [[y, u, v]])

    nxv = tmp_path / "out.nxv"
    assert main(
        [
            "encode", "--in", str(src), "--out", str(nxv),
            "--w", str(w), "--h", str(h), "--pix", "yuv420p", "--qp", "18",
        ]
    ) == 0
    assert nxv.stat().st_size > 64

    out = tmp_path / "out.yuv"
    assert main(["decode", "--in", str(nxv), "--out", str(out), "--pix", "yuv420p"]) == 0
    back = nxvc.read_planar_yuv(out, w, h, "yuv420p")
    assert len(back) == 1
    assert nxvc.metrics.psnr(y, back[0][0]) > 30.0

    # and `info` on a real stream agrees with the encoder
    capsys.readouterr()
    assert main(["info", str(nxv), "--library"]) == 0
    text = capsys.readouterr().out
    assert "1 frame(s)" in text
    assert "reference decoder: 1 frame(s) decoded cleanly" in text


@requires_library
def test_decode_pix_mismatch_is_refused(tmp_path):
    w = h = 64
    src = tmp_path / "src.yuv"
    nxvc.write_planar_yuv(
        src, [[np.zeros((h, w), np.uint8)] + [np.zeros((h // 2, w // 2), np.uint8)] * 2]
    )
    nxv = tmp_path / "out.nxv"
    main(["encode", "--in", str(src), "--out", str(nxv), "--w", str(w), "--h", str(h),
          "--quiet"])
    assert main(["decode", "--in", str(nxv), "--out", str(tmp_path / "o.yuv"),
                 "--pix", "yuv444p"]) == 2


@requires_library
def test_encode_with_a_qp_map_file(tmp_path):
    w = h = 128
    src = tmp_path / "src.yuv"
    plane = np.random.default_rng(1).integers(0, 256, (h, w), dtype=np.uint8)
    nxvc.write_planar_yuv(src, [[plane, plane, plane]])
    qp_map = tmp_path / "qp.map"
    qp_map.write_bytes(bytes([10, 20, 30, 40]))  # 2x2 tiles

    nxv = tmp_path / "out.nxv"
    assert main(
        ["encode", "--in", str(src), "--out", str(nxv), "--w", str(w), "--h", str(h),
         "--pix", "yuv444p", "--qp-map", str(qp_map), "--quiet"]
    ) == 0
    from nxvc import bitstream as bs

    parsed = bs.parse_stream(nxv.read_bytes())
    qps = [t.header.resolved_qp(parsed.frames[0].header.base_qp) for t in parsed.frames[0].tiles]
    assert qps == [10, 20, 30, 40]


@requires_library
def test_encode_rejects_rgb_with_420(tmp_path):
    assert main(
        ["encode", "--in", "x", "--out", "y", "--w", "64", "--h", "64", "--rgb"]
    ) == 2


# ------------------------------------------------- the nxv-enc flag mirror


def test_encode_flags_mirror_nxv_enc():
    """Every tuning flag exists, and every one of them defaults to *unset*.

    A default of 0 rather than None would be a real bug: it would turn the RD
    trellis and the v2 intra tools off for anyone who never mentioned them,
    because `nxvc_config_default()` turns them on.
    """
    parser = build_parser()
    args = parser.parse_args(
        ["encode", "--in", "a.yuv", "--out", "b.nxv", "--w", "64", "--h", "64"]
    )
    for name in ("rdo", "rdo_lambda", "wm", "intra_dir", "ctx", "sign_hide"):
        assert getattr(args, name) is None, name
    assert args.qp_search == 0
    assert args.intra_dir_cand == 0

    args = parser.parse_args(
        [
            "encode", "--in", "a.yuv", "--out", "b.nxv", "--w", "64", "--h", "64",
            "--no-rdo", "--rdo-lambda", "0.5", "--qp-search", "2", "--wm", "auto",
            "--intra-dir", "layer", "--intra-dir-cand", "4", "--ctx", "v2",
            "--no-sign-hide",
        ]
    )
    assert args.rdo is False
    assert args.rdo_lambda == 0.5
    assert args.qp_search == 2
    assert args.wm == "auto"
    assert args.intra_dir == "layer"
    assert args.intra_dir_cand == 4
    assert args.ctx == "v2"
    assert args.sign_hide is False


def test_probe_reports_the_syntax_revision(capsys):
    main(["probe"])
    out = capsys.readouterr().out
    assert f"v1.{nxvc.NXVC_BITSTREAM_MINOR}" in out
