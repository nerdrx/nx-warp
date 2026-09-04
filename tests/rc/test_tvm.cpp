// rc.tvm - the peripheral temporal visibility model.
//
// The scheduler's correctness argument rests on three monotonicities and one
// shape.  If any of these break, the gate search stops being a search and
// the ordering in RATECONTROL.md 8.3 stops being derivable.
//
// SPDX-License-Identifier: Apache-2.0

#include "nxrc/tvm.hpp"
#include "rc_test_util.h"

#include <cmath>
#include <vector>

using namespace nxrc;

namespace {

tvm::TileTemporal tile(float ecc, float R, float mad, float luma = 128.0f) {
    tvm::TileTemporal t;
    t.ecc_deg = ecc; t.freq_ratio = R; t.residual_mad = mad;
    t.mean_luma = luma; t.ppd_render = 22.0f;
    return t;
}

// ---- monotone increasing in eccentricity ---------------------------------
//
// This is the load-bearing one and the one that is counter-intuitive.  The
// paper's T(f_s, e) = b1 - b2 f_s^b3 + b4 e^q(f_s) is *increasing* in e for
// every f_s in the clamped domain, because q > 0 there: peripheral vision is
// more sensitive to temporal change, not less (Ferry-Porter).  The whole
// point of clamping f_s at 4.0 cpd is to stay inside that region, so this
// test also guards the clamp.
void test_monotone_eccentricity() {
    rct::begin("monotone in eccentricity");
    tvm::ModelParams p;

    for (float fs : { 0.0f, 0.25f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 12.0f }) {
        for (float ft : { 6.0f, 12.0f, 24.0f, 36.0f }) {
            float prev = -1.0f;
            for (float e = 0.5f; e <= 55.0f; e += 0.5f) {
                const float s = tvm::sensitivity(ft, fs, e, p);
                CHECK_MSG(s >= prev - 1e-6f,
                          "S fell with eccentricity at f_s=" + std::to_string(fs) +
                          " f_t=" + std::to_string(ft) + " e=" + std::to_string(e));
                CHECK_MSG(s > 0.0f, "S must be positive");
                prev = s;
            }
        }
    }

    // And it must be *strictly* increasing over a real span, or the model is
    // contributing nothing to the scheduler's ordering.
    CHECK_LT(tvm::sensitivity(12.0f, 1.0f, 5.0f, p),
             tvm::sensitivity(12.0f, 1.0f, 30.0f, p));

    // The same at tile level.
    for (float R : { 0.004f, 0.42f, 0.97f }) {
        float prev = -1.0f;
        for (float e = 0.5f; e <= 55.0f; e += 0.5f) {
            const float v = tvm::tile_visibility(tile(e, R, 4.0f), 3, 72.0f, p);
            CHECK_MSG(v >= prev - 1e-6f, "C_M fell with eccentricity");
            prev = v;
        }
    }
}

// ---- monotone in the refresh divisor -------------------------------------
void test_monotone_divisor() {
    rct::begin("monotone in divisor");
    tvm::ModelParams p;
    for (float e : { 2.0f, 12.0f, 25.0f, 40.0f }) {
        for (float R : { 0.004f, 0.1f, 0.42f, 0.97f }) {
            float prev = -1.0f;
            for (int k = 1; k <= 8; ++k) {
                const float v = tvm::tile_visibility(tile(e, R, 4.0f), k, 72.0f, p);
                CHECK_MSG(v >= prev - 1e-6f,
                          "C_M fell with k at e=" + std::to_string(e) +
                          " R=" + std::to_string(R) + " k=" + std::to_string(k));
                prev = v;
            }
            // k <= 1 is "coded every frame": exactly zero, by definition.
            CHECK_EQ(tvm::tile_visibility(tile(e, R, 4.0f), 1, 72.0f, p), 0.0f);
            CHECK_EQ(tvm::tile_visibility(tile(e, R, 4.0f), 0, 72.0f, p), 0.0f);
        }
    }
}

// ---- monotone in the residual --------------------------------------------
void test_monotone_residual() {
    rct::begin("monotone in residual");
    tvm::ModelParams p;
    float prev = -1.0f;
    for (float mad = 0.0f; mad <= 30.0f; mad += 0.5f) {
        const float v = tvm::tile_visibility(tile(25.0f, 0.42f, mad), 3, 72.0f, p);
        CHECK_MSG(v >= prev - 1e-9f, "C_M fell with residual");
        prev = v;
    }
    CHECK_EQ(tvm::tile_visibility(tile(25.0f, 0.42f, 0.0f), 3, 72.0f, p), 0.0f);
}

// ---- the De Lange shape --------------------------------------------------
//
// The reason RATECONTROL.md 8.2 reads the polynomial in log-frequency: in
// raw Hz it is not band-pass and dies at 4.5 Hz.  If someone "fixes" the
// argument back to linear, this test says so.
void test_de_lange_shape() {
    rct::begin("De Lange shape");
    tvm::ModelParams p;

    // Band-pass: a peak somewhere in 5..20 Hz, above both DC and 60 Hz.
    float peak = 0.0f, peak_at = 0.0f;
    for (float f = 0.5f; f <= 90.0f; f += 0.5f) {
        const float s = tvm::sensitivity_temporal(f, p);
        if (s > peak) { peak = s; peak_at = f; }
    }
    CHECK_MSG(peak_at >= 5.0f && peak_at <= 20.0f,
              "temporal peak at " + std::to_string(peak_at) + " Hz, want 5..20");
    CHECK_LT(tvm::sensitivity_temporal(0.0f, p), peak);
    CHECK_LT(tvm::sensitivity_temporal(60.0f, p), peak * 0.5f);

    // Falling above the peak, which is the whole reason a 1/2 rate step is
    // cheaper than a 1/6 one at equal amplitude.
    CHECK_LT(tvm::sensitivity_temporal(36.0f, p),
             tvm::sensitivity_temporal(12.0f, p));
    CHECK_LT(tvm::sensitivity_temporal(72.0f, p),
             tvm::sensitivity_temporal(36.0f, p));
}

// ---- the flat-versus-texture inversion -----------------------------------
//
// The finding the combined ordering of RATECONTROL.md 8.3 turns on: at equal
// contrast a smooth peripheral tile is far more visible when its update is
// withheld than a finely textured one, which is the reverse of the spatial
// ladder's order.  If this ever flips, section 8.3 is wrong.
void test_flat_more_visible_than_texture() {
    rct::begin("flat beats texture in the periphery");
    tvm::ModelParams p;
    const float flat_e30 = tvm::sensitivity(12.0f, tvm::spatial_freq_cpd(0.004f, 22.0f), 30.0f, p);
    const float tex_e30  = tvm::sensitivity(12.0f, tvm::spatial_freq_cpd(0.97f, 22.0f), 30.0f, p);
    const float flat_e1  = tvm::sensitivity(12.0f, tvm::spatial_freq_cpd(0.004f, 22.0f), 1.0f, p);
    const float tex_e1   = tvm::sensitivity(12.0f, tvm::spatial_freq_cpd(0.97f, 22.0f), 1.0f, p);

    CHECK_LT(tex_e30, flat_e30);
    CHECK_LT(tex_e1, flat_e1);
    // And eccentricity widens the gap, which is why the effect matters at
    // all: if the ratio were constant it would just be a class weight.
    CHECK_LT(flat_e1 / tex_e1, flat_e30 / tex_e30);
}

// ---- the frequency reduction ---------------------------------------------
void test_spatial_freq() {
    rct::begin("spatial frequency reduction");
    // R is monotone in the representative frequency, 0 at DC, and white
    // noise (R = 1) must land at a plausible mid frequency rather than at
    // the Nyquist limit.
    float prev = -1.0f;
    for (float R = 0.0f; R <= 1.5f; R += 0.01f) {
        const float f = tvm::spatial_freq_cpd(R, 22.0f);
        CHECK_MSG(f >= prev - 1e-6f, "f_s fell with R");
        prev = f;
    }
    CHECK_NEAR(tvm::spatial_freq_cpd(0.0f, 22.0f), 0.0f, 1e-6);
    const float wn = tvm::spatial_freq_cpd(1.0f, 22.0f);
    CHECK_MSG(wn > 3.0f && wn < 8.0f,
              "white noise at " + std::to_string(wn) + " cpd sum, want 3..8");
    // Scales with the rendered pixel density.
    CHECK_NEAR(tvm::spatial_freq_cpd(0.5f, 44.0f),
               2.0f * tvm::spatial_freq_cpd(0.5f, 22.0f), 1e-4);
}

// ---- the psychometric function -------------------------------------------
void test_detect_prob() {
    rct::begin("detection probability");
    tvm::ModelParams p;
    CHECK_NEAR(tvm::detect_prob(0.0f, p), 0.5, 1e-6);
    CHECK_NEAR(tvm::visibility(0.0f, p), 0.0, 1e-6);
    float prev = -1.0f;
    for (float c = 0.0f; c <= 20.0f; c += 0.05f) {
        const float q = tvm::detect_prob(c, p);
        CHECK_MSG(q >= prev - 1e-6f, "P fell with C_M");
        CHECK_MSG(q >= 0.5f - 1e-6f && q <= 1.0f + 1e-6f, "P out of range");
        prev = q;
    }
    CHECK(tvm::detect_prob(20.0f, p) > 0.99f);
    CHECK(tvm::visibility(20.0f, p) > 0.99f);
}

// ---- determinism ---------------------------------------------------------
void test_determinism() {
    rct::begin("determinism");
    tvm::ModelParams p;
    for (int rep = 0; rep < 3; ++rep) {
        const float a = tvm::tile_visibility(tile(23.5f, 0.37f, 5.25f, 91.0f),
                                             4, 72.0f, p);
        static float first = 0.0f;
        if (rep == 0) first = a; else CHECK_EQ(a, first);
    }
}

} // namespace

int main() {
    test_monotone_eccentricity();
    test_monotone_divisor();
    test_monotone_residual();
    test_de_lange_shape();
    test_flat_more_visible_than_texture();
    test_spatial_freq();
    test_detect_prob();
    test_determinism();
    return rct::finish("rc.tvm");
}
