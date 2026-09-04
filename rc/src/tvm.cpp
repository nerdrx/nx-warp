// The Tursun-Didyk peripheral temporal visibility model, reduced onto the
// per-tile statistics the encoder already computes.
//
// Reference: Tursun and Didyk, ACM TOG 41(6) 2022, arXiv 2205.00108.
// Every fitted constant below is theirs; every clamp and every reduction
// step is ours and is marked APPROX.  docs/RATECONTROL.md 8.2 is the list.
//
// SPDX-License-Identifier: Apache-2.0

#include "nxrc/tvm.hpp"

#include <algorithm>
#include <cmath>

namespace nxrc::tvm {

namespace {

constexpr float kPi = 3.14159265358979f;

// The De Lange polynomial's argument.
//
// APPROX 1, and the largest single judgement in this file.  The paper prints
//     S_DL(f_t) = sum_{i=0..3} a_i (f_t)^i
// with a = (3.2714, 0.3830, 0.7669, -0.2555).  Read with f_t in raw Hz that
// cubic is 3.27 at DC, peaks at 0.5 Hz and crosses zero at 4.5 Hz, after
// which the soft-plus pins it at zero: a model in which nothing above 5 Hz
// is ever visible.  That is not a De Lange curve and it is not what the
// paper's own figures show (band-pass, peak near 10 Hz, critical flicker
// fusion in the 60-70 Hz region).  A log-frequency argument reproduces
// exactly that shape from the same four coefficients:
//
//     x = ln(1 + f_t):  S_DL = 3.27 at 0 Hz, 5.08 at 10 Hz (peak),
//                       2.64 at 36 Hz, ~0.26 at 72 Hz.
//
// We therefore take the argument to be log-frequency.  This is an inference
// from the coefficients and the stated curve family, not something read off
// the paper, and it is the first thing to check against the authors' code.
// The scheduler's behaviour is qualitatively robust to it - what the
// scheduler needs is that S falls steeply above ~30 Hz, which is why a 1/2
// rate step is nearly free and a 1/6 rate step is not - but the absolute
// C_M numbers in rc/RESULTS-temporal.md all depend on it.
float ft_argument(float f_t_hz, const ModelParams& p) {
    const float v = std::log(1.0f + std::max(f_t_hz, 0.0f));
    if (p.ft_log_base <= 0.0f) return v;
    return v / std::log(p.ft_log_base);
}

float softplus(float x) {
    // Numerically stable: ln(1+e^x) = max(x,0) + ln(1 + e^-|x|).
    return std::max(x, 0.0f) + std::log1p(std::exp(-std::fabs(x)));
}

} // namespace

// ---------------------------------------------------------------- pieces --

float sensitivity_temporal(float f_t_hz, const ModelParams& p) {
    const float x = ft_argument(f_t_hz, p);
    const float s = p.a[0] + x * (p.a[1] + x * (p.a[2] + x * p.a[3]));
    return softplus(s);
}

float sensitivity_scale(float f_s_cpd, float e_deg, const ModelParams& p) {
    // Domain clamps, APPROX 2.  T is negative above f_s ~ 5.5 cpd and the
    // exponent q changes sign at 5.75 cpd; both are outside the fit and both
    // would make the model non-monotone in eccentricity.  Clamping f_s at
    // 4.0 keeps T positive and q positive for every eccentricity, which is
    // what makes "further out is more visible, at equal contrast and equal
    // spatial frequency" a theorem rather than a hope.
    const float fs = std::clamp(f_s_cpd, 0.0f, p.fs_max_cpd);
    const float e  = std::clamp(e_deg, p.ecc_min_deg, p.ecc_max_deg);

    const float q = p.b51 * fs * fs + p.b52 * fs + p.b53;
    const float t = p.b1 - p.b2 * std::pow(fs, p.b3) + p.b4 * std::pow(e, q);
    return std::max(t, p.sens_floor);
}

float sensitivity(float f_t_hz, float f_s_cpd, float e_deg,
                  const ModelParams& p) {
    // U(f_t, f_s, e) = f_t - b6 + b7 f_s + b8 e; b6 = b7 = b8 = 0 in the
    // published calibration, so U is the identity.  Kept explicit so a
    // re-fit that gives them values does not need a code change.
    const float fs = std::clamp(f_s_cpd, 0.0f, p.fs_max_cpd);
    const float e  = std::clamp(e_deg, p.ecc_min_deg, p.ecc_max_deg);
    const float u  = f_t_hz - p.b6 + p.b7 * fs + p.b8 * e;
    return sensitivity_scale(fs, e, p) * sensitivity_temporal(u, p);
}

float detect_prob(float c_m, const ModelParams& p) {
    // APPROX 3.  The printed psychometric function,
    //     P = p_g + (p_g - 1)(1 - p_l) / exp[-(C_M/b0)^b1 - 1]
    // does not evaluate to a probability (it is below p_g for every positive
    // C_M and does not tend to 1).  We implement the standard Weibull with
    // the paper's p_g, p_l, beta0 and beta1, which is the function that
    // family of parameters describes:
    //     P = p_g + (1 - p_g)(1 - p_l) * [1 - exp(-(C_M/beta0)^beta1)]
    // At the paper's JND unit C_M = 1 this gives P = 0.67 rather than the
    // stated 0.75; the discrepancy is carried, not tuned away, and only
    // affects the reported probabilities, never the ordering.  The scheduler
    // itself gates on C_M and never on P.
    const float c = std::max(c_m, 0.0f);
    const float w = 1.0f - std::exp(-std::pow(c / std::max(p.beta0, 1e-6f),
                                              p.beta1));
    return p.p_g + (1.0f - p.p_g) * (1.0f - p.p_l) * w;
}

float visibility(float c_m, const ModelParams& p) {
    const float d = 1.0f - p.p_g;
    if (d <= 0.0f) return 0.0f;
    return std::clamp((detect_prob(c_m, p) - p.p_g) / d, 0.0f, 1.0f);
}

float luma_to_nits(float code, const ModelParams& p) {
    const float c = std::clamp(code, 0.0f, 255.0f) / 255.0f;
    return p.display_peak_nits * std::pow(c, p.display_gamma);
}

float spatial_freq_cpd(float freq_ratio_R, float ppd_render) {
    // R = E[sin^2(2 pi f_x)] + E[sin^2(2 pi f_y)] over the tile's spectrum,
    // in cycles/sample; it is exactly 1.0 for white noise (RATECONTROL.md
    // 3.2).  Under an isotropic single-frequency assumption both terms are
    // equal, so sin^2(2 pi f) = R/2 and
    //     f = asin(sqrt(R/2)) / (2 pi)   cycles/sample.
    // APPROX 4: one representative frequency stands in for the tile's whole
    // spectrum.  The paper pools over a real DCT; we pool over the temporal
    // harmonics only.
    const float rr = std::clamp(freq_ratio_R, 0.0f, 2.0f);
    const float f_cyc_per_sample = std::asin(std::sqrt(rr * 0.5f)) / (2.0f * kPi);
    // Two axes, and the model wants the sum f_h + f_v.
    return 2.0f * f_cyc_per_sample * std::max(ppd_render, 1.0f);
}

// ---------------------------------------------------------------- tile ----

float tile_visibility(const TileTemporal& t, int k, float fps,
                      const ModelParams& p) {
    if (k <= 1 || fps <= 0.0f) return 0.0f;

    const float f_s = spatial_freq_cpd(t.freq_ratio, t.ppd_render);

    // Contrast of the uncorrected error after k frames.  APPROX 5: the
    // residual the tile would have coded is `residual_mad` luma codes per
    // frame, and leaving it to the warp lets it accumulate.  Linear
    // accumulation (drift_exponent 1) is the constant-velocity case and the
    // honest worst case for a scheduler that must not surprise the user.
    const float d_code = t.residual_mad * std::pow(float(k), p.drift_exponent);

    // Weber contrast in the model's units.  Differentiate the display's
    // gamma curve at the tile's mean rather than taking a difference, so a
    // dark tile's small code delta does not become a huge nit delta through
    // a badly conditioned subtraction.
    const float mean_n = luma_to_nits(t.mean_luma, p);
    const float c01    = std::clamp(t.mean_luma, 1.0f, 255.0f) / 255.0f;
    const float dL     = p.display_peak_nits * p.display_gamma *
                         std::pow(c01, p.display_gamma - 1.0f) *
                         (d_code / 255.0f);
    const float C      = dL / std::max(mean_n, p.l_min_nits);

    // The error signal is a hold-and-reset ramp with period k/fps, whose
    // harmonic amplitudes fall as 1/(pi m).  Pool the first `harmonics` of
    // them with the paper's Minkowski exponent.
    const float f0 = fps / float(k);
    double acc = 0.0;
    const int m_max = std::max(p.harmonics, 1);
    for (int m = 1; m <= m_max; ++m) {
        const float f_t = f0 * float(m);
        const float amp = C / (kPi * float(m));
        const float c_jnd = sensitivity(f_t, f_s, t.ecc_deg, p) * amp;
        acc += std::pow(double(c_jnd), double(p.r));
    }
    return float(std::pow(acc, 1.0 / double(p.r)));
}

} // namespace nxrc::tvm
