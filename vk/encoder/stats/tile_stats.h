/* tile_stats.h -- NX Warp encoder analysis: per-tile statistics contract.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This header is the ABI between the GPU analysis kernels (E0/E1/E2, see
 * docs/PAPER.md 3.6) and every consumer of their output: the CPU rate
 * controller (`rc/`, paper 4.6 and 4.6.1), the mode decision in E2_transform,
 * and the packetizer in E5.
 *
 * It is plain C (C99, also valid C++) with no includes beyond <stdint.h> so it
 * can be pulled into the reference codec, the rate controller and the Vulkan
 * encoder alike.  The matching GLSL declarations live in
 * `vk/encoder/stats/nxe_common.glsl`; the two are kept in step by the
 * offset assertions in `vk/encoder/tools/nxvc-stats-test.cpp`.
 *
 * Provenance note: at the time this was written `rc/` and `docs/RATECONTROL.md`
 * did not exist in the tree, so this struct is defined here rather than adopted
 * from the rate-control agent.  It carries exactly the five inputs 4.6 asks
 * for -- mean luma, log-variance, structure tensor sums Jxx/Jxy/Jyy, warped
 * SAD, MV magnitude -- with the variance delivered as an exact integer
 * sum-of-squared-deviations so that the log is taken host-side in floating
 * point where it is harmless (nothing downstream of `rc/` is normative).
 *
 * ---------------------------------------------------------------------------
 * Everything in this file is integer and order-independent.
 * ---------------------------------------------------------------------------
 * Every accumulator below is a plain sum of integers.  Integer addition is
 * associative and (for the u32 fields) associative modulo 2^32, so the value a
 * record holds does not depend on the order the GPU happened to reduce in.
 * That is what makes the GPU kernels bit-exact against the CPU models in
 * `stats_cpu.h` on any subgroup size, which paper 3.2.6 requires.
 */

#ifndef NXE_TILE_STATS_H
#define NXE_TILE_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ geometry
 *
 * Tiles are 64x64 luma, fixed for v1 (paper 1.1).  A frame whose dimensions
 * are not a multiple of 64 is padded to the tile grid by edge replication in
 * E0; padded tiles are flagged NXE_TS_F_PADDED and their statistics include
 * the replicated samples.
 */
#define NXE_TILE_SIZE        64
#define NXE_TILE_PIXELS      (NXE_TILE_SIZE * NXE_TILE_SIZE) /* 4096 */

/* Chroma tile side for the two chroma modes. */
#define NXE_CTILE_SIZE_444   64
#define NXE_CTILE_SIZE_420   32

/* Upper bound the kernels are dimensioned for.  8192 tiles is 2 views of
 * 2048x2048 (1024 tiles each) with 4x headroom, and is the limit E2's
 * two-level scan is specified for. */
#define NXE_MAX_TILES        8192

/* E1 and E2 both run 256-lane workgroups (paper 3.2.3 shape).  E2 scans
 * NXE_E2_BLOCK elements per workgroup. */
#define NXE_WG_SIZE          256
#define NXE_E2_PER_THREAD    4
#define NXE_E2_BLOCK         (NXE_WG_SIZE * NXE_E2_PER_THREAD)  /* 1024 */
#define NXE_E2_MAX_BLOCKS    (NXE_MAX_TILES / NXE_E2_BLOCK)     /* 8 */

/* Never assume a workgroup is one subgroup (paper 3.2.6).  The smallest
 * subgroup the codec supports is 8 (lavapipe), so a 256-lane workgroup is at
 * most 32 subgroups; the shaders size their cross-subgroup scratch for 64. */
#define NXE_MAX_SUBGROUPS    64
#define NXE_MIN_SUBGROUP     8

/* --------------------------------------------------------------- plane layout
 *
 * E0 writes tile-major planes, which is the layout the Pass-B-style kernels
 * want: one 64x64 tile is 4096 contiguous samples, so a workgroup that owns a
 * tile reads one contiguous run and never strides by the frame width.
 *
 * Samples are 16-bit two's complement packed two per 32-bit word, low half
 * first.  16 bits is the storage width paper 3.7 mandates ("int32 arithmetic
 * only, with int16 storage"); the manual pack into u32 rather than an int16_t
 * SSBO avoids depending on VK_KHR_16bit_storage and keeps the shader ALU in
 * int32 throughout.  A thread always owns whole words, so packing is never a
 * read-modify-write race.
 *
 * Sample (x, y) of tile t of a plane whose base word is `base`:
 *
 *     i    = y * side + x              (side = 64 luma, 64 or 32 chroma)
 *     word = base + t * (side*side/2) + (i >> 1)
 *     half = i & 1                     (0 = bits 0..15, 1 = bits 16..31)
 *
 * Planes are laid out Y, then Co, then Cg, each a whole number of words.
 */
#define NXE_LUMA_WORDS_PER_TILE   (NXE_TILE_PIXELS / 2)                 /* 2048 */
#define NXE_CHROMA_WORDS_444      (NXE_TILE_PIXELS / 2)                 /* 2048 */
#define NXE_CHROMA_WORDS_420      ((NXE_CTILE_SIZE_420 * NXE_CTILE_SIZE_420) / 2) /* 512 */

/* Sample value ranges after the YCoCg-R forward transform (paper 1.3).  For
 * n-bit RGB input, Y is n-bit unsigned and Co/Cg are (n+1)-bit signed, which
 * is why chroma needs the sign bit of the 16-bit slot. */
#define NXE_DEPTH_8              8
#define NXE_DEPTH_10             10

/* Chroma modes. */
#define NXE_CHROMA_444           0
#define NXE_CHROMA_420           1

/* Source import formats E0 is compiled for (paper 3.6/3.8: the compositor
 * hands us RGBA8 or RGB10A2 in the same VkDevice). */
#define NXE_SRC_RGBA8            0
#define NXE_SRC_RGB10A2          1
/* The Linux compositor path: VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, or
 * G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 at 10 bits, already foveated and
 * already 4:2:0.  E0 passes it through without a colour transform; see the
 * commentary at the head of E0_convert.comp. */
#define NXE_SRC_YCBCR_2PLANE     2

/* --------------------------------------------- gradient operator for E1
 *
 * The structure tensor uses the SAME operator as the CPU rate controller in
 * `rc/` (nxrc::compute_one_tile_stats): the central difference
 *
 *     gx = (I[x+1] - I[x-1]) / 2,   gy = (I[y+1] - I[y-1]) / 2
 *
 * with the neighbour index clamped inside the tile, so a tile's tensor depends
 * only on that tile.  This is not a free choice.  rc/'s classifier thresholds
 * (ClassifyConfig::flat_gradient, text_gradient, edge_coherence) are calibrated
 * in absolute units against that operator; feeding it a Sobel response, whose
 * magnitude is about 8x larger and whose smoothing changes the coherence of
 * fine detail, would silently mis-classify every tile in the frame.
 *
 * The GPU cannot halve an odd difference without leaving the integers, and
 * rounding it would be worse than useless: the bias is systematic, and while
 * it barely moves Jxx it can be tens of percent of Jxy, which is small by
 * cancellation and is exactly what coherence depends on.  So E1 accumulates
 * the *undivided* difference d = I[x+1] - I[x-1] and the record carries a
 * tensor 4x rc/'s, an exact power-of-two rescale that rc_adapter.h undoes.
 *
 * Overflow, stated exactly, because these are the tightest accumulators in the
 * kernel.  With |d| <= 2^n - 1 over 4096 samples:
 *   8-bit:  Jxx <= 4096 * 255^2  = 2.7e8,  a factor of 16 of headroom.
 *   10-bit: Jxx <= 4096 * 1023^2 = 4286562304 < 2^32.  Exact, but only just,
 *           which is why Jxx and Jyy are unsigned rather than signed.
 * Jxy is bounded by sqrt(Jxx*Jyy) and so has the same 2^32 ceiling in
 * magnitude -- too much for an int32 -- so it is carried as two unsigned sums,
 * the positive and negative contributions separately.  Each is individually
 * bounded by that same sqrt, both are exact, and the adapter subtracts them in
 * double where the difference is exact for every value they can hold.  One
 * extra accumulator lane buys an exact signed cross term at every depth.
 */

/* --------------------------------------------------------------- the record */

/* Bits of nxe_tile_stats::flags. */
#define NXE_TS_F_10BIT           0x00000001u /* source was RGB10A2            */
#define NXE_TS_F_CHROMA_420      0x00000002u /* chroma planes are decimated   */
#define NXE_TS_F_PADDED          0x00000004u /* tile extends past the frame   */
#define NXE_TS_F_SAD_VALID       0x00000008u /* a reference was bound to E1   */
/* Planes hold YCbCr passthrough rather than YCoCg-R (stream colour_space =
 * YCbCr).  Nothing in rate control depends on it -- the statistics are luma
 * statistics either way -- but the flag travels with the record so a consumer
 * that wants to reason about absolute chroma amplitude can. */
#define NXE_TS_F_YCBCR           0x00000010u

/*
 * One record per tile, tile index = ty * tiles_x + tx, views concatenated.
 * 52 bytes, all members 4-byte scalars, so the C layout and the std430 layout
 * of the GLSL mirror are identical with no padding on any ABI in play.
 */
typedef struct nxe_tile_stats {
    /* Mean luma, and the raw moments it comes from.
     * sum_luma  = SUM Y over the 4096 samples of the luma tile.
     * sum_sq_luma = SUM Y*Y.  Exact in u32: the worst case is 4096*1023^2 =
     *   4286562304 < 2^32.  Kept because it is free and lets a consumer form
     *   the variance without trusting our mean rounding.
     * mean_luma_q8 = round(sum_luma / 4096) in Q8.8 fixed point, which is
     *   exactly (sum_luma + 8) >> 4.  This is the dQ_lum input of paper 5.2. */
    uint32_t sum_luma;
    uint32_t sum_sq_luma;
    uint32_t mean_luma_q8;

    /* Activity (paper 5.2 dQ_act, 4.6.1 "log-variance activity term").
     * sum_dev_sq = SUM (Y - mean_int)^2 with mean_int = (sum_luma + 2048) >> 12,
     *   i.e. the rounded integer mean.  Computed by a second pass over the
     *   tile, which is nearly free because the tile is already in LDS.
     *   Bounded by 4096 * 512^2 = 1.07e9, so exact in u32 for both depths.
     *
     * The consumer forms  variance   = sum_dev_sq / 4096
     *                     log-var    = log2(variance)
     * in floating point.  The log is deliberately *not* taken on the GPU:
     * nothing downstream of rc/ is normative, and keeping the GPU side purely
     * integral is what makes this record bit-exact across vendors. */
    uint32_t sum_dev_sq;

    /* Structure tensor summed over the tile.  Gradients at the tile border
     * clamp *within the tile*, exactly as rc/ does, so a tile's tensor depends
     * only on that tile -- no cross-tile reads, which the tile-major layout
     * would make expensive and which would couple neighbouring workgroups.
     *
     * Jxy = j_xy_pos - j_xy_neg, formed by rc_adapter.h.  rc/ derives paper
     * 4.6.1's gradient coherence from these:
     *   tr   = Jxx + Jyy
     *   det  = Jxx*Jyy - Jxy*Jxy
     *   coh  = sqrt(max(0, tr*tr - 4*det)) / max(1, tr)   in [0,1]
     * coh near 1 means one dominant gradient direction (an edge or a glyph
     * stroke), coh near 0 means isotropic detail (texture).  Combined with the
     * activity term above it yields the four classes text/edge/texture/flat. */
    uint32_t j_xx;
    uint32_t j_xy_pos;   /* SUM of gx*gy over samples where the product > 0 */
    uint32_t j_xy_neg;   /* SUM of -(gx*gy) where the product < 0          */
    uint32_t j_yy;

    /* Warped SAD: SUM |src(x,y) - pred(x,y)| over the luma tile, where pred is
     * the reference plane sampled at (x,y) + mv, clamped to the frame.  This
     * is paper 4.6's cplx_t numerator.  Bounded by 4096*1023 = 4190208.
     *
     * In this milestone `mv` comes in as a per-tile integer offset supplied by
     * the host, and the caller passes zero (previous frame at identity) until
     * E0_warp lands; the offset buffer is the hook the warp will write.
     * NXE_TS_F_SAD_VALID is clear when no reference was bound, in which case
     * sad is 0. */
    uint32_t sad;

    /* The offset the SAD was measured at, in quarter-pel units (paper 2.3
     * codes one quarter-pel vector per tile).  Integer-pel offsets therefore
     * arrive here multiplied by 4.
     * mv_mag_q4 = isqrt(mv_qx^2 + mv_qy^2), the floor integer square root, in
     * the same quarter-pel units.  It is computed with a deterministic integer
     * routine (nxe_isqrt) rather than sqrt() so the record stays bit-exact;
     * it feeds paper 5.2's dQ_motion retinal-slip term. */
    int32_t  mv_qx;
    int32_t  mv_qy;
    uint32_t mv_mag_q4;

    uint32_t flags;
} nxe_tile_stats;

/* 13 x 4 bytes.  Asserted in stats_cpu.c and against the shader in the test. */
#define NXE_TILE_STATS_SIZE 52

/* ------------------------------------------------------- frame parameters
 *
 * Pushed to E0 and E1 as push constants (both fit the guaranteed 128-byte
 * minimum).  Offsets are in 32-bit words into the plane buffer.
 */
typedef struct nxe_frame_params {
    uint32_t width;         /* luma width  in pixels (unpadded)             */
    uint32_t height;        /* luma height in pixels (unpadded)             */
    uint32_t tiles_x;       /* ceil(width  / 64)                            */
    uint32_t tiles_y;       /* ceil(height / 64)                            */
    uint32_t plane_y_off;   /* word offset of the Y plane                   */
    uint32_t plane_co_off;  /* word offset of the Co plane                  */
    uint32_t plane_cg_off;  /* word offset of the Cg plane                  */
    uint32_t flags;         /* NXE_TS_F_* bits that are frame-constant      */
} nxe_frame_params;

/* Push-constant blocks.  Each starts with nxe_frame_params so the common
 * prefix is identical across kernels; all are well under the 128-byte
 * guaranteed minimum push-constant size. */
typedef struct nxe_e0_push {
    nxe_frame_params f;
    uint32_t plane_words;   /* size of the plane buffer, for store clamping  */
} nxe_e0_push;              /* 36 bytes */

typedef struct nxe_e1_push {
    nxe_frame_params f;
    uint32_t plane_words;   /* size of one plane buffer, for load clamping   */
    uint32_t num_tiles;     /* tiles_x * tiles_y, for stats store clamping   */
    uint32_t has_ref;       /* 0 = no reference bound, SAD is skipped        */
    uint32_t pad_;
} nxe_e1_push;              /* 48 bytes */

typedef struct nxe_e2_push {
    uint32_t num_elems;     /* number of per-tile sizes to scan, <= 8192     */
    uint32_t num_blocks;    /* ceil(num_elems / NXE_E2_BLOCK)                */
} nxe_e2_push;

/* Number of 32-bit words a full set of planes needs for `num_tiles` tiles. */
static inline uint32_t nxe_plane_total_words(uint32_t num_tiles, int chroma_420)
{
    uint32_t c = (uint32_t)(chroma_420 ? NXE_CHROMA_WORDS_420 : NXE_CHROMA_WORDS_444);
    return num_tiles * ((uint32_t)NXE_LUMA_WORDS_PER_TILE + 2u * c);
}

/* Fill in the derived fields of a frame-parameter block. */
/* `src_fmt` is one of NXE_SRC_*; it only affects the flag bits. */
static inline void nxe_frame_params_init2(nxe_frame_params *p, uint32_t w, uint32_t h,
                                          int chroma_420, int depth10, int src_fmt);

static inline void nxe_frame_params_init(nxe_frame_params *p, uint32_t w, uint32_t h,
                                         int chroma_420, int depth10)
{
    uint32_t tx = (w + NXE_TILE_SIZE - 1u) / NXE_TILE_SIZE;
    uint32_t ty = (h + NXE_TILE_SIZE - 1u) / NXE_TILE_SIZE;
    uint32_t nt = tx * ty;
    uint32_t cw = (uint32_t)(chroma_420 ? NXE_CHROMA_WORDS_420 : NXE_CHROMA_WORDS_444);
    p->width  = w;
    p->height = h;
    p->tiles_x = tx;
    p->tiles_y = ty;
    p->plane_y_off  = 0u;
    p->plane_co_off = nt * (uint32_t)NXE_LUMA_WORDS_PER_TILE;
    p->plane_cg_off = p->plane_co_off + nt * cw;
    p->flags = (uint32_t)(depth10 ? NXE_TS_F_10BIT : 0)
             | (uint32_t)(chroma_420 ? NXE_TS_F_CHROMA_420 : 0);
}

static inline void nxe_frame_params_init2(nxe_frame_params *p, uint32_t w, uint32_t h,
                                          int chroma_420, int depth10, int src_fmt)
{
    nxe_frame_params_init(p, w, h, chroma_420, depth10);
    if (src_fmt == NXE_SRC_YCBCR_2PLANE) p->flags |= NXE_TS_F_YCBCR;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NXE_TILE_STATS_H */
