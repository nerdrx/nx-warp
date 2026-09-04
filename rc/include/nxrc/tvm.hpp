// nxrc::tvm - the peripheral temporal-change visibility model.
//
// Tursun and Didyk, "Perceptual Visibility Model for Temporal Contrast
// Changes in Periphery", ACM TOG 41(6), 2022, doi 10.1145/3564241,
// arXiv 2205.00108.  docs/RESEARCH-ACADEMIC.md entry 4.
//
// This is the cost function of the per-tile refresh scheduler
// (nxrc/refresh.hpp).  It answers the question entry 3 (Floeter et al.,
// arXiv 2505.03682) leaves open: not "can peripheral frame rate drop" but
// "will *this* tile's *this* update be seen if it is left to the warp".
//
// The paper's model consumes a local spatio-temporal DCT of the video.  We
// do not have one and will never have one on the encoder's critical path, so
// what follows is a *reduction* of the model onto the five per-tile
// statistics the analysis dispatch already produces, plus eccentricity from
// the foveation map.  Every approximation is marked APPROX in the comments
// and listed in docs/RATECONTROL.md 8.2.  The fitted constants are the
// paper's, verbatim.
//
// Data layout discipline: every function here is a pure scalar function of
// one tile's numbers.  No state, no allocation, no cross-tile reads, so the
// GPU port is one ALU block inside the existing per-tile thread.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef NXRC_TVM_HPP
#define NXRC_TVM_HPP

#include <cstdint>

namespace nxrc::tvm {

// Fitted parameters of the paper's Table 2, verbatim, plus the constants of
// our reduction.  Everything is a field so the tests and the simulator can
// perturb it and so a later re-fit is a data change.
struct ModelParams {
    // ---- the paper's numbers ------------------------------------------
    // De Lange polynomial, S_DL(x) = sum_i a_i x^i.
    float a[4]   = { 3.2714f, 0.3830f, 0.7669f, -0.2555f };
    // Scaling function T(f_s, e) = b1 - b2 * f_s^b3 + b4 * e^q(f_s).
    float b1     = 1.0051f;
    float b2     = 0.1830f;
    float b3     = 0.9517f;
    float b4     = 0.0173f;
    // q(f_s) = b51 f_s^2 + b52 f_s + b53.
    float b51    = -0.1375f;
    float b52    =  0.3753f;
    float b53    =  2.3855f;
    // Shift function U(f_t, f_s, e) = f_t - b6 + b7 f_s + b8 e.  All three
    // are zero in the published calibration, i.e. the peak of the temporal
    // curve does not move with eccentricity or spatial frequency.
    float b6     = 0.0f;
    float b7     = 0.0f;
    float b8     = 0.0f;
    // Minkowski pooling exponent over the spatio-temporal bands.
    float r      = 1.9932f;
    // Weibull psychometric function.
    float p_g    = 0.5f;     // guess rate, 2AFC
    float p_l    = 0.0f;     // lapse rate
    float beta0  = 1.7934f;
    float beta1  = 1.5f;
    // Weber-contrast denominator floor (de Vries-Rose), cd/m^2.
    float l_min_nits = 50.0f;

    // ---- our reduction, all APPROX -------------------------------------
    // The polynomial argument.  See tvm.cpp: in raw Hz the published cubic
    // is monotonically divergent and crosses zero at 4.5 Hz, which cannot be
    // the De Lange curve the paper says it fits.  We take the argument to be
    // log-frequency; ln(1 + f_t) reproduces a band-pass curve peaking near
    // 10 Hz and reaching zero near 70 Hz, which is the De Lange shape.
    // ft_log_base = 0 means ln, otherwise log to that base.
    float ft_log_base = 0.0f;

    // Domain clamps.  T() goes negative above f_s ~ 5.5 cpd and the
    // eccentricity exponent q changes sign at f_s = 5.75 cpd; both are
    // extrapolation past the fit.  We clamp inside the region where T > 0
    // and q > 0, so the model is monotone increasing in eccentricity
    // everywhere it is evaluated (tests/rc/test_tvm.cpp).
    float fs_max_cpd   = 4.0f;    // on the SUM f_h + f_v
    float ecc_min_deg  = 0.5f;    // e^q is singular at e = 0 for q < 0
    float ecc_max_deg  = 60.0f;   // the fit covers 10..40 deg
    float sens_floor   = 0.02f;   // floor on T, guards the clamp boundary

    // Display model, for luma code -> cd/m^2.  A Pico 4 panel is about
    // 100 nits peak; note that this puts almost every tile below the model's
    // 50 cd/m^2 Weber floor, which is a real property of the model on this
    // hardware, not a bug (RATECONTROL.md 8.2).
    float display_peak_nits = 100.0f;
    float display_gamma     = 2.2f;

    // The uncorrected error of a tile left to the warp is a hold-and-reset
    // ramp, whose harmonics fall as 1/m.  We pool this many.
    int   harmonics = 3;

    // Residual growth over k skipped frames.  1.0 = the per-frame residual
    // accumulates linearly (constant-velocity worst case).
    float drift_exponent = 1.0f;
};

// ---------------------------------------------------------------- pieces --

// S_DL after the soft-plus, ln(1 + exp(S_DL)).  `f_t` in Hz.
float sensitivity_temporal(float f_t_hz, const ModelParams& p);
// T(f_s, e): the spatial-frequency and eccentricity scaling.  `f_s` is the
// paper's (f_h + f_v) in cycles/degree, `e` in degrees.
float sensitivity_scale(float f_s_cpd, float e_deg, const ModelParams& p);
// S = T * S_SP(U).  Sensitivity in 1/Weber-contrast: threshold contrast is
// 1/S, and C_JND = S * C.
float sensitivity(float f_t_hz, float f_s_cpd, float e_deg,
                  const ModelParams& p);
// Weibull mapping from the pooled JND-normalised contrast to a detection
// probability in [p_g, 1].
float detect_prob(float c_m, const ModelParams& p);
// The same, rescaled to "excess over chance" in [0, 1]: 2 * (P - 0.5) for a
// 2AFC guess rate.  This is what the simulator reports as "visibility".
float visibility(float c_m, const ModelParams& p);

// Luma code (0..255, gamma encoded) to cd/m^2 under the display model.
float luma_to_nits(float code, const ModelParams& p);

// Representative radial spatial frequency of a tile, in cycles/degree, from
// the classifier's normalised frequency ratio R = grad_energy / var and the
// rendered pixels per degree.  APPROX: R is the second moment of the tile's
// power spectrum under the central-difference operator,
//     R = E[sin^2(2 pi f_x)] + E[sin^2(2 pi f_y)]   (cycles/sample),
// exactly 1.0 for white noise, which is where the classifier's guard comes
// from (RATECONTROL.md 3.2).  Inverting it under an isotropic
// single-frequency assumption gives one representative frequency per axis.
// Returns the SUM f_h + f_v the model wants.
float spatial_freq_cpd(float freq_ratio_R, float ppd_render);

// ---------------------------------------------------------------- tile ----

// Everything the reduction needs about one tile.  All of it already exists
// per tile in the rate controller.
struct TileTemporal {
    float ecc_deg      = 0.0f;   // e', from FoveationMap::ecc_deg
    float mean_luma    = 128.0f; // TileStats::mean_luma
    float freq_ratio   = 0.5f;   // R, from tile_frequency_ratio()
    float residual_mad = 0.0f;   // warped SAD, mean abs difference per pixel
    float ppd_render   = 22.0f;  // nxfov::ppd_render at this tile
};

// Predicted visibility, in the paper's JND units (C_M), of leaving this tile
// to the warp and coding its residual only every `k`-th frame at `fps`.
// k <= 1 is "coded every frame" and returns exactly 0.
//
// Monotone non-decreasing in k, in residual_mad, and in ecc_deg.  The three
// monotonicities are the scheduler's correctness argument and are asserted
// in tests/rc/test_tvm.cpp.
float tile_visibility(const TileTemporal& t, int k, float fps,
                      const ModelParams& p);

} // namespace nxrc::tvm

#endif // NXRC_TVM_HPP
