// nxe_common.glsl -- shared declarations for the NX Warp encoder analysis
// kernels E0/E1/E2.  Mirror of vk/encoder/stats/tile_stats.h; the two are kept
// in step by the offset assertions in nxvc-stats-test.cpp.
//
// SPDX-License-Identifier: Apache-2.0
//
// Rules obeyed throughout (paper 3.7 bit-exactness, 3.2.6 subgroup
// portability):
//   * int32 arithmetic only.  No float, no fp16, no int64.
//   * No OpSDiv/OpSRem: every division is by a power of two and written as a
//     shift, every modulo as a mask.
//   * Rounding shifts are (x + (1 << (s-1))) >> s with an arithmetic shift.
//   * Shift amounts are compile-time constants.
//   * Every buffer load is bounds-clamped; robustBufferAccess is not relied on.
//   * Workgroups are 256 lanes and are never assumed to be one subgroup.
//     Cross-subgroup combination goes through shared memory with a barrier.

#ifndef NXE_COMMON_GLSL
#define NXE_COMMON_GLSL

#extension GL_KHR_shader_subgroup_basic      : require
#extension GL_KHR_shader_subgroup_arithmetic : require

#define NXE_TILE_SIZE      64
#define NXE_TILE_PIXELS    4096
#define NXE_WG_SIZE        256
#define NXE_MAX_SUBGROUPS  64

#define NXE_LUMA_WORDS_PER_TILE 2048
#define NXE_CHROMA_WORDS_444    2048
#define NXE_CHROMA_WORDS_420    512

#define NXE_E2_PER_THREAD  4
#define NXE_E2_BLOCK       1024

#define NXE_TS_F_10BIT      0x00000001u
#define NXE_TS_F_CHROMA_420 0x00000002u
#define NXE_TS_F_PADDED     0x00000004u
#define NXE_TS_F_SAD_VALID  0x00000008u
#define NXE_TS_F_YCBCR      0x00000010u
#define NXE_TS_F_CHROMA_RAW 0x00000020u

#define NXE_SRC_RGBA8         0
#define NXE_SRC_RGB10A2       1
#define NXE_SRC_YCBCR_2PLANE  2

// ---------------------------------------------------------------- the record
// std430 layout; 13 tightly packed 4-byte scalars == the C struct.
struct nxe_tile_stats {
    uint sum_luma;
    uint sum_sq_luma;
    uint mean_luma_q8;
    uint sum_dev_sq;
    uint j_xx;
    uint j_xy_pos;
    uint j_xy_neg;
    uint j_yy;
    uint sad;
    int  mv_qx;
    int  mv_qy;
    uint mv_mag_q4;
    uint flags;
};

// ------------------------------------------------------------- packed planes
// Two 16-bit two's-complement samples per 32-bit word, low half first.
// A thread always owns whole words, so nxe_pack2 is never a partial write.

int nxe_unpack_lo(uint w) { return bitfieldExtract(int(w), 0, 16); }
int nxe_unpack_hi(uint w) { return bitfieldExtract(int(w), 16, 16); }

// hi == 0 -> bits 0..15, hi == 1 -> bits 16..31
int nxe_unpack(uint w, int hi) { return bitfieldExtract(int(w), hi * 16, 16); }

uint nxe_pack2(int lo, int hi) {
    return (uint(lo) & 0xffffu) | (uint(hi) << 16);
}

// First word of tile `t` of a plane based at `base`, given words-per-tile.
uint nxe_tile_word_base(uint base, uint t, uint words_per_tile) {
    return base + t * words_per_tile;
}

// ------------------------------------------------------------- YCoCg-R (1.3)
// Co = R - B;  t = B + (Co >> 1);  Cg = G - t;  Y = t + (Cg >> 1)
// Exactly invertible in integers.  `>>` on a signed int is an arithmetic
// shift in SPIR-V, which is what makes the inverse exact.
void nxe_rgb_to_ycocgr(int r, int g, int b, out int y, out int co, out int cg) {
    co = r - b;
    int t = b + (co >> 1);
    cg = g - t;
    y  = t + (cg >> 1);
}

// -------------------------------------------------- integer square root (E1)
// Floor of sqrt(v) by restoring binary search, 16 iterations, no division and
// no float.  Deterministic on every target; mirrored bit-for-bit by
// nxe_isqrt() in stats_cpu.h.
uint nxe_isqrt(uint v) {
    uint rem = 0u;
    uint root = 0u;
    for (int i = 0; i < 16; ++i) {
        root <<= 1;
        rem = (rem << 2) | (v >> 30);
        v <<= 2;
        if (root < rem) {
            rem -= root | 1u;
            root += 2u;
        }
    }
    return root >> 1;
}

// --------------------------------------------- cross-subgroup reduction (3.2.6)
//
// Every caller declares
//     shared uint s_nxe_red[NXE_MAX_SUBGROUPS][NXE_RED_LANES];
// and calls these from uniform control flow.  The pattern is subgroup-size
// agnostic: it uses subgroupAdd for the intra-subgroup part (any width, any
// vendor) and shared memory for the cross-subgroup part, and it reads
// gl_NumSubgroups rather than assuming it.
//
// Because the accumulators are integers, the reduction order does not change
// the result -- that is the entire bit-exactness argument for E1.

#endif // NXE_COMMON_GLSL
