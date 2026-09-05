// nxe_enc_common.glsl -- shared declarations for the NX Warp encoder coding
// kernels E3 (forward.comp), E4 (rans_encode.comp) and E5 (packetize.comp).
//
// SPDX-License-Identifier: Apache-2.0
//
// GLSL mirror of `nxe_enc.h`, `nxe_tables.h`, `forward_cpu.h` and
// `rans_cpu.h`.  The CPU models are the specification; every function here is
// the same arithmetic in the same order, and the harness diffs the two.
//
// Rules obeyed throughout, as in nxe_common.glsl:
//   * int32 arithmetic only.  No float, no fp16, no int64 type.
//   * Rounding shifts are (x + (1 << (s-1))) >> s with an arithmetic shift.
//   * Every buffer load is bounds-clamped; robustBufferAccess is not relied on.
//   * Workgroup size is never assumed to be one subgroup.
//
// The one deliberate exception to the decoder's rules is integer division.
// The quantiser is `(a*16 + dz) / t` and the rANS encoder needs `x / f` and
// `x % f`; both are encoder-side, both are non-normative (docs/SYNTAX.md 6.5
// is explicit that the quantiser is informative), and paper 3.6 says so:
// "rANS encoding needs x / freq; integer division on GPU is 20 to 40
// instructions but this is the PC side and 8 lanes per tile".  A reciprocal
// table is the optimisation, not a correctness requirement.

#ifndef NXE_ENC_COMMON_GLSL
#define NXE_ENC_COMMON_GLSL

#extension GL_KHR_shader_subgroup_basic      : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#extension GL_KHR_shader_subgroup_ballot     : require

// ------------------------------------------------------------- constants
#define NXE_TILE            64
#define NXE_MAX_PLANES      3
#define NXE_NUM_SYM         16
#define NXE_MAX_CTX         32
#define NXE_NCTX_V2         16
#define NXE_NCTX_V3         27

#define NXE_CTX_CBF_LUMA    0
#define NXE_CTX_CBF_CHROMA  1
#define NXE_CTX_LAST_LUMA   2
#define NXE_CTX_LAST_CHROMA 3
#define NXE_CTX_LEVEL_BASE  4
#define NXE_CTX_CBF_DC      12
#define NXE_CTX_LAST_DC     13
#define NXE_CTX_LEVEL_DC    14
#define NXE_CTX_MODE        15
#define NXE_CTX_NONE        0

// The v3 model, tool bit 25.  nxe_enc.h carries the reasoning; these are the
// eleven rows v3 adds on top of v2's sixteen.
#define NXE_CTX_CBF_LUMA_N    16   // 16..18, + (nbr - 1)
#define NXE_CTX_CBF_CHROMA_N  19   // 19..21, + (nbr - 1)
#define NXE_CTX_LAST_LUMA_N   22
#define NXE_CTX_LAST_CHROMA_N 23
#define NXE_CTX_LEVEL_DC0     24
#define NXE_CTX_LEVEL_LAST_LO 25
#define NXE_CTX_LEVEL_LAST_HI 26
#define NXE_NBR_DENSE_LAST    4

#define NXE_UCLS_LUMA       0
#define NXE_UCLS_CHROMA     1
#define NXE_UCLS_DC         2

#define NXE_ESC_SYM         15
#define NXE_ESC_ORDER       3
#define NXE_ESC_MAX_PREFIX  16
#define NXE_SDH_MIN_LAST    4
#define NXE_NUM_INTRA_MODES 9

#define NXE_RANS_L          (1u << 16)
#define NXE_PROB_BITS       10

#define NXE_PLANE_COEFS_444 (64 + 64 * 64)
#define NXE_PLANE_COEFS_420 (16 + 16 * 64)
#define NXE_TILE_COEFS_MAX  (3 * NXE_PLANE_COEFS_444)
#define NXE_TILE_COEF_WORDS (NXE_TILE_COEFS_MAX / 2)
#define NXE_TILE_UNITS_MAX  (3 * (1 + 1 + 64))
#define NXE_TILE_UNIT_SLOTS NXE_TILE_UNITS_MAX
#define NXE_UNIT_MAX_OPS    (3 + 64 * (1 + (NXE_ESC_MAX_PREFIX + 1) + 3 + 1))
#define NXE_LANE_OPS_CAP    2048
#define NXE_TILE_SLOT_WORDS (2 + 8 + NXE_TILE_COEFS_MAX)
#define NXE_TILE_SLOT_BYTES (NXE_TILE_SLOT_WORDS * 4)

#define NXE_MODE_WARP_SKIP     0
#define NXE_MODE_STATIC_MV     1
#define NXE_MODE_WARP_MV       2
#define NXE_MODE_INTRA         3
#define NXE_MODE_STEREO        4

#define NXE_FRAME_HEADER_BYTES 40
#define NXE_TABLE_AREA_MAX     2188
#define NXE_ROW_HEADER_BYTES   12
#define NXE_TILE_HEADER_BYTES  8

#define NXE_E3_WG           64
#define NXE_E4_TILES_PER_WG 8
#define NXE_E4_WG           (NXE_E4_TILES_PER_WG * 8)
#define NXE_E5_WG           256

#define NXE_OP_SYM          0u
#define NXE_OP_BYPASS       1u
#define NXE_OP_PACK(k, a, v) ((k) | (uint(a) << 1) | (uint(v) << 8))
#define NXE_OP_KIND(w)      ((w) & 1u)
#define NXE_OP_ARG(w)       (((w) >> 1) & 31u)
#define NXE_OP_VALUE(w)     (((w) >> 8) & 255u)

// 9-bit transform constants (ref/src/transform.h).
#define NXE_C4 362
#define NXE_C2 473
#define NXE_S2 196
#define NXE_A1 502
#define NXE_A3 426
#define NXE_A5 284
#define NXE_A7 100

// ------------------------------------------------- specialization constants
//
// The transform edge and the directional-intra switch.  Both exist for the
// merge that is adding large transforms and inter tools: a 16x16 transform is
// this source compiled with NXE_SC_XFORM_LOG2 = 4, not a second shader.  The
// context count is a *push* constant (nxe_frame_params.nctx) because it does
// not change any loop bound, only how much of the table buffer is live.
layout(constant_id = 0) const int NXE_SC_INTRA_DIR  = 0;
layout(constant_id = 1) const int NXE_SC_XFORM_LOG2 = 3;

#define NXE_XB   (1 << NXE_SC_XFORM_LOG2)      // transform edge
#define NXE_XN   (NXE_XB * NXE_XB)             // coefficients per block

// -------------------------------------------------------- the frame record
// std430 mirror of nxe_frame_params.
struct nxe_frame_params {
    uint tiles_x, tiles_y, eyes, ntiles;
    uint chroma420, base_qp;
    int  chroma_qp_off;
    uint nctx;
    uint sdh, intra_dir, dir_layer, nsub_log2;
    uint frame_number, frame_flags, quant_matrix, tables_present;
    uint width, height, ycocgr, table_bytes;
    uint warp_bytes, ref_slots, pad1, pad2;
    uint wm_luma[64];
    uint wm_chroma[64];
};

// std430 mirror of nxe_tile_job.
struct nxe_tile_job {
    uint tile, col, row, eye;
    int  qp_delta;
    uint table_set, tskip, wm_id;
    uint chroma444, res_level, mode, nsub_log2;
    uint payload_len, tile_bytes, nunits, flags;
};

// ------------------------------------------------------------------ tables
// Transcribed from ref/src/tables.cpp; nxe_tables.c holds the same values and
// nxvc-vkenc asserts all three copies against each other.
//
// They are copied into shared memory at the top of every kernel and read from
// there.  A `const int[64]` indexed by a runtime value is not a lookup on this
// hardware -- there is no constant-address space for it, so the compiler
// emits a select chain, sixty-four v_cndmask for one zigzag position.  The
// entropy kernel does that lookup a few thousand times per tile, and moving
// these five arrays into LDS was worth more than every other change made to
// it.  The copy costs one dispatch-wide pass over 380 words.
const int nxe_k_qstep[64] = int[64](
       16,    18,    20,    23,    25,    29,    32,    36,
       40,    45,    51,    57,    64,    72,    81,    91,
      102,   114,   128,   144,   161,   181,   203,   228,
      256,   287,   323,   362,   406,   456,   512,   575,
      645,   724,   813,   912,  1024,  1149,  1290,  1448,
     1625,  1825,  2048,  2299,  2580,  2896,  3251,  3649,
     4096,  4598,  5161,  5793,  6502,  7298,  8192,  9195,
    10321, 11585, 13004, 14596, 16384, 18390, 20643, 23170);

const int nxe_k_weight[4 * 64] = int[4 * 64](
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 17, 18, 19, 20, 21, 22, 23, 17, 18, 19, 20, 21, 22, 23, 24,
    18, 19, 20, 21, 22, 23, 24, 25, 19, 20, 21, 22, 23, 24, 25, 26,
    20, 21, 22, 23, 24, 25, 26, 27, 21, 22, 23, 24, 25, 26, 27, 28,
    22, 23, 24, 25, 26, 27, 28, 29, 23, 24, 25, 26, 27, 28, 29, 30,
    16, 18, 20, 22, 24, 26, 28, 30, 18, 20, 22, 24, 26, 28, 30, 32,
    20, 22, 24, 26, 28, 30, 32, 32, 22, 24, 26, 28, 30, 32, 32, 32,
    24, 26, 28, 30, 32, 32, 32, 32, 26, 28, 30, 32, 32, 32, 32, 32,
    28, 30, 32, 32, 32, 32, 32, 32, 30, 32, 32, 32, 32, 32, 32, 32,
    16, 17, 19, 20, 22, 23, 25, 26, 17, 19, 20, 22, 23, 25, 26, 28,
    19, 20, 22, 23, 25, 26, 28, 29, 20, 22, 23, 25, 26, 28, 29, 31,
    22, 23, 25, 26, 28, 29, 31, 32, 23, 25, 26, 28, 29, 31, 32, 32,
    25, 26, 28, 29, 31, 32, 32, 32, 26, 28, 29, 31, 32, 32, 32, 32);

const int nxe_k_zigzag8[64] = int[64](
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63);
const int nxe_k_zigzag4[16] =
    int[16](0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15);
const int nxe_k_last_base[16] =
    int[16](0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 24, 32, 48, 64);
const int nxe_k_last_raw_bits[16] =
    int[16](0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 3, 4, 4, 0);
const int nxe_k_level_ctx[12] = int[12](0, 1, 2, 3, 4, 2, 5, 6, 7, 5, 6, 7);

shared int nxe_qstep[64];
shared int nxe_weight[4 * 64];
shared int nxe_zigzag8[64];
shared int nxe_zigzag4[16];
shared int nxe_last_base[16];
shared int nxe_last_raw_bits[16];
shared int nxe_level_ctx_tab[12];

// Call once per dispatch from uniform control flow, then barrier().
void nxe_init_tables(uint lid, uint wgsize) {
    for (uint i = lid; i < 64u; i += wgsize) {
        nxe_qstep[i] = nxe_k_qstep[i];
        nxe_zigzag8[i] = nxe_k_zigzag8[i];
    }
    for (uint i = lid; i < 256u; i += wgsize) nxe_weight[i] = nxe_k_weight[i];
    for (uint i = lid; i < 16u; i += wgsize) {
        nxe_zigzag4[i] = nxe_k_zigzag4[i];
        nxe_last_base[i] = nxe_k_last_base[i];
        nxe_last_raw_bits[i] = nxe_k_last_raw_bits[i];
    }
    for (uint i = lid; i < 12u; i += wgsize)
        nxe_level_ctx_tab[i] = nxe_k_level_ctx[i];
}

// scan_pos -> block-local index.  `n` is the unit's coefficient count.
int nxe_scan(int n, bool tskip, int p) {
    if (n == 64) return tskip ? p : nxe_zigzag8[p];
    if (n == 16) return nxe_zigzag4[p];
    if (n == 4) return p;
    return 0;
}

int nxe_last_class_of(int pos) {
    for (int c = 14; c >= 0; --c)
        if (pos >= nxe_last_base[c]) return c;
    return 0;
}

int nxe_band_of(int p) {
    if (p == 0) return 0;
    if (p < 4) return 1;
    if (p < 10) return 2;
    return 3;
}
int nxe_level_class(int m) { return m == 0 ? 0 : (m == 1 ? 1 : 2); }
int nxe_level_ctx(int p, int prev) {
    return NXE_CTX_LEVEL_BASE + nxe_level_ctx_tab[nxe_band_of(p) * 3 + prev];
}

// ------------------------------------------------- v3 context derivation
// The GLSL half of rans_cpu.c's three functions, which are in turn the
// encode-side mirror of vk/decoder/passA/syntax_constants.h's nxs_v3_ctx_*.
// These are the only places a v3 context is chosen.
int nxe_v3_ctx_cbf(int ucls, int nbr) {
    if (nbr == 0)
        return ucls == NXE_UCLS_DC
                   ? NXE_CTX_CBF_DC
                   : (ucls == NXE_UCLS_CHROMA ? NXE_CTX_CBF_CHROMA
                                              : NXE_CTX_CBF_LUMA);
    return (ucls == NXE_UCLS_CHROMA ? NXE_CTX_CBF_CHROMA_N
                                    : NXE_CTX_CBF_LUMA_N) + (nbr - 1);
}
int nxe_v3_ctx_last(int ucls, int nbr) {
    if (nbr < 2)
        return ucls == NXE_UCLS_DC
                   ? NXE_CTX_LAST_DC
                   : (ucls == NXE_UCLS_CHROMA ? NXE_CTX_LAST_CHROMA
                                              : NXE_CTX_LAST_LUMA);
    return ucls == NXE_UCLS_CHROMA ? NXE_CTX_LAST_CHROMA_N
                                   : NXE_CTX_LAST_LUMA_N;
}
int nxe_v3_ctx_level(int ucls, int scan_pos, int band_scan_pos, int last,
                     int prev_class) {
    if (ucls == NXE_UCLS_DC)
        return scan_pos == 0 ? NXE_CTX_LEVEL_DC0 : NXE_CTX_LEVEL_DC;
    if (scan_pos == last)
        return nxe_band_of(band_scan_pos) < 2 ? NXE_CTX_LEVEL_LAST_LO
                                              : NXE_CTX_LEVEL_LAST_HI;
    return nxe_level_ctx(band_scan_pos, prev_class);
}
int nxe_nbr_class_of(int cbf, int last) {
    if (cbf == 0) return 1;
    return last < NXE_NBR_DENSE_LAST ? 2 : 3;
}

// --------------------------------------------------------------- helpers
int nxe_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int nxe_clamp16(int v) { return nxe_clamp(v, -32768, 32767); }

int nxe_unpack16(uint w, int hi) { return bitfieldExtract(int(w), hi * 16, 16); }
uint nxe_pack16(int lo, int hi) {
    return (uint(lo) & 0xffffu) | (uint(hi) << 16);
}

// ------------------------------------------------------------------ 1D DCT
// ref/src/transform.cpp.  The C4 rotation is split as hi*362 +
// ((lo*362 + 256) >> 9) with hi = s >> 9: that is the exact value of
// (s*362 + 256) >> 9 and it cannot overflow int32, which the direct form can.
int nxe_mul_c4_rnd9(int s) {
    int hi = s >> 9, lo = s & 511;
    return hi * NXE_C4 + ((lo * NXE_C4 + 256) >> 9);
}

// ---------------------------------------------------------------------------
// The 8-point butterflies, and why their operands are ivec4 pairs
// ---------------------------------------------------------------------------
// These used to read `int y[8]` and write `out int x[8]`.  Both forms are a
// private-memory array on the Adreno 650: a local array is not promoted to
// registers there, and an array *parameter* is worse still, because glslang
// copies the whole thing in at the call and an `out` array back out at the
// return.  Pass B measured such an array being read back with the wrong
// contents, so this is a correctness rule and not a performance one; see
// docs/ADRENO-RULES.md and commit d47c095.
//
// So a row travels as two ivec4s -- `lo` is elements 0..3, `hi` is 4..7 -- and
// every access below is a constant component select, which is a register on
// every driver.  The arithmetic is unchanged, element for element; the encoder
// and decoder GPU-vs-CPU diffs are what hold that claim up.
//
// Gather 8 consecutive entries of a shared array into such a pair.  `arr` may
// be an indexed expression (`s_blk[g]`) as long as it contains no comma.
#define NXE_GATHER8(arr, base, lo, hi)                                         \
    (lo) = ivec4(arr[(base) + 0], arr[(base) + 1],                             \
                 arr[(base) + 2], arr[(base) + 3]);                            \
    (hi) = ivec4(arr[(base) + 4], arr[(base) + 5],                             \
                 arr[(base) + 6], arr[(base) + 7])

// Scatter a pair back with the rounding shift (v + add) >> sh and the 16-bit
// clamp, at `stride` apart -- stride 8 is the transpose the two transform
// passes rely on.
#define NXE_SCATTER8_RS(arr, base, stride, lo, hi, add, sh)                    \
    arr[(base) + 0 * (stride)] = nxe_clamp16(((lo).x + (add)) >> (sh));        \
    arr[(base) + 1 * (stride)] = nxe_clamp16(((lo).y + (add)) >> (sh));        \
    arr[(base) + 2 * (stride)] = nxe_clamp16(((lo).z + (add)) >> (sh));        \
    arr[(base) + 3 * (stride)] = nxe_clamp16(((lo).w + (add)) >> (sh));        \
    arr[(base) + 4 * (stride)] = nxe_clamp16(((hi).x + (add)) >> (sh));        \
    arr[(base) + 5 * (stride)] = nxe_clamp16(((hi).y + (add)) >> (sh));        \
    arr[(base) + 6 * (stride)] = nxe_clamp16(((hi).z + (add)) >> (sh));        \
    arr[(base) + 7 * (stride)] = nxe_clamp16(((hi).w + (add)) >> (sh))

void nxe_fdct8_1d(ivec4 ylo, ivec4 yhi, out ivec4 xlo, out ivec4 xhi) {
    int e0 = ylo.x + yhi.w, O0 = ylo.x - yhi.w;
    int e1 = ylo.y + yhi.z, O1 = ylo.y - yhi.z;
    int e2 = ylo.z + yhi.y, O2 = ylo.z - yhi.y;
    int e3 = ylo.w + yhi.x, O3 = ylo.w - yhi.x;
    int P = nxe_mul_c4_rnd9(O1 + O2);
    int Q = nxe_mul_c4_rnd9(O1 - O2);
    int A = O0 + P, C = O0 - P;
    int B = O3 + Q, D = Q - O3;
    xlo.y = A * NXE_A1 + B * NXE_A7;
    xhi.w = A * NXE_A7 - B * NXE_A1;
    xlo.w = C * NXE_A3 + D * NXE_A5;
    xhi.y = C * NXE_A5 - D * NXE_A3;
    int t0 = e0 + e3, t3 = e0 - e3;
    int t1 = e1 + e2, t2 = e1 - e2;
    xlo.x = (t0 + t1) * NXE_C4;
    xhi.x = (t0 - t1) * NXE_C4;
    xlo.z = t2 * NXE_S2 + t3 * NXE_C2;
    xhi.z = t3 * NXE_S2 - t2 * NXE_C2;
}

void nxe_idct8_1d(ivec4 xlo, ivec4 xhi, out ivec4 ylo, out ivec4 yhi) {
    int t0 = (xlo.x + xhi.x) * NXE_C4;
    int t1 = (xlo.x - xhi.x) * NXE_C4;
    int t2 = xlo.z * NXE_S2 - xhi.z * NXE_C2;
    int t3 = xlo.z * NXE_C2 + xhi.z * NXE_S2;
    int e0 = t0 + t3, e3 = t0 - t3;
    int e1 = t1 + t2, e2 = t1 - t2;
    int A = xlo.y * NXE_A1 + xhi.w * NXE_A7;
    int B = xlo.y * NXE_A7 - xhi.w * NXE_A1;
    int C = xlo.w * NXE_A3 + xhi.y * NXE_A5;
    int D = xlo.w * NXE_A5 - xhi.y * NXE_A3;
    int O0 = A + C;
    int O3 = B - D;
    int P = A - C, Q = B + D;
    int O1 = nxe_mul_c4_rnd9(P + Q);
    int O2 = nxe_mul_c4_rnd9(P - Q);
    ylo.x = e0 + O0; yhi.w = e0 - O0;
    ylo.y = e1 + O1; yhi.z = e1 - O1;
    ylo.z = e2 + O2; yhi.y = e2 - O2;
    ylo.w = e3 + O3; yhi.x = e3 - O3;
}

// -------------------------------------------------------------- quantizer
int nxe_dequant_step(int qp, int w) { return (nxe_qstep[qp] * w + 8) >> 4; }
int nxe_dequant(int q, int t) { return nxe_clamp16((q * t + 8) >> 4); }
int nxe_quantize(int c, int t, int dz) {
    int a = c < 0 ? -c : c;
    int q = int((uint(a * 16 + dz)) / uint(t));   // encoder-side division
    if (q > 32767) q = 32767;
    return c < 0 ? -q : q;
}

// ------------------------------------------------------------- geometry
int nxe_plane_size(uint chroma444, uint res_level, int p) {
    if (p == 0) return NXE_TILE >> res_level;
    int s = int((chroma444 != 0u ? uint(NXE_TILE) : uint(NXE_TILE / 2)) >>
                res_level);
    return s < 8 ? 8 : s;
}

int nxe_plane_coef_offset(uint chroma444, uint res_level, int p) {
    int off = 0;
    for (int i = 0; i < p; ++i) {
        int nb = nxe_plane_size(chroma444, res_level, i) / 8;
        off += nb * nb + nb * nb * 64;
    }
    return off;
}

#endif // NXE_ENC_COMMON_GLSL
