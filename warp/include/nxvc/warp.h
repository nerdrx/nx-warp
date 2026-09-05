// NX Warp -- pose-warped predictor, normative integer definition.
//
// See docs/WARP.md for the normative prose. This header is the C API surface;
// warp/ref/warp_ref.cpp is the bit-exact implementation and
// warp/glsl/warp_tile.comp is its GLSL twin. The two MUST agree bit for bit.
//
// Everything on the normative path is int32. No float, no double, no 64-bit
// integer opcode (64-bit intermediates are emulated as (hi,lo) uint32 pairs),
// no division except the fixed-iteration restoring divide in nxvc_warp_div().
//
// SPDX-License-Identifier: Apache-2.0

#ifndef NXVC_WARP_H
#define NXVC_WARP_H

#include <cstddef>
#include <cstdint>

namespace nxvc::warp {

// ---------------------------------------------------------------------------
// Fixed-point formats (see WARP.md section 3)
// ---------------------------------------------------------------------------

// Rows 0 and 1 of the homography (h00 h01 h02 / h10 h11 h12) are Q10.21:
// signed, 21 fractional bits, representable range +-1024.0.
inline constexpr int kQNum = 21;

// Row 2 (h20 h21 h22) is Q2.29: signed, 29 fractional bits, range +-4.0.
// h22 is normalised to exactly 1.0 == 2^29.
inline constexpr int kQDen = 29;

// Corner source coordinates are Q.6 (1/64 pel).
inline constexpr int kQCorner = 6;

// Per-tile motion vectors are Q.2 (1/4 pel).
inline constexpr int kQMv = 2;

// Sampling positions are Q.4 (1/16 pel).
inline constexpr int kQSample = 4;

// Shift applied to the numerator before the restoring divide so the quotient
// comes out in Q.6:  x = (num/2^kQNum) / (den/2^kQDen)  =>  x*2^6 =
// (num << (6 + kQDen - kQNum)) / den.
inline constexpr int kDivShift = kQCorner + kQDen - kQNum;  // == 14

// Tile side in samples. v1 Pass B is one workgroup per 64x64 tile.
inline constexpr int kTile = 64;

// Corner coordinates are saturated to +-2^19 in Q.6 (== +-8192 pel). The
// in-tile interpolation is done in two rounded steps (see bilerp_corner in
// warp_ref.cpp) precisely so this bound can be this generous: a single-step
// bilerp would overflow int32 above +-4096 pel, which a 4096-wide eye plus a
// few hundred pixels of displacement already exceeds.
inline constexpr int32_t kCornerClamp = 1 << 19;

// Safety margin on the quantised entries. derive_homography() rejects any
// matrix with an entry beyond this, so the format is never operated at the
// ragged edge of int32. For the Q10.21 rows this is a translation term of
// +-512 px, which at a 2160 px / 95 deg eye is about 2200 deg/s of head
// rotation -- roughly seven times the 300 deg/s that paper 2.2 calls fast.
inline constexpr int32_t kEntryMax = 1 << 30;

// Sampling coordinates are saturated here after the motion vector is added,
// so that the Q.6 -> Q.4 rounding step cannot overflow int32 for any input.
// Legal values are at most kCornerClamp plus a +-64 px vector (~2^20), so this
// never binds in the operational envelope.
inline constexpr int32_t kCoordClamp = 1 << 22;

// Legal range of the homogeneous denominator. The encoder MUST guarantee this
// for every corner of every tile it emits; the decoder saturates if violated.
inline constexpr int32_t kDenMin = 1 << 28;  // 0.5
inline constexpr int32_t kDenMax = 1 << 30;  // 2.0 (exclusive)

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

enum Filter : uint32_t {
    kFilterBilinear = 0,    // Lite profile: 4 taps, 4 loads
    kFilterCatmullRom = 1,  // Full profile: 4x4 taps, 16 loads
};

enum Mode : uint32_t {
    kModeWarp = 0,    // WARP_SKIP / WARP_MV: homography + MV
    kModeStatic = 1,  // STATIC_MV: identity predictor + MV, H ignored
};

// Quantised per-eye homography, plus the centring origin it was derived for.
// h[0..5] are Q10.21, h[6..8] are Q2.29, h[8] (== h22) is always 1<<kQDen.
// The matrix maps *centred integer sample indices* of the target frame to
// centred source indices of the reference frame:
//     (x - ox, y - oy)  ->  (x_src - ox, y_src - oy)
struct Homography {
    int32_t h[9];
    int32_t ox;  // normally frame_width  / 2
    int32_t oy;  // normally frame_height / 2
};

// Identity homography for the given origin.
Homography identity_homography(int32_t ox, int32_t oy);

// Reference picture. Samples are uint16 regardless of bit depth; `max_value`
// is (1 << bit_depth) - 1. Interleaved `channels` samples per pixel.
struct RefImage {
    const uint16_t* data = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;  // in samples, i.e. >= width * channels
    int32_t channels = 1;
    int32_t max_value = 255;
};

// ---------------------------------------------------------------------------
// Normative integer primitives (exposed for conformance testing)
// ---------------------------------------------------------------------------

// Emulated 64-bit value as an unsigned (hi,lo) pair.
struct U64 {
    uint32_t lo;
    uint32_t hi;
};

U64 nxvc_umul_ext(uint32_t a, uint32_t b);       // a*b, unsigned
U64 nxvc_imul_ext(int32_t a, int32_t b);         // a*b, signed, two's complement
U64 nxvc_add64(U64 a, U64 b);
U64 nxvc_neg64(U64 a);
U64 nxvc_shl64(U64 a, uint32_t n);               // n in 0..31
U64 nxvc_from_i32(int32_t v);                    // sign-extend

// Fixed 32-iteration restoring division. Requires d != 0 and n.hi < d.
// Returns floor(n / d) as a uint32 (the quotient is guaranteed to fit).
uint32_t nxvc_warp_div(U64 n, uint32_t d);

// The four source corner coordinates of a tile, in Q.6, already offset back
// into absolute (uncentred) reference-picture coordinates.
// Order: [0]=(tx,ty) [1]=(tx+64,ty) [2]=(tx,ty+64) [3]=(tx+64,ty+64).
struct TileCorners {
    int32_t x[4];
    int32_t y[4];
};

TileCorners warp_tile_corners(const Homography& H, int32_t tile_x, int32_t tile_y, Mode mode);

// Catmull-Rom taps, [fraction 0..15][tap 0..3], normalised so each row sums
// to exactly 64.
extern const int8_t kCatmullRom[16][4];

// ---------------------------------------------------------------------------
// The predictor
// ---------------------------------------------------------------------------

// Produce the 64x64 prediction for the tile whose top-left sample is
// (tile_x, tile_y) in the target frame.
//
//   ref        reference picture (previous decoded frame, or left eye)
//   H          quantised homography for this eye and frame
//   mv_qpel    per-tile motion vector, Q.2, {x, y}
//   filter     kFilterBilinear or kFilterCatmullRom
//   mode       kModeWarp or kModeStatic
//   out_tile   64*64*channels samples, row stride `out_stride` samples
void warp_tile(const RefImage& ref,
               int32_t tile_x,
               int32_t tile_y,
               const Homography& H,
               const int32_t mv_qpel[2],
               Filter filter,
               Mode mode,
               uint16_t* out_tile,
               int32_t out_stride);

// The same predictor with four vectors, one per quadrant of the output block:
// quadrant `(v >= quad_split) * 2 + (u >= quad_split)` of `mv_qpel`, in the
// order top-left, top-right, bottom-left, bottom-right.
//
// The GEOMETRY is unchanged: every sample still reads the whole tile's four
// corners through the same bilerp_corner(), so four equal vectors reproduce
// warp_tile() bit for bit -- warp_tile() is literally this function with the
// vector replicated. Only the Q.6 vector added after the corner interpolation
// is chosen per quadrant. `quad_split` is the extent at which the block is
// halved; it is kTile/2 for a full-extent plane and half the plane's extent
// for a plane the caller crops out of the 64x64 block (docs/SYNTAX.md 13.8).
void warp_tile_quad(const RefImage& ref,
                    int32_t tile_x,
                    int32_t tile_y,
                    const Homography& H,
                    const int32_t mv_qpel[4][2],
                    int32_t quad_split,
                    Filter filter,
                    Mode mode,
                    uint16_t* out_tile,
                    int32_t out_stride);

// ---------------------------------------------------------------------------
// Homography derivation (encoder side, double precision, NOT normative)
// ---------------------------------------------------------------------------

// OpenXR-style pose: unit quaternion (x,y,z,w), camera-to-world (-Z forward,
// +Y up, right-handed).
struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

// OpenXR XrFovf: angles in radians, left/down negative.
struct Fov {
    double angle_left = -0.8, angle_right = 0.8;
    double angle_up = 0.8, angle_down = -0.8;
};

// H = K(fov_prev, W, H) * R_prev^T * R_cur * K(fov_cur, W, H)^-1, expressed on
// integer sample indices centred at (W/2, H/2), then quantised.
// Returns false (and an identity result) if any entry falls outside its format
// or if the denominator leaves [kDenMin, kDenMax) anywhere in the picture.
bool derive_homography(const Quat& r_prev,
                       const Fov& fov_prev,
                       const Quat& r_cur,
                       const Fov& fov_cur,
                       int32_t frame_width,
                       int32_t frame_height,
                       Homography* out);

// Exact (double) homography in the same centred-index convention, row-major.
// Used by the oracle and by derive_homography() before quantisation.
void exact_homography(const Quat& r_prev,
                      const Fov& fov_prev,
                      const Quat& r_cur,
                      const Fov& fov_cur,
                      int32_t frame_width,
                      int32_t frame_height,
                      double out[9]);

// ---------------------------------------------------------------------------
// Float oracle -- TESTS ONLY. Never on the normative path.
// ---------------------------------------------------------------------------

namespace oracle {

// Continuous source coordinate for target sample (x, y) under the *exact*
// double homography, in pixel units (sample-index convention).
void source_coord(const double Hd[9],
                  int32_t ox,
                  int32_t oy,
                  int32_t x,
                  int32_t y,
                  const int32_t mv_qpel[2],
                  double* sx,
                  double* sy);

// Same, but from the quantised integer matrix, evaluated in double at full
// precision (no corner interpolation, no 1/16-pel snap). This isolates the
// error contributed by the integer pipeline from the error contributed by
// quantising H.
void source_coord_q(const Homography& H,
                    int32_t x,
                    int32_t y,
                    const int32_t mv_qpel[2],
                    double* sx,
                    double* sy);

// Float warp of a tile: exact coordinates, exact filter weights, double
// accumulation, single round at the end. The integer result is required to
// stay within the tolerance documented in WARP.md section 9.
void warp_tile_float(const RefImage& ref,
                     int32_t tile_x,
                     int32_t tile_y,
                     const double Hd[9],
                     int32_t ox,
                     int32_t oy,
                     const int32_t mv_qpel[2],
                     Filter filter,
                     Mode mode,
                     double* out_tile,
                     int32_t out_stride);

}  // namespace oracle

}  // namespace nxvc::warp

#endif  // NXVC_WARP_H
