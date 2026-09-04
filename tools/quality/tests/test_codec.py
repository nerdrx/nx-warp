"""The codec CLI wrapper and the dummy codec that stands in for it."""

from __future__ import annotations

import os
import subprocess
import sys

import numpy as np
import pytest

import dummy_codec
from nxq import yuv
from nxq.codec import CodecCLI, CodecError

DUMMY = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dummy_codec.py")


@pytest.fixture
def seq(tmp_path):
    """A small noisy sequence with real structure, so rate responds to QP."""
    rng = np.random.default_rng(42)
    fmt = yuv.Format(64, 32, "yuv444p")
    frames = []
    for i in range(3):
        y = np.clip(
            np.linspace(0, 255, 64)[None, :] + rng.normal(0, 20, (32, 64)), 0, 255
        ).astype(np.uint8)
        frames.append(yuv.Frame(y, np.full((32, 64), 110 + i, np.uint8),
                                np.full((32, 64), 140, np.uint8)))
    path = str(tmp_path / "src.yuv")
    yuv.write_sequence(path, fmt, frames)
    return path, fmt, frames


class TestCodecCLI:
    def test_prefix_expands_to_enc_and_dec(self):
        c = CodecCLI.from_args("nxv")
        assert c.enc == ["nxv-enc"]
        assert c.dec == ["nxv-dec"]

    def test_prefix_with_a_path(self):
        c = CodecCLI.from_args("/opt/build/nxv")
        assert c.enc == ["/opt/build/nxv-enc"]
        assert c.name == "nxv"

    def test_prefix_with_a_launcher(self):
        c = CodecCLI.from_args("wine build/nxv")
        assert c.enc == ["wine", "build/nxv-enc"]

    def test_explicit_commands(self):
        c = CodecCLI.from_args(None, "python3 d.py enc", "python3 d.py dec")
        assert c.enc == ["python3", "d.py", "enc"]
        assert c.dec == ["python3", "d.py", "dec"]

    def test_explicit_needs_both(self):
        with pytest.raises(CodecError, match="must be given together"):
            CodecCLI.from_args(None, "python3 d.py enc", None)

    def test_default_is_nxv(self):
        assert CodecCLI.from_args().enc == ["nxv-enc"]

    def test_missing_binary_reports_unavailable(self):
        c = CodecCLI.from_args("definitely-not-installed-xyz")
        ok, why = c.available()
        assert not ok and "not found" in why

    def test_require_points_at_the_mock(self):
        c = CodecCLI.from_args("definitely-not-installed-xyz")
        with pytest.raises(CodecError, match="dummy_codec.py"):
            c.require()


class TestDummyCodecUnit:
    def test_qp_step_ladder_doubles_every_six(self):
        assert dummy_codec.qp_to_step(0) == pytest.approx(1.0)
        assert dummy_codec.qp_to_step(6) == pytest.approx(2.0)
        assert dummy_codec.qp_to_step(12) == pytest.approx(4.0)

    def test_roundtrip_geometry(self, seq, tmp_path):
        src, fmt, _ = seq
        bs = str(tmp_path / "a.nxv")
        out = str(tmp_path / "a.yuv")
        dummy_codec.encode(src, fmt, 12, bs)
        dummy_codec.decode(bs, out)
        assert os.path.getsize(out) == os.path.getsize(src)
        assert fmt.frame_count(out) == 3

    def test_qp0_is_lossless(self, seq, tmp_path):
        src, fmt, _ = seq
        bs = str(tmp_path / "a.nxv")
        out = str(tmp_path / "a.yuv")
        dummy_codec.encode(src, fmt, 0, bs)
        dummy_codec.decode(bs, out)
        assert open(src, "rb").read() == open(out, "rb").read()

    def test_rate_and_quality_both_fall_with_qp(self, seq, tmp_path):
        from nxq import metrics
        src, fmt, _ = seq
        sizes, psnrs = [], []
        for qp in (6, 18, 30):
            bs = str(tmp_path / f"q{qp}.nxv")
            out = str(tmp_path / f"q{qp}.yuv")
            sizes.append(dummy_codec.encode(src, fmt, qp, bs))
            dummy_codec.decode(bs, out)
            ref = list(yuv.read_sequence(src, fmt))
            dis = list(yuv.read_sequence(out, fmt))
            psnrs.append(np.mean([metrics.psnr_plane(a.y, b.y) for a, b in zip(ref, dis)]))
        assert sizes[0] > sizes[1] > sizes[2], "bitrate must fall as QP rises"
        assert psnrs[0] > psnrs[1] > psnrs[2], "quality must fall as QP rises"

    def test_420_roundtrip(self, tmp_path):
        fmt = yuv.Format(32, 16, "yuv420p")
        src = str(tmp_path / "s.yuv")
        yuv.write_sequence(src, fmt, [yuv.Frame.gray(fmt, 90)] * 2)
        bs, out = str(tmp_path / "s.nxv"), str(tmp_path / "o.yuv")
        dummy_codec.encode(src, fmt, 6, bs)
        dummy_codec.decode(bs, out)
        assert fmt.frame_count(out) == 2

    def test_rejects_foreign_bitstream(self, tmp_path):
        bad = tmp_path / "bad.nxv"
        bad.write_bytes(b"XXXX" + b"\x00" * 32)
        with pytest.raises(SystemExit, match="not a dummy-codec stream"):
            dummy_codec.decode(str(bad), str(tmp_path / "o.yuv"))


class TestDummyCodecCLI:
    def test_end_to_end_through_the_wrapper(self, seq, tmp_path):
        src, fmt, _ = seq
        cli = CodecCLI.from_args(
            None, f"{sys.executable} {DUMMY} enc", f"{sys.executable} {DUMMY} dec", "dummy"
        )
        ok, why = cli.available()
        assert ok, why
        bs = str(tmp_path / "c.nxv")
        out = str(tmp_path / "c.yuv")
        size = cli.encode(src, fmt, 18, bs)
        assert size > 0
        cli.decode(bs, out)
        assert fmt.frame_count(out) == 3

    def test_argparse_contract_matches_the_spec(self, seq, tmp_path):
        """The exact command line nxv-enc must accept."""
        src, fmt, _ = seq
        out = str(tmp_path / "x.nxv")
        r = subprocess.run(
            [sys.executable, DUMMY, "enc", "--in", src, "--w", "64", "--h", "32",
             "--pix", "yuv444p", "--qp", "20", "--out", out],
            capture_output=True, text=True,
        )
        assert r.returncode == 0, r.stderr
        assert os.path.exists(out)
        r2 = subprocess.run(
            [sys.executable, DUMMY, "dec", "--in", out, "--out", str(tmp_path / "x.yuv")],
            capture_output=True, text=True,
        )
        assert r2.returncode == 0, r2.stderr

    def test_encoder_failure_surfaces_as_codec_error(self, seq, tmp_path):
        src, fmt, _ = seq
        cli = CodecCLI.from_args(
            None, f"{sys.executable} {DUMMY} enc", f"{sys.executable} {DUMMY} dec", "dummy"
        )
        with pytest.raises(CodecError, match="encode failed"):
            cli.encode(src, fmt, 999, str(tmp_path / "z.nxv"))
