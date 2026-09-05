// NX Warp decoder, inter path: the one description of every buffer the
// Phase 2 predictor uses.
//
// Shared verbatim by warp_pred.comp (Pass W), inter_hook.glsl (the Pass B
// prediction source and the reference-ring store), inter_model.cpp (the
// line-for-line CPU model) and the host in nxvc_vkdec.cpp, so there is
// exactly one layout and it cannot drift between the four.
//
// NORMATIVE SOURCE: docs/SYNTAX.md 13 and 3.1.1, spec/annex-d-inter-*.md,
// docs/WARP.md, and the executable specification in ref/src/inter.{h,cpp},
// ref/src/codec_impl.inc (predict_tile, store_ref_tile, reconstruct_skip) and
// warp/ref/warp_ref.cpp.  Where this file and ref/ disagree, ref/ wins.
//
// ---------------------------------------------------------------- buffers
//
//   Pass W (warp_pred.comp)
//     binding 0  RefRing    readonly   uint[]: the four reference slots, u16
//                                      samples packed two per uint
//     binding 1  WarpParams readonly   uint[]: the header below, then one
//                                      NxvwWarpTile per tile
//     binding 2  WPred      writeonly  uint[]: the predictor, i16 packed two
//                                      per uint, `wpredStrideI16` per tile
//
//   Pass B (reconstruct.comp, through inter_hook.glsl)
//     binding 13 WPred      readonly   the same buffer Pass W wrote
//     binding 14 RefRing    writeonly  the same ring, the slot this frame owns
//     binding 15 WarpParams readonly   the same header, for the ring geometry
//
// Pass B's own bindings 0-12 are untouched, which is what keeps the inter
// path out of passB_layout.h.
//
// ------------------------------------------------------------- the ring
// [SYN] 13.2.  Four slots addressed by `frame_number mod 4`.  A slot holds the
// whole reconstructed picture of every eye in the CODED sample domain --
// Y/Co/Cg before the inverse colour transform, never RGB -- because that is
// the domain the predictor predicts in.  Samples are u16 so a 9-bit YCoCg-R
// chroma plane fits; they are packed two per uint, and the row stride is
// rounded up to an even number of samples so that a tile's x origin, which is
// always even, lands on a uint boundary.  See "why the stride is padded"
// below.
//
// Plane p of slot s starts at u16 element
//     s * ringSlotU16 + ringPlaneOff[p]
// and is `ringStride[p]` u16 wide by `ph(p)` tall, holding `eyes` eye
// sub-pictures side by side: eye e's sub-picture starts at column e * pw(p).
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXVW_INTER_LAYOUT_H
#define NXVW_INTER_LAYOUT_H

#ifdef __cplusplus
#include <cstdint>
namespace nxvw {
using uint = uint32_t;
#define NXVW_IFN inline int
#define NXVW_IFNU inline uint
#else
#define NXVW_IFN int
#define NXVW_IFNU uint
#endif

// ------------------------------------------------ normative warp constants
// Mirror of warp/include/nxvc/warp.h.  These MUST equal the values there; the
// conformance harness asserts it in C++ so a divergence is a build failure
// rather than a wrong pixel.
#ifdef __cplusplus
#define NXVW_WCONST static constexpr int
#else
#define NXVW_WCONST const int
#endif
NXVW_WCONST kWarpQNum = 21;        // rows 0 and 1 are Q10.21
NXVW_WCONST kWarpQDen = 29;        // row 2 is Q2.29
NXVW_WCONST kWarpQCorner = 6;      // corner coordinates are Q.6
NXVW_WCONST kWarpQMv = 2;          // motion vectors are quarter samples
NXVW_WCONST kWarpQSample = 4;      // the sampling grid is Q.4
NXVW_WCONST kWarpDivShift = 14;    // kWarpQCorner + kWarpQDen - kWarpQNum
NXVW_WCONST kWarpTile = 64;        // warp_tile()'s fixed block edge
NXVW_WCONST kWarpCornerClamp = 1 << 19;
NXVW_WCONST kWarpCoordClamp = 1 << 22;
NXVW_WCONST kWarpDenMin = 1 << 28;
NXVW_WCONST kWarpDenMax = 1 << 30;
NXVW_WCONST kWarpH22 = 1 << 29;
NXVW_WCONST kWarpEntryMax = 1 << 30;
// nw::Mode.  kWarpModeStatic is the identity predictor: no homography, no
// divide.  STATIC_MV and STEREO take it ([SYN] 13.3 step 3).
NXVW_WCONST kWarpModeWarp = 0;
NXVW_WCONST kWarpModeStatic = 1;

// [SYN] 13.9: nine signed bytes per near-skip record, three planes of three.
NXVW_WCONST kNearSkipPlanes = 3;
NXVW_WCONST kNearSkipBytes = 9;

// ------------------------------------------------------- the param buffer
// One uint array: a fixed header, then one NxvwWarpTile per tile.
//
// The header is read by Pass W and by Pass B's ring store, which is why the
// ring geometry lives here rather than in NxvwPassBPush: Pass B's push
// constant block is passB_layout.h's and the inter path does not get to grow
// it.
//
// 4 matrices: [eye][sub-1], sub 1 and 2.  A plane subsampled by `sub` uses the
// conjugated matrix S H S^-1 ([SYN] 13.3 step 1); the conjugation is done on
// the host, once per frame, because it is four integers and a rounding rule
// and neither is worth a shader branch.
#define NXVW_WARP_MAT_UINTS 12
#define NXVW_WARP_NMAT 4
#define NXVW_WARP_HDR_UINTS 64        // 4 * 12 matrices + 16 of ring geometry
#define NXVW_WARP_TILE_UINTS 12

// Header layout, uint indices.  NXVW_WARP_MAT_UINTS * NXVW_WARP_NMAT == 48
// matrix words come first; the ring geometry follows at 48.
#define NXVW_WARP_HDR_RING 48
//  [48] ringSlotU16   u16 elements per ring slot
//  [49] eyes
//  [50] colsPerEye    cols_per_eye, so a tile index gives its eye
//  [51] curSlot       the ring slot this frame writes (STEREO's source)
//  [52..55] ringPlaneOff[4]   u16 element offset of the plane inside a slot
//  [56..59] ringStride[4]     u16 row stride of the plane (padded even)
//  [60..63] planeW[4]         per-eye sample width of the plane
// Per-plane heights are not in the header: a plane's height is the picture's
// (luma/alpha) or the chroma height, and both are in the Pass W push block.
// The ring store in Pass B needs the width to clip a partial edge tile and
// the stride to address a row; it derives nothing else.

// One matrix record, NXVW_WARP_MAT_UINTS uints:
//   h[0..8]  the conjugated homography, Q10.21 rows 0/1, Q2.29 row 2
//   ox, oy   the plane's origin, (plane_width >> 1, plane_height >> 1)
//   pad
struct NxvwWarpMat {
    int h[9];
    int ox, oy;
    int pad;
};

// One per tile, NXVW_WARP_TILE_UINTS uints (48 B).
struct NxvwWarpTile {
    // bits 0-2  mode ([SYN] 4.1: 0 WARP_SKIP, 1 STATIC_MV, 2 WARP_MV,
    //                  3 INTRA, 4 STEREO)
    // bit  3    inter: 1 = Pass W predicts this tile at all.  An INTRA tile
    //           and every tile of a frame with no reference clear it, and the
    //           kernel then writes nothing -- Pass B's hook reads the
    //           predictor only for a tile whose record says it is inter, so
    //           the WPred slot of an intra tile is never read and never has
    //           to be zeroed.
    // bit  4    eye
    // bits 5-6  res_level
    // bit  7    chroma444 (the TILE's, already resolved against the stream)
    // bits 8-9  alpha_mode
    // bit  10   quad_mv
    // bit  11   near_skip (this tile is named by its row's dc_bitmap)
    uint w0;
    int tx;        // tile column INSIDE THE EYE
    int ty;        // tile row
    int mvx;       // Q.2 luma samples; the STEREO disparity goes in mvx
    int mvy;
    uint quad;     // the four raw quadrant bytes, TL TR BL BR in bytes 0..3
    uint refBase;  // u16 element offset of the reference SLOT this tile reads
    int qp;        // clamp(base_qp + qp_delta, 0, 63); near-skip dequantises
                   // its correction at the DC-plane step of this
    uint ns0, ns1, ns2;  // the near-skip record, three signed bytes per plane
                         // in bytes 0..2, planes Y, Co, Cg
    uint pad0;
};

NXVW_IFN nxvw_wt_mode(uint w0) { return int(w0 & 7u); }
NXVW_IFN nxvw_wt_inter(uint w0) { return int((w0 >> 3) & 1u); }
NXVW_IFN nxvw_wt_eye(uint w0) { return int((w0 >> 4) & 1u); }
NXVW_IFN nxvw_wt_res_level(uint w0) { return int((w0 >> 5) & 3u); }
NXVW_IFN nxvw_wt_chroma444(uint w0) { return int((w0 >> 7) & 1u); }
NXVW_IFN nxvw_wt_alpha_mode(uint w0) { return int((w0 >> 8) & 3u); }
NXVW_IFN nxvw_wt_quad(uint w0) { return int((w0 >> 10) & 1u); }
NXVW_IFN nxvw_wt_near_skip(uint w0) { return int((w0 >> 11) & 1u); }

// A signed nibble, two's complement, -8..+7.  [REF] codec.cpp sign_nibble().
NXVW_IFN nxvw_sign_nibble(uint v) {
    int n = int(v & 15u);
    return n >= 8 ? n - 16 : n;
}

// ------------------------------------------------------------- geometry
// [REF] plane_full_extent(): the tile's extent in plane p at STREAM
// resolution.  A chroma plane is 32 only when the STREAM is 4:2:0; inside a
// 4:4:4 stream even a 4:2:0 tile is upsampled straight to 64.
NXVW_IFN nxvw_inter_plane_full(int p, int chroma420) {
    return ((p == 1 || p == 2) && chroma420 != 0) ? 32 : 64;
}
// [REF] Geometry::psub(): the factor the matrix and the vector are
// conjugated by.
NXVW_IFN nxvw_inter_plane_sub(int p, int chroma420) {
    return ((p == 1 || p == 2) && chroma420 != 0) ? 2 : 1;
}

// ------------------------------------------------------------ the WPred
// One i16 per CODED sample of every coded plane, at fixed res_level-0 plane
// offsets so a tile coded at 32 or 16 uses a prefix of its plane's region.
// Values are in [0, maxval] -- warp_tile() clamps, the box average of clamped
// values stays clamped, and a near-skip tile's own combination is clamped
// before it is stored -- so an i16 holds every one of them.
NXVW_IFN nxvw_wpred_plane_edge(int p, int chroma420) {
    return ((p == 1 || p == 2) && chroma420 != 0) ? 32 : 64;
}
NXVW_IFN nxvw_wpred_plane_off(int p, int chroma420) {
    const int ce = ((chroma420 != 0) ? 32 : 64);
    if (p == 0) return 0;
    if (p == 1) return 4096;
    if (p == 2) return 4096 + ce * ce;
    return 4096 + 2 * ce * ce;
}
NXVW_IFN nxvw_wpred_stride_i16(int chroma420, int alphaPresent) {
    const int ce = ((chroma420 != 0) ? 32 : 64);
    int n = 4096 + 2 * ce * ce;
    if (alphaPresent != 0) n += 4096;
    return (n + 1) & ~1;   // even, so every tile slot starts on a uint
}

// ------------------------------------------------- Pass W push constants
// All scalars, so std430 and the push-constant block are trivially the same
// on both sides.
struct NxvwWarpPush {
    int eyeW, eyeH;      // per-eye luma dimensions
    int chromaW, chromaH;// per-eye chroma dimensions
    int eyes;
    int colsPerEye;
    int chroma420;       // 1 = the STREAM is 4:2:0
    int alphaPresent;
    int colorTransform;  // kCtNone / kCtYCoCgR: it decides chroma's maxval
    int chromaQpOff;
    int alphaQpOff;
    int wpredStrideI16;
    int ringSlotU16;
    int tileCount;
    int eyeFilter;       // -1 = every tile; 0 or 1 = only that eye's tiles.
                         // A STEREO tile reads the first eye of THIS frame,
                         // so a frame that carries one runs Pass W and Pass B
                         // once per eye with a barrier between; see
                         // vk/decoder/README.md.
    int pad0;
};

#ifdef __cplusplus
// u16 row stride of plane p in a ring slot: the eye pair's width, rounded up
// to an even number of samples.
//
// **Why the stride is padded.** The ring is addressed as packed u16 pairs, so
// a store writes a whole uint -- two horizontally adjacent samples.  A tile's
// x origin inside a plane is `eye * pw + tile_col * full_extent`, and
// `full_extent` is 64 or 32; the origin is therefore even whenever `eye * pw`
// is, which holds for eyes == 1 always and for eyes == 2 because the decoder
// only accepts a stereo stream whose width is a multiple of 64.  Padding the
// stride to even then makes every row start on a uint boundary too, so no
// uint ever straddles two rows and no two workgroups ever write the same one.
inline int nxvw_ring_stride(int plane_w, int eyes) {
    return (plane_w * eyes + 1) & ~1;
}
// u16 elements one ring slot occupies, and the offset of each plane in it.
// nplanes is 3 or 4.
inline void nxvw_ring_layout(int width, int height, int cw, int ch, int eyes,
                             int nplanes, int off[4], int stride[4],
                             int planeW[4], int *slot_u16) {
    int o = 0;
    for (int p = 0; p < 4; ++p) {
        const int pw = (p == 1 || p == 2) ? cw : width;
        const int ph = (p == 1 || p == 2) ? ch : height;
        off[p] = o;
        stride[p] = nxvw_ring_stride(pw, eyes);
        planeW[p] = pw;
        if (p < nplanes) o += stride[p] * ph;
    }
    *slot_u16 = o;
}
}  // namespace nxvw
#endif

#undef NXVW_IFN
#undef NXVW_IFNU
#undef NXVW_WCONST

#endif  // NXVW_INTER_LAYOUT_H
