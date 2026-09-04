// nxfov - the NX Warp foveation map.
//
// One R8 texel per 64x64 codec tile, generated on the server from the per-eye
// lens parameters and (optionally) a gaze point.  Section 5.1 of docs/PAPER.md
// is the specification; docs/RATECONTROL.md section 6 is the implementation
// write-up.
//
// Data layout note: everything is a flat array indexed by tile id
// (tile_id = ty * tiles_x + tx), one entry per tile, no per-pixel work.  The
// map generator is a pure function of (lens, config, gaze) so the GPU port is
// a single dispatch of tiles_x*tiles_y threads with no cross-thread traffic.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef NXFOV_FOVEATION_HPP
#define NXFOV_FOVEATION_HPP

#include <cstdint>
#include <cstddef>
#include <vector>

namespace nxfov {

// ---------------------------------------------------------------- lens ----

// Per-eye projection, OpenXR XrFovf convention: tangents of the four half
// angles, left/down negative, right/up positive.  The render target is a
// rectilinear (tan-space) image of width_px x height_px luma samples.
struct LensParams {
    float tan_left   = -0.8568f;
    float tan_right  =  0.8568f;
    float tan_down   = -0.8568f;
    float tan_up     =  0.8568f;
    int   width_px   = 2160;
    int   height_px  = 2160;
    int   tile_size  = 64;
};

// Pico 4 default, chosen so that ppd_center comes out at 22.0 and the corner
// eccentricity at 50 degrees, reproducing the table in PAPER.md 5.1.2.
LensParams pico4_eye();

// Wider render FOV variant (+/- 50 deg, ppd_center 15.8) for the case where
// WiVRn is asked for the full lens FOV at 1.0x render scale.
LensParams pico4_eye_wide();

// ---------------------------------------------------------------- config --

enum class LadderLevel : uint8_t { Full = 0, Half = 1, Quarter = 2 };

struct FoveationConfig {
    // Acuity model: ppd_needed(e) = ppd_fovea / (1 + e / e2_deg).
    float e2_deg          = 2.3f;
    float ppd_fovea       = 60.0f;
    float margin          = 1.5f;   // safety factor on the acuity model

    // Ladder decision thresholds on s_raw = margin*ppd_needed/ppd_render.
    // Reproduces PAPER.md 5.1.2: 1/2 engages at ~14 deg, 1/4 at ~35 deg on
    // the Pico 4 lens.
    float thresh_half     = 0.55f;  // s_raw >= this -> s = 1
    float thresh_quarter  = 0.17f;  // s_raw >= this -> s = 1/2, else 1/4

    // Fixed foveation eye box (half extents in degrees, elliptical).
    float box_x_deg       = 20.0f;
    float box_y_deg       = 15.0f;

    // Eye-tracked padding: pad = pad_per_ms * latency_ms + pad_tracker_deg.
    float pad_per_ms      = 0.05f;
    float pad_tracker_deg = 1.0f;

    // Single user/governor knob: scales the eye box and the pad together.
    // The decode-time governor's knob 3 ("shrink the fovea radius by 10%")
    // sets this to 0.9, 0.81, ...
    float region_scale    = 1.0f;

    // Rate-control eccentricity weight: w = clamp((1+e'/e2)^-exp, floor, 1).
    float weight_exp      = 0.75f;
    float weight_floor    = 0.15f;

    // Eccentricity beyond which a full-resolution tile is still considered
    // "mid ring" rather than fovea (feeds dQ_ecc, PAPER.md 5.2).
    float mid_ring_deg    = 8.0f;
};

// Gaze point in the same tan space as LensParams.  If `valid` is false the
// map generator falls back to fixed foveation on the lens axis.
struct Gaze {
    float tan_x       = 0.0f;
    float tan_y       = 0.0f;
    float latency_ms  = 40.0f;   // measured gaze-to-photon, PAPER.md 5.1.4
    bool  valid       = false;
};

// ---------------------------------------------------------------- map -----

// Struct of arrays; every vector has tiles_x*tiles_y entries.
struct FoveationMap {
    int tiles_x = 0;
    int tiles_y = 0;

    std::vector<uint8_t> r8;        // the wire map: 255 / 128 / 64
    std::vector<uint8_t> level;     // LadderLevel as 0 / 1 / 2
    std::vector<float>   ecc_deg;   // e' (after eye box or gaze pad)
    std::vector<float>   ecc_raw;   // e, angle from gaze/lens axis
    std::vector<float>   theta_deg; // off-axis angle from the lens axis
    std::vector<float>   ratio;     // s_raw before quantisation
    std::vector<float>   weight;    // rate-control eccentricity weight

    size_t size() const { return r8.size(); }
    int    index(int tx, int ty) const { return ty * tiles_x + tx; }
};

FoveationMap foveation_map(const LensParams& lens,
                           const FoveationConfig& cfg,
                           const Gaze* gaze /* nullable */);

// --------------------------------------------------------------- pieces ---

float ppd_needed(float e_deg, const FoveationConfig& cfg);
// Pixels per degree at the centre of the projection (tan-space derivative).
float ppd_center(const LensParams& lens);
// ppd_render(theta) = ppd_center / cos^2(theta).
float ppd_render(const LensParams& lens, float theta_deg);

// s in {1, 0.5, 0.25} for a level.
float level_scale(uint8_t level);
// R8 encoding of a level and its inverse.
uint8_t r8_from_level(uint8_t level);
uint8_t level_from_r8(uint8_t r8);

} // namespace nxfov

#endif // NXFOV_FOVEATION_HPP
