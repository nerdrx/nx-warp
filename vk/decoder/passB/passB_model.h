// CPU model of the Pass B reconstruction kernel.
//
// This is a line-for-line model of vk/decoder/passB/reconstruct.comp: the same
// formulas, the same constants (both include syntax_constants.h), the same
// rounding and the same clamps, executed sequentially instead of by 256
// threads.  It is the oracle the GPU kernel is tested against, and it is what
// gets diffed against ref/ when the two are re-aligned.
#pragma once

#include <cstdint>
#include <vector>

#include "passB_layout.h"

namespace nxvw {

struct PassBInput {
    NxvwPassBPush push{};
    int tilesX = 0;
    int tilesY = 0;
    const int16_t *coef = nullptr;      // tilesX*tilesY * push.coefStrideI16
    const NxvwTileRec *recs = nullptr;  // tilesX*tilesY
    // 512: four 128-entry sets of 64 luma then 64 chroma weights, Q4.  Set 0
    // is the frame's pair; sets 1..3 are the built-in pairs a tile's wm_id
    // selects.  A caller whose tiles all have wm_id 0 may pass 128 entries.
    const int *weights = nullptr;
    // [v3] Pass A's packed per-block intra modes, NXVW_MODE_WORDS_PER_TILE
    // uints per tile.  May be null when push.intraDir is 0.
    const uint32_t *modes = nullptr;
    // [sparse] Pass A's per-unit coefficient counts, LAST + 1 with 0 for an
    // uncoded unit: NXVW_UNIT_LEN_WORDS_PER_TILE uints per tile, four units
    // per uint.  Required when push.sparse is set, ignored otherwise.
    const uint32_t *unit_lens = nullptr;
    // [v3] the wavefront schedule the stream was encoded under, kDirSched* in
    // syntax_constants.h.  Matches specialization constant 2 of the kernel.
    int dirSched = 0;
};

// RGBA8: 4 bytes per pixel, R,G,B,A, tightly packed, imageW*imageH pixels.
void passB_reconstruct_rgba8(const PassBInput &in, uint8_t *out);

// RGB10A2: one uint32 per pixel in VK_FORMAT_A2B10G10R10_UINT_PACK32 order,
// R in bits 0..9, G in 10..19, B in 20..29, A in 30..31.
void passB_reconstruct_rgb10a2(const PassBInput &in, uint32_t *out);

// Two-plane 4:2:0 YCbCr passthrough (kOutYcbcr420): `luma` is imageW*imageH
// bytes, `cbcr` is ceil(imageW/2)*ceil(imageH/2) interleaved Cb,Cr pairs.
// Requires push.chroma420 == 1 and push.colorTransform == kCtNone.
void passB_reconstruct_ycbcr420(const PassBInput &in, uint8_t *luma,
                                uint8_t *cbcr);

// --- primitives, exposed so the conformance test can compare them one by one
// with the reference implementation in ref/src/transform.cpp.
void model_idct8x8(const int src[64], int dst[64]);
int model_dequant_step(int qp, int w);
int model_dequant(int q, int t);
int model_bilinear_q4(const int *src, int w, int h, int stride, int sx, int sy);

// Default weighting matrices for a frame header `quant_matrix` selection.
// Fills 128 ints: 64 luma then 64 chroma.  [REF] resolve_matrices().
void model_resolve_matrices(int quant_matrix, const uint8_t *custom128,
                            int *out128);

}  // namespace nxvw
