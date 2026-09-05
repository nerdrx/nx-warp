"""Bjontegaard delta tests.

The strongest tests here are the analytic invariants.  For the standard cubic
method both of these are *exact*, not approximate, so they pin the
implementation down without needing a magic number from a spreadsheet:

* scaling every rate by ``k`` at unchanged quality must give
  ``BD-rate = (k - 1) * 100`` percent exactly;
* shifting every PSNR by ``d`` at unchanged rates must give
  ``BD-PSNR = d`` exactly.

They hold because a cubic fit of a shifted dataset is the same cubic shifted by
the same constant, and the integral of a constant over the overlap divided by
the overlap width is that constant.

Note the asymmetry: each invariant applies to the figure whose fit runs along
the shifted axis.  Scaling rates does *not* leave BD-PSNR at zero, because
BD-PSNR integrates over the overlapping rate range and the scaled curve really
is worse at matched rate.  Asserting otherwise would be asserting a bug.

On top of those there is a cross-check against an independently written
integrator (fine-grained trapezoid instead of the analytic ``polyint``), and a
regression case on a published-style four-point dataset.
"""

from __future__ import annotations

import numpy as np
import pytest

from nxq import bdrate

# numpy renamed ``trapz`` to ``trapezoid`` in 2.0 and kept ``trapz`` as a
# deprecated alias; 1.x has only ``trapz``.  CI runs both (the C++ jobs use the
# distribution's python3-numpy 1.26, the python job pip-installs 2.x), so bind
# whichever exists rather than pinning the harness to one numpy major.
_trapz = getattr(np, "trapezoid", None) or np.trapz

# A representative four-point RD dataset (rate in kbit/s, PSNR in dB).
RATE_A = [9487.76, 4593.60, 2486.44, 1358.24]
PSNR_A = [40.037, 38.615, 36.845, 34.851]
RATE_B = [9787.80, 4469.00, 2451.52, 1356.24]
PSNR_B = [40.121, 38.717, 36.887, 34.879]


def _independent_bd_rate(r1, d1, r2, d2, n=200001):
    """A deliberately different implementation: fit, then integrate numerically."""
    r1, d1 = np.asarray(r1, float), np.asarray(d1, float)
    r2, d2 = np.asarray(r2, float), np.asarray(d2, float)
    o1, o2 = np.argsort(d1), np.argsort(d2)
    p1 = np.polyfit(d1[o1], np.log10(r1[o1]), 3)
    p2 = np.polyfit(d2[o2], np.log10(r2[o2]), 3)
    lo = max(d1.min(), d2.min())
    hi = min(d1.max(), d2.max())
    xs = np.linspace(lo, hi, n)
    a1 = _trapz(np.polyval(p1, xs), xs) / (hi - lo)
    a2 = _trapz(np.polyval(p2, xs), xs) / (hi - lo)
    return (10.0 ** (a2 - a1) - 1.0) * 100.0


class TestAnalyticInvariants:
    @pytest.mark.parametrize("k", [0.5, 0.8, 1.0, 1.25, 2.0])
    def test_uniform_rate_scaling_gives_exact_bd_rate(self, k):
        scaled = [r * k for r in RATE_A]
        got = bdrate.bd_rate(RATE_A, PSNR_A, scaled, PSNR_A)
        assert got == pytest.approx((k - 1.0) * 100.0, abs=1e-8)

    def test_rate_scaling_moves_bd_psnr_the_right_way(self):
        """BD-PSNR is *not* zero under rate scaling, and that is correct.

        BD-PSNR integrates over the overlapping *rate* range. Scaling rates by
        k shifts the curve along log-rate, so within the overlap the shifted
        curve really does sit lower (k > 1: more bits for the same quality) or
        higher (k < 1). Only BD-rate is invariant here; asserting BD-PSNR == 0
        would be asserting a bug.
        """
        cheaper = bdrate.bd_psnr(RATE_A, PSNR_A, [r * 0.5 for r in RATE_A], PSNR_A)
        same = bdrate.bd_psnr(RATE_A, PSNR_A, list(RATE_A), PSNR_A)
        pricier = bdrate.bd_psnr(RATE_A, PSNR_A, [r * 2.0 for r in RATE_A], PSNR_A)
        assert cheaper > 0
        assert same == pytest.approx(0.0, abs=1e-9)
        assert pricier < 0
        assert cheaper > same > pricier

    def test_psnr_shift_moves_bd_rate_the_right_way(self):
        """The dual statement: a pure quality gain shows up as a rate saving."""
        better = [p + 1.0 for p in PSNR_A]
        assert bdrate.bd_rate(RATE_A, PSNR_A, RATE_A, better) < 0

    @pytest.mark.parametrize("d", [-1.5, -0.25, 0.0, 0.5, 2.0])
    def test_uniform_psnr_shift_gives_exact_bd_psnr(self, d):
        shifted = [p + d for p in PSNR_A]
        got = bdrate.bd_psnr(RATE_A, PSNR_A, RATE_A, shifted)
        assert got == pytest.approx(d, abs=1e-9)

    def test_identical_curves_are_zero(self):
        assert bdrate.bd_rate(RATE_A, PSNR_A, RATE_A, PSNR_A) == pytest.approx(0.0, abs=1e-9)
        assert bdrate.bd_psnr(RATE_A, PSNR_A, RATE_A, PSNR_A) == pytest.approx(0.0, abs=1e-9)


class TestCrossCheck:
    def test_matches_an_independent_integrator(self):
        mine = bdrate.bd_rate(RATE_A, PSNR_A, RATE_B, PSNR_B)
        theirs = _independent_bd_rate(RATE_A, PSNR_A, RATE_B, PSNR_B)
        assert mine == pytest.approx(theirs, abs=1e-4)

    def test_reference_dataset_regression(self):
        """The two curves are near-identical, so BD-rate must be small."""
        bd = bdrate.bd_rate(RATE_A, PSNR_A, RATE_B, PSNR_B)
        assert abs(bd) < 5.0
        # and it is stable: recomputing gives the same answer
        assert bd == pytest.approx(bdrate.bd_rate(RATE_A, PSNR_A, RATE_B, PSNR_B))

    def test_antisymmetry_of_bd_psnr(self):
        fwd = bdrate.bd_psnr(RATE_A, PSNR_A, RATE_B, PSNR_B)
        rev = bdrate.bd_psnr(RATE_B, PSNR_B, RATE_A, PSNR_A)
        assert fwd == pytest.approx(-rev, abs=1e-9)


class TestSignConventions:
    def test_fewer_bits_is_negative_bd_rate(self):
        cheaper = [r * 0.7 for r in RATE_A]
        assert bdrate.bd_rate(RATE_A, PSNR_A, cheaper, PSNR_A) < 0

    def test_more_quality_is_positive_bd_psnr(self):
        better = [p + 1.0 for p in PSNR_A]
        assert bdrate.bd_psnr(RATE_A, PSNR_A, RATE_A, better) > 0


class TestValidation:
    def test_needs_four_points(self):
        with pytest.raises(ValueError, match="at least 4"):
            bdrate.bd_rate(RATE_A[:3], PSNR_A[:3], RATE_B[:3], PSNR_B[:3])

    def test_rejects_nonpositive_rates(self):
        bad = [0.0] + RATE_A[1:]
        with pytest.raises(ValueError, match="positive"):
            bdrate.bd_rate(bad, PSNR_A, RATE_B, PSNR_B)

    def test_rejects_infinite_distortion(self):
        bad = [float("inf")] + PSNR_A[1:]
        with pytest.raises(ValueError, match="finite"):
            bdrate.bd_rate(RATE_A, bad, RATE_B, PSNR_B)

    def test_mismatched_lengths(self):
        with pytest.raises(ValueError, match="same length"):
            bdrate.bd_rate(RATE_A, PSNR_A[:3], RATE_B, PSNR_B)

    def test_non_overlapping_curves_explain_themselves(self):
        far = [p + 40.0 for p in PSNR_A]
        with pytest.raises(ValueError, match="do not overlap"):
            bdrate.bd_rate(RATE_A, PSNR_A, RATE_B, far)

    def test_summary_reports_errors_instead_of_raising(self):
        """Disjoint quality ranges kill BD-rate; the rates still overlap, so
        BD-PSNR survives and is reported rather than discarded."""
        far = [p + 40.0 for p in PSNR_A]
        out = bdrate.bd_summary(RATE_A, PSNR_A, RATE_B, far)
        assert "bd_rate_pct" not in out
        assert "bd_rate_error" in out
        assert "bd_psnr_db" in out

    def test_summary_happy_path(self):
        out = bdrate.bd_summary(RATE_A, PSNR_A, RATE_B, PSNR_B)
        assert set(out) >= {"bd_rate_pct", "bd_psnr_db", "overlap_lo", "overlap_hi", "method"}
        assert out["overlap_lo"] < out["overlap_hi"]


class TestPchip:
    def test_pchip_agrees_roughly_with_cubic(self):
        pytest.importorskip("scipy")
        c = bdrate.bd_rate(RATE_A, PSNR_A, RATE_B, PSNR_B, method="cubic")
        p = bdrate.bd_rate(RATE_A, PSNR_A, RATE_B, PSNR_B, method="pchip")
        assert p == pytest.approx(c, abs=2.0)

    def test_pchip_also_exact_under_rate_scaling(self):
        pytest.importorskip("scipy")
        scaled = [r * 1.5 for r in RATE_A]
        got = bdrate.bd_rate(RATE_A, PSNR_A, scaled, PSNR_A, method="pchip")
        assert got == pytest.approx(50.0, abs=1e-6)

    def test_unknown_method(self):
        with pytest.raises(ValueError, match="unknown method"):
            bdrate.bd_rate(RATE_A, PSNR_A, RATE_B, PSNR_B, method="spline")


class TestPartialSummary:
    """BD-rate and BD-PSNR need overlap on different axes, so one can survive."""

    def test_quality_overlap_without_rate_overlap_keeps_bd_rate(self):
        # Same quality range, disjoint rate ranges.
        anchor_r = [100.0, 70.0, 45.0, 28.0]
        test_r = [900.0, 600.0, 400.0, 260.0]
        d = [44.0, 41.0, 38.0, 35.0]
        out = bdrate.bd_summary(anchor_r, d, test_r, d)
        assert "bd_rate_pct" in out
        assert "bd_psnr_db" not in out
        assert "bd_psnr_error" in out
        assert "error" not in out

    def test_rate_error_message_uses_real_units_not_log10(self):
        anchor_r = [100.0, 70.0, 45.0, 28.0]
        test_r = [900.0, 600.0, 400.0, 260.0]
        d = [44.0, 41.0, 38.0, 35.0]
        msg = bdrate.bd_summary(anchor_r, d, test_r, d)["bd_psnr_error"]
        assert "in rate" in msg
        assert "900" in msg and "28" in msg
        assert "2.95" not in msg  # log10(900), the old confusing output

    def test_quality_error_message_says_quality(self):
        r = [100.0, 70.0, 45.0, 28.0]
        out = bdrate.bd_summary(r, [44.0, 41.0, 38.0, 35.0], r, [84.0, 81.0, 78.0, 75.0])
        assert "in quality" in out["bd_rate_error"]

    def test_neither_available_sets_error(self):
        anchor_r = [100.0, 70.0, 45.0, 28.0]
        test_r = [9000.0, 6000.0, 4000.0, 2600.0]
        out = bdrate.bd_summary(anchor_r, [44.0, 41.0, 38.0, 35.0],
                                test_r, [84.0, 81.0, 78.0, 75.0])
        assert "error" in out
        assert "bd_rate_pct" not in out and "bd_psnr_db" not in out
