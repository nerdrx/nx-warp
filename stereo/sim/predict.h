// nxvc-stereosim: the integer predictors and the bit model.
//
// These reimplement, on the CPU and in the same integer arithmetic, the parts
// of the codec the STEREO experiment needs:
//   * the 1/16-pel separable 4-tap sampler (PAPER 2.2 step 4),
//   * the quantised pose homography with corner evaluation and interior
//     bilinear interpolation (PAPER 2.2 steps 1 to 3),
//   * DC-plane intra (PAPER 6.4),
//   * an 8x8 transform plus an Exp-Golomb bit model used as a rate proxy.
//
// If docs/WARP.md lands with a normative filter table or a different fixed-
// point layout, this file is the thing that must follow it; the numbers in
// RESULTS.md are not sensitive to the tap values at the 1 percent level, but
// bit-exactness with the shipped decoder is a separate obligation.
#pragma once

#include <vector>

#include "nxs_common.h"
#include "scene.h"

namespace nxs {

// ------------------------------------------------------------------ sampler

// 16 phases x 4 taps, Catmull-Rom scaled by 64, each row summing to exactly 64.
const i32 (*filter_table())[4];

// Sample at (x_q4, y_q4) in 1/16-pel units, separable 4-tap, clamp-to-edge.
i32 sample_q4(const Image& img, i32 x_q4, i32 y_q4);

// ------------------------------------------------------------------ warp

// Nine int32 in a common Q10.21 scale, coordinates measured from the image
// centre.  See docs/STEREO.md "Fixed point" for why this is not the Q8.24 of
// PAPER 2.2.
struct WarpQ {
    i32 h[9] = {0, 0, 0, 0, 0, 0, 0, 0, 1 << 21};
    double cx = 0, cy = 0;
};

constexpr int kWarpShift = 21;

// H = K * R_prev^T * R_cur * K^-1, quantised.
WarpQ quantize_warp(const Mat3& r_prev, const Mat3& r_cur, double f, double cx, double cy);

// Source position in Q.6 for one integer target pixel, exact evaluation.
void warp_point_q6(const WarpQ& w, i32 x, i32 y, i32* sx_q6, i32* sy_q6);

// Per-tile prediction from a reference image: corner evaluation plus interior
// bilinear, then the MV (quarter-pel) is added and the sample taken at 1/16
// pel.  out must hold kTile*kTile entries.
void warp_tile(const Image& ref, const WarpQ& w, int tx, int ty, i32 mv_x_q2, i32 mv_y_q2,
               std::vector<i32>* out);

// Pure translation prediction (used by STEREO and by STATIC_MV): sample the
// reference at (x + dx, y + dy), dx/dy in quarter-pel.
void shift_tile(const Image& ref, int tx, int ty, i32 dx_q2, i32 dy_q2, std::vector<i32>* out);

// ------------------------------------------------------------------ intra

// Least-squares DC-plane over the tile, evaluated back into out.
void intra_plane(const Image& img, int tx, int ty, std::vector<i32>* out);

// ------------------------------------------------------------------ rate

// Sum of absolute differences of a tile against a prediction.
i64 tile_sad(const Image& img, int tx, int ty, const std::vector<i32>& pred);
i64 tile_sse(const Image& img, int tx, int ty, const std::vector<i32>& pred);

// Modelled bits for the residual of one tile at quantiser step q.
// 8x8 DCT-II per block, level = round(coeff/q), signed Exp-Golomb cost for
// non-zero levels, 0.08 bits per zero level, 2 bits per block of flags.
double tile_bits(const Image& img, int tx, int ty, const std::vector<i32>& pred, double q);

// Side-information cost per mode, in bits.  See docs/STEREO.md.
double mode_side_bits(const char* mode);

// ------------------------------------------------------------------ recon

// Code `cur` against the pose-warped `prev` at quantiser step q and return what
// a decoder would hold: prediction plus the dequantised residual, per tile,
// with a DC-plane intra fallback where that is cheaper in SAD.
//
// This exists because the STEREO reference is the *decoded* left eye of the
// current frame, not the rendered one.  The decoded left eye has itself been
// through a fractional resample and a quantiser, so measuring STEREO against
// the pristine render overstates it.  RESULTS.md reports both.
Image reconstruct_frame(const Image& cur, const Image& prev, const WarpQ& w, double q, int range);

}  // namespace nxs
