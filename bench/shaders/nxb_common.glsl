// NX Warp Phase 0 bench -- shared normative integer core.
//
// Rules from PAPER.md 3.2.6 and 3.7 that this file exists to enforce:
//   * int32 arithmetic only in the normative path. No float, no fp16, no int64.
//   * No OpSDiv / OpSRem / OpSMod.
//   * Rounding shifts are (x + (1 << (s-1))) >> s with an arithmetic shift,
//     and s is always a compile-time literal.
//   * Every load is bounds-clamped in the shader; robustBufferAccess semantics
//     differ across vendors and the codec may not depend on them.
//   * Clamp ranges are normative so overflow cannot differ by vendor.
#ifndef NXB_COMMON_GLSL
#define NXB_COMMON_GLSL

// ---------------------------------------------------------------- geometry
#define NXB_TILE        64            // normative tile edge (PAPER 6.2)
#define NXB_BLOCK       8             // transform block edge (PAPER 1.4)
#define NXB_BLOCKS_PER_TILE   64      // 64x64 / 8x8, single plane (see README)
#define NXB_COEFS_PER_TILE    4096    // 64 blocks * 64 coefficients
#define NXB_RANS_LANES  8             // PAPER 6.3: v1 fixes eight lanes

// ------------------------------------------------------------ integer ops
// Rounding arithmetic right shift. s must be a literal >= 1.
#define NXB_RSHIFT(x, s)   (((x) + (1 << ((s) - 1))) >> (s))
#define NXB_CLAMP(x, lo, hi) (clamp((x), (lo), (hi)))

// Normative clamp ranges (see bench/README.md "Transform normalisation").
#define NXB_DEQUANT_CLAMP   8191      // dequantised coefficient, +-2^13-1
#define NXB_STAGE1_CLAMP    32767     // after the row transform: fits int16 LDS
#define NXB_RESIDUAL_CLAMP  2048      // final residual range, +-2048

// Two-stage transform shifts (PAPER 1.4 defines a two-stage shift; the exact
// pair is derived in bench/README.md -- 8 then 12, total 2^20, which is what a
// 9-bit-constant Loeffler pair actually normalises to).
#define NXB_XFORM_SHIFT1   8
#define NXB_XFORM_SHIFT2   12

// ----------------------------------------------- 9-bit Loeffler constants
// round(512 * cos(k*pi/16)) / round(512 * sin(k*pi/16)). All < 512, so every
// product of a constant with a 16-bit intermediate stays well inside int32.
#define NXB_C4  362   // 512*cos(pi/4)
#define NXB_C2  473   // 512*cos(pi/8)
#define NXB_S2  196   // 512*sin(pi/8)
#define NXB_C1  502   // 512*cos(pi/16)
#define NXB_S1  100   // 512*sin(pi/16)
#define NXB_C3  426   // 512*cos(3pi/16)
#define NXB_S3  284   // 512*sin(3pi/16)

// Inverse 8-point integer DCT, Loeffler-Ligtenberg-Moschytz flow graph with
// our own 9-bit constants. Input F[0..7] are (dequantised) coefficients,
// output f[0..7] are scaled by 2^9 relative to the true IDCT; the caller
// applies the stage shift. Pure int32, 16 multiplies, 26 adds.
// ---------------------------------------------------------------------------
// Eight values in two ivec4s, not an int[8]
// ---------------------------------------------------------------------------
// This kernel is where the Adreno 650's handling of local storage was first
// measured, so it is the last place that should keep an `inout int F[8]`.  A
// local array is lowered to private (scratch) memory there rather than to
// registers, an array *parameter* is copied in and out on top of that, and Pass
// B's own bisect caught such a value coming back wrong.  See
// docs/ADRENO-RULES.md and the transposed-store note in passb.comp.
//
// So eight values travel as `lo` (elements 0..3) and `hi` (4..7), and the two
// accessors below turn a runtime index into a select ladder -- predicated moves,
// never a memory access.
int nxb_at8(ivec4 lo, ivec4 hi, int k)
{
    ivec4 h = (k < 4) ? lo : hi;
    int j = k & 3;
    return (j == 0) ? h.x : ((j == 1) ? h.y : ((j == 2) ? h.z : h.w));
}

void nxb_set8(inout ivec4 lo, inout ivec4 hi, int k, int v)
{
    if      (k == 0) lo.x = v;
    else if (k == 1) lo.y = v;
    else if (k == 2) lo.z = v;
    else if (k == 3) lo.w = v;
    else if (k == 4) hi.x = v;
    else if (k == 5) hi.y = v;
    else if (k == 6) hi.z = v;
    else             hi.w = v;
}

uint nxb_atu4(uvec4 v, int k)
{
    return (k == 0) ? v.x : ((k == 1) ? v.y : ((k == 2) ? v.z : v.w));
}

void nxb_idct8(inout ivec4 Flo, inout ivec4 Fhi)
{
    // ---- even part: 4-point inverse on F0,F2,F4,F6
    int a = NXB_C4 * (Flo.x + Fhi.x);
    int b = NXB_C4 * (Flo.x - Fhi.x);
    int c = NXB_C2 * Flo.z + NXB_S2 * Fhi.z;
    int d = NXB_S2 * Flo.z - NXB_C2 * Fhi.z;

    int e0 = a + c;
    int e1 = b + d;
    int e2 = b - d;
    int e3 = a - c;

    // ---- odd part: two rotations, a butterfly, then a pi/4 rotation
    int t1 = NXB_C1 * Flo.y + NXB_S1 * Fhi.w;
    int t7 = NXB_C1 * Fhi.w - NXB_S1 * Flo.y;
    int t3 = NXB_C3 * Flo.w + NXB_S3 * Fhi.y;
    int t5 = NXB_C3 * Fhi.y - NXB_S3 * Flo.w;

    int u1 = t1 + t3;
    int u3 = t1 - t3;
    int u7 = t7 + t5;
    int u5 = t7 - t5;

    // The pi/4 rotation carries its own 2^9, so its two outputs come back
    // down by 9 to stay on the same scale as u1 / u5.
    int o0 = u1;
    int o1 = NXB_RSHIFT(NXB_C4 * (u3 - u7), 9);
    int o2 = NXB_RSHIFT(NXB_C4 * (u3 + u7), 9);
    int o3 = -u5;

    Flo.x = e0 + o0;
    Fhi.w = e0 - o0;
    Flo.y = e1 + o1;
    Fhi.z = e1 - o1;
    Flo.z = e2 + o2;
    Fhi.y = e2 - o2;
    Flo.w = e3 + o3;
    Fhi.x = e3 - o3;
}

// ------------------------------------------------------- YCoCg-R to RGB
// Malvar/Sullivan lifting, exactly invertible, 5 adds and shifts (PAPER 3.2.3
// step 6). y in [0,255], co/cg are stored biased by 128.
ivec3 nxb_ycocgr_to_rgb(int y, int co, int cg)
{
    int t = y - (cg >> 1);
    int g = cg + t;
    int bl = t - (co >> 1);
    int r = bl + co;
    return ivec3(r, g, bl);
}

// ------------------------------------------------------- packed int16 I/O
// Coefficients live as dense int16 but are addressed as uint so that no
// 16-bit storage feature is required (see README: portability).
#define NXB_UNPACK_LO(w)  ((int(w) << 16) >> 16)
#define NXB_UNPACK_HI(w)  (int(w) >> 16)
#define NXB_PACK16(lo, hi) ((uint(lo) & 0xffffu) | (uint(hi) << 16))

// ------------------------------------------------------------ tile record
// 32 bytes. The paper's 16-byte record is the four Q4 corner displacements;
// the bench carries QP/mode/flags alongside instead of in a side buffer.
struct NxbTileRec
{
    uint corner[4];   // per corner: int16 dx in low half, int16 dy in high half
    uint qp;          // quantiser index 0..63
    uint mode;        // 0 skip, 1 inter, 2 intra
    uint flags;
    uint pad;
};

// The four packed corner words of a record, as a uvec4.  The interpolator takes
// this rather than the NxbTileRec itself: a by-value struct parameter is copied
// into a Function-storage temporary, which the Adreno 650 puts in private
// (scratch) memory instead of registers -- the same lowering that made Pass B
// read a local array back wrong.  See docs/ADRENO-RULES.md.
#define NXB_CORNERS(rec) \
    uvec4((rec).corner[0], (rec).corner[1], (rec).corner[2], (rec).corner[3])

// Bilinear interpolation of the four Q4 corner displacements over the tile.
// u, v are 0..63 inside the tile. Six integer ops per component as costed in
// PAPER 3.2.5. Corner weights sum to 64*64, hence the shift by 12.
ivec2 nxb_warp_delta(uvec4 corner, int u, int v)
{
    int iu = NXB_TILE - u;
    int iv = NXB_TILE - v;

    int w00 = iu * iv;
    int w10 = u  * iv;
    int w01 = iu * v;
    int w11 = u  * v;

    int d00x = NXB_UNPACK_LO(corner.x); int d00y = NXB_UNPACK_HI(corner.x);
    int d10x = NXB_UNPACK_LO(corner.y); int d10y = NXB_UNPACK_HI(corner.y);
    int d01x = NXB_UNPACK_LO(corner.z); int d01y = NXB_UNPACK_HI(corner.z);
    int d11x = NXB_UNPACK_LO(corner.w); int d11y = NXB_UNPACK_HI(corner.w);

    int dx = w00 * d00x + w10 * d10x + w01 * d01x + w11 * d11x;
    int dy = w00 * d00y + w10 * d10y + w01 * d01y + w11 * d11y;
    return ivec2(NXB_RSHIFT(dx, 12), NXB_RSHIFT(dy, 12));
}

#endif // NXB_COMMON_GLSL
