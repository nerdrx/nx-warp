/* stats_cpu.h -- bit-exact CPU models of the encoder analysis kernels.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * These are the specification the GPU shaders are validated against, in the
 * same relationship paper 3.7 sets up for the decoder ("the CPU reference
 * decoder is the specification; SPIR-V is validated against it, not the other
 * way round").  They are scalar, integer-only, single-threaded and slow, and
 * they exist to be obviously correct, not fast.
 *
 * Bit-exactness rests on three things and no more:
 *   1. Every accumulator is an integer sum, so the reduction order the GPU
 *      picks -- which depends on subgroup width, and paper 3.7 warns that on
 *      RDNA the driver picks 32 or 64 as it likes -- cannot change the answer.
 *   2. Signed accumulators are added through uint32_t so that overflow wraps
 *      two's-complement here exactly as it does in SPIR-V, rather than being
 *      undefined behaviour in C.
 *   3. Every rounding is a shift by a literal, written the same way in both
 *      languages.
 */

#ifndef NXE_STATS_CPU_H
#define NXE_STATS_CPU_H

#include "tile_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- primitives */

/* Arithmetic (sign-propagating) right shift.  C++20 and C23 define >> on a
 * negative signed value as an arithmetic shift, and every compiler this
 * project targets has always done so; the assertion in nxe_stats_selftest()
 * makes the assumption fail loudly rather than silently if that ever changes. */
static inline int32_t nxe_sar(int32_t x, int s) { return x >> s; }

/* Floor of sqrt(v).  Restoring binary search, 16 iterations, no division and
 * no float -- the exact algorithm nxe_isqrt() in nxe_common.glsl runs. */
static inline uint32_t nxe_isqrt(uint32_t v)
{
    uint32_t rem = 0, root = 0;
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

/* YCoCg-R forward (paper 1.3).  Exactly invertible over integers. */
static inline void nxe_rgb_to_ycocgr(int32_t r, int32_t g, int32_t b,
                                     int32_t *y, int32_t *co, int32_t *cg)
{
    int32_t t;
    *co = r - b;
    t   = b + nxe_sar(*co, 1);
    *cg = g - t;
    *y  = t + nxe_sar(*cg, 1);
}

/* Packed-plane access.  Samples are 16-bit two's complement, two per word,
 * low half first. */
static inline int32_t nxe_plane_get(const uint32_t *plane, uint32_t words,
                                    uint32_t sample_index)
{
    uint32_t w = sample_index >> 1;
    uint32_t h = sample_index & 1u;
    uint32_t v = (w < words) ? plane[w] : 0u;
    /* Sign-extend the selected 16-bit half. */
    return (int32_t)((int16_t)((v >> (h * 16u)) & 0xffffu));
}

static inline void nxe_plane_set(uint32_t *plane, uint32_t words,
                                 uint32_t sample_index, int32_t value)
{
    uint32_t w = sample_index >> 1;
    uint32_t h = sample_index & 1u;
    uint32_t m, s;
    if (w >= words) return;
    s = h * 16u;
    m = 0xffffu << s;
    plane[w] = (plane[w] & ~m) | ((((uint32_t)value) & 0xffffu) << s);
}

/* ------------------------------------------------------------------ E0 model
 *
 * src is the import image as tightly packed 4-component samples in raster
 * order, one uint32_t per component (the harness expands both RGBA8 and
 * RGB10A2 to this form so the model does not need a format switch).
 * `dst` receives the full Y/Co/Cg plane set, sized nxe_plane_total_words().
 */
void nxe_e0_convert_cpu(const uint32_t *src_rgba, uint32_t src_w, uint32_t src_h,
                        const nxe_frame_params *fp, int chroma_420,
                        uint32_t *dst, uint32_t dst_words);

/* ------------------------------------------------------------------ E1 model
 *
 * `luma` and `ref_luma` are tile-major packed luma planes (E0 output, offset
 * fp->plane_y_off already applied by the caller passing the whole buffer).
 * `mv_q` is one ivec2 per tile in quarter-pel, `has_ref` selects whether SAD
 * is computed at all.
 */
void nxe_e1_stats_cpu(const uint32_t *plane, uint32_t plane_words,
                      const nxe_frame_params *fp,
                      const uint32_t *ref_plane,
                      const int32_t *mv_q, int has_ref,
                      nxe_tile_stats *out, uint32_t num_tiles);

/* ------------------------------------------------------------------ E2 model
 *
 * Exclusive prefix sum, wrapping modulo 2^32 exactly as the shader does.
 * Returns the grand total.
 */
uint32_t nxe_e2_prefix_cpu(const uint32_t *in, uint32_t *out, uint32_t n);

/* Assumptions the models make about the host C implementation.  Returns 0 on
 * success, or a negative code naming the assumption that failed. */
int nxe_stats_selftest(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NXE_STATS_CPU_H */
