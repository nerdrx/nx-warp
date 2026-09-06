// NX Warp decoder, the inter predictor's arithmetic, shared by both kernels
// that need it.
//
// Pass W (inter/warp_pred.comp) runs this for the tiles whose residual Pass B
// still has to add.  The Pass B WARP_SKIP module (passB/reconstruct.comp built
// with NXVW_SKIP_STORE) runs it for the tiles that have no residual at all, so
// that a skipped tile's reconstruction is produced in the same kernel that
// stores it and never travels through the WPred buffer.
//
// The two callers differ in exactly two things -- which buffers the parameters
// and the reference ring are bound to, and where the finished samples go -- so
// those are the only two things this file does not contain.  Everything
// between `warp_tile_corners` and `sample_bilinear` is still a LINE-FOR-LINE
// twin of warp/ref/warp_ref.cpp, the normative implementation, and of
// warp/glsl/warp_tile.comp; the three MUST agree on every bit of every sample,
// and there is now one copy of them rather than two to keep in step.
//
// ------------------------------------------------------- what to provide
// The includer defines these BEFORE the #include.  They are functions rather
// than macros so that a mistake is a type error:
//
//   uint nxvwWarpParam(uint i);   the warp parameter buffer, uint-indexed
//   uint nxvwRingWord(uint i);    the reference ring, uint-indexed
//   uint nxvwWarpScratchRead(int i);
//   void nxvwWarpScratchWrite(int i, uint v);
//        one uint per PAIR of horizontally adjacent samples, at least
//        (full * full) / 2 of them, private to this workgroup
//   void nxvwWarpEmit(int i, uint v);
//        where the FINISHED plane goes, at coded extent: pair `i` of
//        (size * size) / 2.  Pass W sends it to the WPred buffer; the Pass B
//        module sends it to the tile's plane slot, which is also its scratch,
//        and says so with
//
//   #define NXVW_WARP_EMIT_IS_SCRATCH
//        which drops the one copy that would otherwise write every word of a
//        res_level-0 plane back over itself.
//
// and these at file scope:
//
//   shared ivec2 sCorner[4];      the four corner coordinates
//   shared int sMeans[64];        the near-skip block-mean field
//   pc.p.{chroma420, colorTransform, chromaQpOff, imageH}
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXVW_WARP_PRED_GLSL
#define NXVW_WARP_PRED_GLSL

// --------------------------------------------- overflow-safe integer helpers
// [REF] warp_ref.cpp shl_u32 / shl_i32_mod / sat_add_i32.  Every value they
// touch comes off the wire, so none of them may rely on a shift or an add
// staying in range; all three are bit-identical to the naive form whenever it
// does.
uint shl_u32(uint v, uint n) { return (v & (0xffffffffu >> n)) << n; }

int shl_i32_mod(int v, uint n) { return int(shl_u32(uint(v), n)); }

int sat_add_i32(int a, int b) {
    uint ua = uint(a);
    uint ub = uint(b);
    uint s = ua + ub;
    if ((((ua ^ s) & (ub ^ s)) & 0x80000000u) != 0u) {
        return (a < 0) ? int(0x80000000u) : 0x7fffffff;
    }
    return int(s);
}

// ------------------------------------------------------- emulated 64-bit
// u64 is (x = lo, y = hi).  [REF] warp_ref.cpp nxvc_umul_ext and friends.
uvec2 umul_ext(uint a, uint b) {
    uint msb, lsb;
    umulExtended(a, b, msb, lsb);   // OpUMulExtended
    return uvec2(lsb, msb);
}

uvec2 imul_ext(int a, int b) {
    uvec2 r = umul_ext(uint(a), uint(b));
    // signed_hi = unsigned_hi - (a<0 ? b : 0) - (b<0 ? a : 0)
    if (a < 0) r.y -= uint(b);
    if (b < 0) r.y -= uint(a);
    return r;
}

uvec2 add64(uvec2 a, uvec2 b) {
    uint lo = a.x + b.x;
    uint hi = a.y + b.y + ((lo < a.x) ? 1u : 0u);
    return uvec2(lo, hi);
}

uvec2 neg64(uvec2 a) {
    uint lo = ~a.x + 1u;
    uint hi = ~a.y + ((lo == 0u) ? 1u : 0u);
    return uvec2(lo, hi);
}

uvec2 shl64(uvec2 a, uint n) {
    if (n == 0u) return a;
    return uvec2(shl_u32(a.x, n), shl_u32(a.y, n) | (a.x >> (32u - n)));
}

uvec2 from_i32(int v) { return uvec2(uint(v), (v < 0) ? 0xffffffffu : 0u); }

// Fixed 32-iteration restoring division, branchless.
// Precondition: d != 0, n.hi < d, d < 2^30 -- guaranteed by the frame
// header's four warp_ext() conditions ([SYN] 3.1.1), which the host checks
// before a single sample is predicted.
uint warp_div(uvec2 n, uint d) {
    uint rem = n.y;
    uint q = 0u;
    for (int k = 31; k >= 0; --k) {
        rem = (rem << 1) | ((n.x >> uint(k)) & 1u);
        uint ge = (rem >= d) ? 1u : 0u;
        rem -= ge * d;
        q |= ge << uint(k);
    }
    return q;
}

// ------------------------------------------------------ step 1: corners
// [REF] warp_ref.cpp corner_component().
int corner_component(int h_a, int h_b, int h_c, int cx, int cy, int den,
                     int origin) {
    uvec2 num = add64(add64(imul_ext(h_a, cx), imul_ext(h_b, cy)), from_i32(h_c));

    bool neg = (num.y & 0x80000000u) != 0u;
    uvec2 mag = neg ? neg64(num) : num;
    mag = shl64(mag, uint(kWarpDivShift));
    mag = add64(mag, from_i32(den >> 1));   // round half away from zero

    uint ud = uint(den);
    int v;
    if (mag.y >= ud) {
        v = neg ? -kWarpCornerClamp : kWarpCornerClamp;
    } else {
        uint q = warp_div(mag, ud);
        // Negate through uint: q is unrestricted and -int(q) is UB at 2^31.
        v = int(neg ? 0u - q : q);
    }
    v = sat_add_i32(v, shl_i32_mod(origin, uint(kWarpQCorner)));
    return clamp(v, -kWarpCornerClamp, kWarpCornerClamp);
}

// The matrix is read field by field out of the SSBO rather than copied into a
// local record.  docs/ADRENO-RULES.md: a by-value struct or a local array the
// compiler cannot prove is constant-indexed lands in private memory on the
// Adreno 650, and that is what made Pass B read a local array back wrong.
// Every index below is a constant plus a workgroup-uniform base, which is a
// plain load.
//
// [REF] warp_ref.cpp warp_tile_corners().  `mat` is the uint index of the
// matrix record; `tox`/`toy` are the tile's origin in the PLANE's samples.
void compute_corner(int i, uint mat, int tox, int toy, int wmode) {
    if (wmode == kWarpModeStatic) {
        // STATIC_MV and STEREO: the identity predictor, exactly.  No
        // homography, no divide.  Saturated exactly like the warped path
        // below -- warp.h promises every corner is inside +-kWarpCornerClamp
        // and the in-tile interpolation's overflow argument depends on it.
        int px = int(uint(tox) + (((i & 1) != 0) ? uint(kWarpTile) : 0u));
        int py = int(uint(toy) + (((i >> 1) != 0) ? uint(kWarpTile) : 0u));
        sCorner[i] = ivec2(
            clamp(shl_i32_mod(px, uint(kWarpQCorner)), -kWarpCornerClamp,
                  kWarpCornerClamp),
            clamp(shl_i32_mod(py, uint(kWarpQCorner)), -kWarpCornerClamp,
                  kWarpCornerClamp));
        return;
    }
    int ox = int(nxvwWarpParam(mat + 9u));
    int oy = int(nxvwWarpParam(mat + 10u));
    int cx = int(uint(tox) + (((i & 1) != 0) ? uint(kWarpTile) : 0u) - uint(ox));
    int cy = int(uint(toy) + (((i >> 1) != 0) ? uint(kWarpTile) : 0u) - uint(oy));

    uvec2 d64 = add64(add64(imul_ext(int(nxvwWarpParam(mat + 6u)), cx),
                            imul_ext(int(nxvwWarpParam(mat + 7u)), cy)),
                      from_i32(int(nxvwWarpParam(mat + 8u))));
    int den = int(d64.x);
    bool den_ok = (d64.y == ((den < 0) ? 0xffffffffu : 0u)) &&
                  den >= kWarpDenMin && den < kWarpDenMax;
    if (!den_ok) {
        // Behind the camera or outside the validated envelope.  Saturate.
        sCorner[i] = ivec2(kWarpCornerClamp, kWarpCornerClamp);
        return;
    }
    sCorner[i] = ivec2(corner_component(int(nxvwWarpParam(mat + 0u)),
                                        int(nxvwWarpParam(mat + 1u)),
                                        int(nxvwWarpParam(mat + 2u)), cx, cy, den, ox),
                       corner_component(int(nxvwWarpParam(mat + 3u)),
                                        int(nxvwWarpParam(mat + 4u)),
                                        int(nxvwWarpParam(mat + 5u)), cx, cy, den, oy));
}

// ------------------- step 2: in-tile bilinear interpolation of the corners
// Two rounded steps, not one: it keeps the intermediate at 2^25 so the corner
// clamp can be +-8192 pel instead of +-4096.  [REF] warp_ref.cpp
// bilerp_corner(), which carries the argument in full.
//
// The span is kWarpTile == 64 whatever the plane's extent is.  That is
// [SYN] 13.7's chroma caveat stated as code: warp_tile() emits a 64x64 block
// and a 4:2:0 chroma tile takes its top-left 32x32, so the corner basis a
// chroma sample is interpolated in is fitted over 64 chroma samples and not
// over 32.  Both sides of the codec do this, so it is exact; a decoder that
// re-fitted the basis at 32 would produce different samples.
int bilerp_corner(int v00, int v10, int v01, int v11, int u, int v) {
    int top = (v00 * (kWarpTile - u) + v10 * u + (kWarpTile / 2)) >> 6;
    int bot = (v01 * (kWarpTile - u) + v11 * u + (kWarpTile / 2)) >> 6;
    return (top * (kWarpTile - v) + bot * v + (kWarpTile / 2)) >> 6;
}

// ------------------------------------------------------ step 4: the filter
// Version 1 is bilinear in every profile: tool bit 23 FILTER_CATMULL_ROM is
// not defined for version 1 and the stream-header check refuses it, so there
// is exactly one legal predicted sample for every conforming stream
// ([SYN] 13.4).  The tap table stays normative for the version 2 bit and is
// deliberately NOT carried here, because carrying it would put a filter
// selection in the inner loop that no v1 stream can reach.

// The reference image is one eye's sub-picture of one plane of one ring slot.
// Clamp-to-edge, applied per tap on integer sample indices, INSIDE the eye:
// [REF] warp_ref.cpp fetch() over ref_image()'s RefImage, whose width is the
// per-eye plane width and whose data pointer is already offset by the eye.
int refElemBase = 0;   // u16 element index of (0, 0) of the eye sub-picture
int refStride = 0;     // u16 row stride of the whole plane
int refW = 0, refH = 0;

int fetchRef(int x, int y) {
    x = clamp(x, 0, refW - 1);
    y = clamp(y, 0, refH - 1);
    uint e = uint(refElemBase + y * refStride + x);
    return int((nxvwRingWord(e >> 1u) >> ((e & 1u) * 16u)) & 0xffffu);
}

#ifdef NXVW_ABL_STAGE
// ABLATION ONLY.  A staging block for the tile's source footprint, sized 2048
// uints (4096 u16 = a bare 64x64 with no warp margin) so the probe's own index
// arithmetic is a mask rather than a modulo and does not itself become the
// measurement.  A real staging would need the margin and so ~2592 words, about
// 27 % more copy than this prices.
//
// Two switches, so the two costs are separable:
//   NXVW_ABL_STAGE_COPY  do the copy, leave the taps on the ring  -> copy cost
//   NXVW_ABL_STAGE_TAP   take the taps from the block, do not fill it -> tap cost
// Neither produces a correct picture; both produce the right memory traffic.
const uint kStageWords = 2048u;
shared uint sSrc[2048];
#endif

// The two horizontally adjacent taps of one row, in one or two loads instead
// of two.  The ring is u16 packed two per uint, so `x` and `x + 1` land in the
// SAME uint whenever the clamped `x` is even -- which is half the time, the
// source coordinate being whatever the warp lands on.  Same samples, same
// clamping, one fewer load when they share a word.
//
// The clamp has to be applied to each x separately before the comparison: at
// the right edge both clamp to refW - 1 and share a word trivially, and at a
// row start they do not.
void fetchRefPair(int x, int y, out int a, out int b) {
    const int xa = clamp(x, 0, refW - 1);
    const int xb = clamp(x + 1, 0, refW - 1);
    const int yc = clamp(y, 0, refH - 1);
    const uint base = uint(refElemBase + yc * refStride);
    const uint ea = base + uint(xa);
    const uint eb = base + uint(xb);
#if defined(NXVW_ABL_STAGE) && NXVW_ABL_STAGE_TAP
    const uint wa = sSrc[(ea >> 1u) & (kStageWords - 1u)];
    const uint wb = ((eb >> 1u) == (ea >> 1u))
                        ? wa
                        : sSrc[(eb >> 1u) & (kStageWords - 1u)];
#else
    const uint wa = nxvwRingWord(ea >> 1u);
    const uint wb = ((eb >> 1u) == (ea >> 1u)) ? wa : nxvwRingWord(eb >> 1u);
#endif
    a = int((wa >> ((ea & 1u) * 16u)) & 0xffffu);
    b = int((wb >> ((eb & 1u) * 16u)) & 0xffffu);
}

int sample_bilinear(int ix, int iy, int fx, int fy) {
#ifdef NXVW_ABL_COPYWARP
    // ABLATION ONLY, and it produces a wrong picture whenever the tile's
    // displacement is not already integer: one ring fetch instead of four and
    // no interpolation, which is exactly what the identity fast path would
    // cost.  It prices that path's CEILING before anyone writes it.
    return fetchRef(ix, iy);
#endif
    int gx = 16 - fx;
    int gy = 16 - fy;
    int t00, t10, t01, t11;
    fetchRefPair(ix, iy, t00, t10);
    fetchRefPair(ix, iy + 1, t01, t11);
    // Four INDEPENDENT products, deliberately.  `gy * (gx * t00 + fx * t10) +
    // fy * (gx * t01 + fx * t11)` is the same integer -- nothing rounds before
    // the shift, so the regrouping is exact -- and it is six multiplies rather
    // than eight.  It was measured and it is 22 % SLOWER on the Adreno 650,
    // because the flat form's four products issue in parallel and the
    // factored form is a dependency chain.  See "what does NOT work" in
    // ../passB/README.md.
    int acc = gx * gy * t00 + fx * gy * t10 + gx * fy * t01 + fx * fy * t11;
    return (acc + 128) >> 8;   // the weights sum to 256
}

// ------------------------------------------------------ near-skip [SYN] 13.9
// The block-mean field a near-skip tile carries instead of a coded DC plane:
// a level and two ramps in nine signed bytes, dequantised through the DC
// plane's own step, because the two are the same quantity written two ways.
// [REF] codec.cpp reconstruct_near_skip().
int dequantStepW(int qp, int w) {
    return (kQStep[qp] * w + kQStepRound) >> kQStepShift;
}
int dequantW(int q, int t) {
    int v = (q * t + kDequantRound) >> kDequantShift;
    return clamp(v, -32768, 32767);
}
int signByte(uint v) {
    int b = int(v & 255u);
    return b >= 128 ? b - 256 : b;
}
int log2of(int n) {
    int k = 0;
    while ((1 << k) < n) ++k;
    return k;
}

// The Q4 bilinear of [SYN] 7.2 over the block-mean grid, at one sample.
// [REF] transform.cpp bilinear_impl, and reconstruct.comp's bilinearMeans():
// one definition of the arithmetic, evaluated here for the tile form that has
// no coded DC plane to drive it.
int bilinearMeansW(int nb, int sx, int sy) {
    int qx = sx >> kBilinFracBits, fx = sx & (kBilinOne - 1);
    int qy = sy >> kBilinFracBits, fy = sy & (kBilinOne - 1);
    int x0 = clamp(qx, 0, nb - 1), x1 = clamp(qx + 1, 0, nb - 1);
    int r0 = clamp(qy, 0, nb - 1) * nb, r1 = clamp(qy + 1, 0, nb - 1) * nb;
    int m00 = sMeans[r0 + x0], m01 = sMeans[r0 + x1];
    int m10 = sMeans[r1 + x0], m11 = sMeans[r1 + x1];
    int wx0 = kBilinOne - fx, wy0 = kBilinOne - fy;
    return (m00 * wx0 * wy0 + m01 * fx * wy0 + m10 * wx0 * fy + m11 * fx * fy +
            kBilinRound) >> kBilinShift;
}


// ===================================================== the plane driver
// One coded plane of one inter tile, start to finish, left in the scratch
// buffer at CODED extent: `(size * size) / 2` uints, one pair of horizontally
// adjacent samples each, exactly the layout the WPred buffer and a Pass B
// plane slot both use.
//
// It is one function and not three because the box average has to know
// whether the near-skip field applies, the near-skip field has to know the
// plane's quantiser, and the quantiser has to know whether the plane is
// chroma: splitting it would put the same four facts in three signatures.
//
// **Why the box average is staged through registers.** At res_level > 0 the
// predictor is produced at FULL extent and averaged down to the coded one
// ([REF] codec_impl.inc predict_tile), and here the two live in the same
// scratch: output (y, x) is written at `y * size + x` while some other thread
// is still reading inputs at `(y' * factor) * full + x' * factor`, and those
// two index sets overlap -- with full 64, size 32, output (1, 0) writes 32 and
// output (0, 16) reads 32.  So every thread computes its outputs into
// registers, the workgroup meets at a barrier, and only then does anyone
// write.  A thread has at most TWO output pairs to hold: the average runs only
// when factor > 1, which forces size <= 32 and so `npair <= 512`, and 512
// pairs over 256 threads is two.  Hence four scalars and no array -- an array
// indexed by anything the compiler cannot fold lands in private memory on the
// Adreno 650 (docs/ADRENO-RULES.md rule 1).
//
// At factor == 1 the average is the identity and the samples are already in
// place, so nothing is read, written or barriered.
void nxvwWarpPlane(int tid, int p, uint tb, int size, int full, int sub,
                   int eye, int refEye, int wmode, int pw, int ph,
                   int near_skip, int quad, int tileQp, uint qbits,
                   uint refBase, int mvx, int mvy, int tx, int ty) {
    const bool chroma = (p == 1 || p == 2);
    const int factor = full / size;
    const bool ctChroma = (pc.p.colorTransform == kCtYCoCgR) && chroma;
    const int dcOff = ctChroma ? kDcOffsetChromaCT : kDcOffset8;
    const int maxval = ctChroma ? kMaxvalChromaCT : kMaxval8;
    const int npair = (size * size) >> 1;

    // ---- no reference: [REF] codec_impl.inc, "leave mid-grey, which is what
    // a decoder that has never held a reference can honestly show".
    if (refBase == 0xffffffffu) {
        barrier();
        for (int i = tid; i < npair; i += 256)
            nxvwWarpEmit(i, (uint(dcOff) & 0xffffu) | (uint(dcOff) << 16));
        barrier();
        return;
    }

    // ---- the reference image: one eye's sub-picture of this plane.  `pw` and
    // `ph` are the caller's: Pass W has them in its own push block, Pass B
    // derives them the way its reference-ring store already does, and neither
    // has to agree with the other about where they came from.
    refElemBase = int(refBase) +
                  int(nxvwWarpParam(uint(NXVW_WARP_HDR_RING + 4 + p))) +
                  refEye * pw;
    refStride = int(nxvwWarpParam(uint(NXVW_WARP_HDR_RING + 8 + p)));
    refW = pw;
    refH = ph;

#if defined(NXVW_ABL_STAGE) && NXVW_ABL_STAGE_COPY
    // The staging copy: one contiguous run of the ring per plane, clamped into
    // the ring so the probe can never read out of bounds.  Contiguous rather
    // than the real footprint's row-by-row gather, which makes this the
    // OPTIMISTIC copy cost.
    {
        const int ringWords = int(nxvwWarpParam(uint(NXVW_WARP_HDR_RING))) * 2;
        const int src0 = clamp(refElemBase >> 1, 0, ringWords - 1);
        barrier();
        for (int i = tid; i < int(kStageWords); i += 256)
            sSrc[i] = nxvwRingWord(uint(min(src0 + i, ringWords - 1)));
        barrier();
#if !NXVW_ABL_STAGE_TAP
        // Nothing reads sSrc in this configuration; keep it live.
        if (sSrc[uint(tid) & (kStageWords - 1u)] == 0xffffffffu) refW = refW;
#endif
    }
#endif

    // ---- the tile's origin in this plane's own eye-local samples.
    // [REF] plane_tile_origin(): tx * 32 for chroma of a 4:2:0 stream,
    // tx * 64 otherwise -- which is tx * full either way.
    const int tox = tx * full;
    const int toy = ty * full;

    // ---- step 1: four corners, four threads, one barrier.
    barrier();
    if (tid < 4)
        compute_corner(tid, uint((eye * 2 + (sub - 1)) * NXVW_WARP_MAT_UINTS),
                       tox, toy, wmode);
    barrier();
    const ivec2 c0 = sCorner[0], c1 = sCorner[1];
    const ivec2 c2 = sCorner[2], c3 = sCorner[3];

    // ---- the plane's vectors.  [SYN] 13.3 step 2: the same displacement in a
    // plane subsampled by `sub` is `mv / sub` quarter plane-samples, an
    // arithmetic shift.  A quadrant vector is a DELTA from the tile vector and
    // is halved by the same rule applied to the sum ([SYN] 13.10).
    //
    // Eight scalars and a select ladder rather than `int mvx_q6[4]`:
    // docs/ADRENO-RULES.md rule 1, and `q` comes from the sample's own
    // position so the compiler cannot fold the index.
    int mvxq0, mvxq1, mvxq2, mvxq3, mvyq0, mvyq1, mvyq2, mvyq3;
    {
        int vx = mvx, vy = mvy;
        if (sub == 2) { vx = vx >> 1; vy = vy >> 1; }
        mvxq0 = shl_i32_mod(vx, uint(kWarpQCorner - kWarpQMv));
        mvyq0 = shl_i32_mod(vy, uint(kWarpQCorner - kWarpQMv));
        mvxq1 = mvxq0; mvxq2 = mvxq0; mvxq3 = mvxq0;
        mvyq1 = mvyq0; mvyq2 = mvyq0; mvyq3 = mvyq0;
    }
    if (quad != 0) {
        // Four bytes, raster order TL TR BL BR, bits 3:0 the x delta and 7:4
        // the y delta, each a signed nibble in quarter samples.
        int dx0 = nxvw_sign_nibble(qbits), dy0 = nxvw_sign_nibble(qbits >> 4u);
        int dx1 = nxvw_sign_nibble(qbits >> 8u),
            dy1 = nxvw_sign_nibble(qbits >> 12u);
        int dx2 = nxvw_sign_nibble(qbits >> 16u),
            dy2 = nxvw_sign_nibble(qbits >> 20u);
        int dx3 = nxvw_sign_nibble(qbits >> 24u),
            dy3 = nxvw_sign_nibble(qbits >> 28u);
        int vx0 = mvx + dx0, vy0 = mvy + dy0;
        int vx1 = mvx + dx1, vy1 = mvy + dy1;
        int vx2 = mvx + dx2, vy2 = mvy + dy2;
        int vx3 = mvx + dx3, vy3 = mvy + dy3;
        if (sub == 2) {
            vx0 >>= 1; vy0 >>= 1; vx1 >>= 1; vy1 >>= 1;
            vx2 >>= 1; vy2 >>= 1; vx3 >>= 1; vy3 >>= 1;
        }
        const uint sh = uint(kWarpQCorner - kWarpQMv);
        mvxq0 = shl_i32_mod(vx0, sh); mvyq0 = shl_i32_mod(vy0, sh);
        mvxq1 = shl_i32_mod(vx1, sh); mvyq1 = shl_i32_mod(vy1, sh);
        mvxq2 = shl_i32_mod(vx2, sh); mvyq2 = shl_i32_mod(vy2, sh);
        mvxq3 = shl_i32_mod(vx3, sh); mvyq3 = shl_i32_mod(vy3, sh);
    }
    // The quadrant boundary is the PLANE's own half extent, so a 4:2:0 chroma
    // plane at 32 splits at 16 ([SYN] 13.10, ref/src/inter.cpp).
    const int qsplit = full / 2;

    // ---- steps 2-4: the predictor, at full extent, into the scratch.  Each
    // thread owns a horizontally adjacent PAIR so every word has one writer.
    // `full`, `size` and `factor * factor` are always powers of two -- 64 or 32
    // for the extent, 64/32/16/8 for the coded edge (nxvw_plane_size floors
    // chroma at 8), 1/4/16/64 for the box-average divisor -- so every division
    // in the per-sample loops below is a shift.  It was written as `/` and the
    // Adreno 650 has no integer divide: each one was a software sequence, run
    // once per sample PAIR, 3072 times a tile on a 4:2:0 frame.  The results
    // are bit-identical because both operands are non-negative: `e` is 2*i for
    // i >= 0, and `acc` is a sum of samples already clamped to [0, maxval].
    const int lfull = log2of(full);
    const int lsize = log2of(size);
    // A thread owns a CONTIGUOUS run of samples in ONE row rather than every
    // 256th pair, which is what lets the corner interpolation run as a DDA.
    // [SYN] 13.3's per-sample coordinate is bilinear over the four corners with
    // a rounding at the end of EACH of its two stages:
    //
    //     top = (c0 * (64 - u) + c1 * u + 32) >> 6
    //     bot = (c2 * (64 - u) + c3 * u + 32) >> 6
    //     out = (top * (64 - v) + bot * v + 32) >> 6
    //
    // A rounded intermediate is what an incremental evaluation usually founders
    // on, and here it does not: stage one's NUMERATOR is exactly
    // integer-linear in u -- c0 * 64 + 32 + (c1 - c0) * u -- so an accumulator
    // stepped by (c1 - c0) reproduces every numerator exactly and the >> 6 is
    // still applied per sample, to the same integer.  Stage two is NOT linear
    // in u, because it consumes the rounded top and bot; but v is constant
    // along a row, so its two multiplies stay while stage one's four go.  Four
    // multiplies a sample per axis become one add, and the samples are the
    // same ones.  Byte-identity on the conformance set is the proof.
    //
    // The run length divides the row exactly -- 16 samples at extent 64, 4 at
    // 32, 256 threads either way -- so every word still has exactly one writer
    // and no thread needs a bound test.
    const int tpr = 256 / full;      // threads per row: 4 at extent 64, 8 at 32
    const int spt = full / tpr;      // samples per thread: 16 at 64, 4 at 32
    const int ltpr = log2of(tpr);
    const int myRow = tid >> ltpr;
    const int myU0 = (tid & (tpr - 1)) * spt;
    const int qrow = (myRow >= qsplit) ? 2 : 0;
    // Stage two's weights: the basis is fitted over kWarpTile whatever this
    // plane's extent is ([SYN] 13.7's chroma caveat), so this is 64 - row and
    // row, not full - row.
    const int wv1 = kWarpTile - myRow, wv0 = myRow;
    // Stage one's numerators at u = myU0, and their per-u steps.
    const int dTopX = c1.x - c0.x, dBotX = c3.x - c2.x;
    const int dTopY = c1.y - c0.y, dBotY = c3.y - c2.y;
    int nTopX = c0.x * kWarpTile + (kWarpTile / 2) + dTopX * myU0;
    int nBotX = c2.x * kWarpTile + (kWarpTile / 2) + dBotX * myU0;
    int nTopY = c0.y * kWarpTile + (kWarpTile / 2) + dTopY * myU0;
    int nBotY = c2.y * kWarpTile + (kWarpTile / 2) + dBotY * myU0;
    // ---- the identity fast path's predicate.  [passb] Off unless
    // NXVW_PASSB_EXTRA_DEFS asks for NXVW_ABL_IDENTITY.
    //
    // A tile whose corners are the identity grid and whose every active vector
    // is a whole number of SAMPLES predicts each sample from exactly one
    // reference sample: the bilinear degenerates to a copy at an integer
    // offset.  That is a proof, and it needs all three parts.
    //
    // 1. The corner grid.  With c0 = (tox, toy) << 6 and the others one
    //    kWarpTile away in each axis, dTopX = dBotX = kWarpTile << 6 = 4096
    //    and c2.x == c0.x, so nTopX = 4096(tox + u) + 32, whose >> 6 is
    //    64(tox + u) because 32 < 64 -- and nBotX >> 6 is the same integer.
    //    Stage two's weights sum to kWarpTile, so
    //        bx = (64(tox+u)*wv1 + 64(tox+u)*wv0 + 32) >> 6 = 64(tox + u)
    //    exactly, for every u, whatever the row weights are.  Likewise by.
    //    The geometry contributes no fractional part.
    //
    // 2. The vector.  mqx is the plane's own vector in quarter samples shifted
    //    into Q.6, so mqx = 16*vx and
    //        xq4 = (64(tox+u) + 16vx + 2) >> 2 = 16(tox+u) + 4vx
    //        fx  = xq4 & 15 = (4vx) & 15
    //    which is zero exactly when vx is a multiple of four -- a whole
    //    sample.  Testing mq & 63 tests the same thing one step earlier and
    //    covers both axes and all four quadrants at once.  Chroma asks about
    //    the vector it already halved for sub == 2, not luma's.
    //
    // 3. No saturation.  sat_add_i32 and the clamp are not linear, so a copy
    //    cannot reproduce them.  The corners are the grid, so the extreme
    //    coordinates are the tile's own opposite corners plus the extreme
    //    vector; if neither end saturates or clamps, no interior sample can.
    //
    // Then sample_bilinear(ix, iy, 0, 0) has gx = gy = 16, acc = 256*t00, and
    // (256*t00 + 128) >> 8 == t00 for every t00 >= 0.  Every tap is a
    // reconstructed sample already clamped to [0, maxval], so t00 >= 0 always
    // and the fetch is the same integer the four-tap path produces.
    bool nxvwIdentity = false;
#ifdef NXVW_ABL_IDENTITY
    {
        const int gx0 = shl_i32_mod(tox, uint(kWarpQCorner));
        const int gy0 = shl_i32_mod(toy, uint(kWarpQCorner));
        const int gx1 = shl_i32_mod(tox + kWarpTile, uint(kWarpQCorner));
        const int gy1 = shl_i32_mod(toy + kWarpTile, uint(kWarpQCorner));
        nxvwIdentity = c0 == ivec2(gx0, gy0) && c1 == ivec2(gx1, gy0) &&
                       c2 == ivec2(gx0, gy1) && c3 == ivec2(gx1, gy1) &&
                       ((mvxq0 | mvyq0 | mvxq1 | mvyq1 |
                         mvxq2 | mvyq2 | mvxq3 | mvyq3) & 63) == 0;
        if (nxvwIdentity) {
            const int mnx = min(min(mvxq0, mvxq1), min(mvxq2, mvxq3));
            const int mxx = max(max(mvxq0, mvxq1), max(mvxq2, mvxq3));
            const int mny = min(min(mvyq0, mvyq1), min(mvyq2, mvyq3));
            const int mxy = max(max(mvyq0, mvyq1), max(mvyq2, mvyq3));
            const int ex0 = shl_i32_mod(tox, uint(kWarpQCorner));
            const int ex1 = shl_i32_mod(tox + full - 1, uint(kWarpQCorner));
            const int ey0 = shl_i32_mod(toy, uint(kWarpQCorner));
            const int ey1 = shl_i32_mod(toy + full - 1, uint(kWarpQCorner));
            nxvwIdentity =
                sat_add_i32(ex0, mnx) == ex0 + mnx &&
                sat_add_i32(ex1, mxx) == ex1 + mxx &&
                sat_add_i32(ey0, mny) == ey0 + mny &&
                sat_add_i32(ey1, mxy) == ey1 + mxy &&
                abs(ex0 + mnx) <= kWarpCoordClamp &&
                abs(ex1 + mxx) <= kWarpCoordClamp &&
                abs(ey0 + mny) <= kWarpCoordClamp &&
                abs(ey1 + mxy) <= kWarpCoordClamp;
        }
    }
#ifdef NXVW_ABL_IDENTITY_FORCE
    // VALIDATION ONLY, and it produces a WRONG picture on any tile whose warp
    // is not already the identity.  It exists to answer the question the
    // byte-identity test cannot answer by passing: does the fast path ever
    // FIRE on the fixtures?  If forcing it true leaves the conformance set
    // green, then every fixture tile was an identity warp and the honest
    // predicate was never gating anything -- the pass would be vacuous.  A
    // failure here is the result being looked for.
    nxvwIdentity = true;
#endif
#endif

    for (int j = 0; j < spt; j += 2) {
        const int u0 = myU0 + j;
        int s0 = 0, s1 = 0;
        for (int h = 0; h < 2; ++h) {
            const int u = u0 + h;
            // The geometric part is the WHOLE tile's corner basis evaluated at
            // (u, v); only the vector added to it is per quadrant, which is
            // what makes four equal quadrant vectors bit-identical to a
            // single-vector tile ([SYN] 13.10).
            // Per sample, and NOT hoisted, though it could be: a thread's run
            // starts at a multiple of its own length and `qsplit` is full / 2,
            // so `q` is in fact constant over the run.  Hoisting it -- strictly
            // less work -- costs 22 % on the Adreno 650.  Measured twice, three
            // interleaved rounds each; see ../passB/README.md.
            const int q = qrow + ((u >= qsplit) ? 1 : 0);
            const int mqx = (q == 0) ? mvxq0
                          : (q == 1) ? mvxq1
                          : (q == 2) ? mvxq2
                                     : mvxq3;
            const int mqy = (q == 0) ? mvyq0
                          : (q == 1) ? mvyq1
                          : (q == 2) ? mvyq2
                                     : mvyq3;
            const int bx = ((nTopX >> 6) * wv1 + (nBotX >> 6) * wv0 +
                            (kWarpTile / 2)) >> 6;
            const int by = ((nTopY >> 6) * wv1 + (nBotY >> 6) * wv0 +
                            (kWarpTile / 2)) >> 6;
            const int xq6 = clamp(sat_add_i32(bx, mqx), -kWarpCoordClamp,
                                  kWarpCoordClamp);
            const int yq6 = clamp(sat_add_i32(by, mqy), -kWarpCoordClamp,
                                  kWarpCoordClamp);
            // Q.6 -> Q.4, round half up (paper 2.2 step 4: "(c + 2) >> 2").
            const int xq4 = (xq6 + 2) >> (kWarpQCorner - kWarpQSample);
            const int yq4 = (yq6 + 2) >> (kWarpQCorner - kWarpQSample);
            // The predicate is a property of the TILE, so this branch is
            // uniform across the workgroup and over the whole loop; it is
            // written here rather than as a second loop so that every barrier
            // below, and the near-skip and box-average stages after it, are
            // the code they always were.
            int sv;
            if (nxvwIdentity) {
                // mqx is 16*vx with vx a multiple of four, so mqx >> 6 is
                // vx / 4 exactly -- the whole-sample displacement.
                sv = clamp(fetchRef(tox + u + (mqx >> kWarpQCorner),
                                    toy + myRow + (mqy >> kWarpQCorner)),
                           0, maxval);
            } else {
                sv = clamp(sample_bilinear(xq4 >> kWarpQSample,
                                           yq4 >> kWarpQSample,
                                           xq4 & 15, yq4 & 15),
                           0, maxval);
            }
            if (h == 0) s0 = sv; else s1 = sv;
            nTopX += dTopX; nBotX += dBotX;
            nTopY += dTopY; nBotY += dBotY;
        }
        nxvwWarpScratchWrite((myRow * full + u0) >> 1,
                             (uint(s0) & 0xffffu) | (uint(s1) << 16));
    }
    barrier();

    // ---- the near-skip mean field, if the row header named this tile.
    // [REF] codec.cpp reconstruct_near_skip(): the correction is dequantised
    // at the DC PLANE's step, because it is that DC plane written in nine
    // bytes.
    const int nb = size >> 3;
    if (near_skip != 0 && p < kNearSkipPlanes) {
        int planeQp = tileQp;
        if (chroma) planeQp = clamp(tileQp + pc.p.chromaQpOff, 0, 63);
        const uint rec = (p == 0) ? nxvwWarpParam(tb + 8u)
                       : (p == 1) ? nxvwWarpParam(tb + 9u)
                                  : nxvwWarpParam(tb + 10u);
        const int t = dequantStepW(nxvw_dc_qp(planeQp), kFlatWeight);
        const int d0 = dequantW(signByte(rec), t);
        const int dh = dequantW(signByte(rec >> 8u), t);
        const int dv = dequantW(signByte(rec >> 16u), t);
        const int lb = log2of(nb);
        if (tid < nb * nb) {
            // nb is size >> 3, so 8/4/2/1: a shift here too, and `lb` is
            // already the log2 the ramps need.
            const int by = tid >> lb, bx = tid & (nb - 1);
            sMeans[tid] = dcOff + d0 + ((dh * (2 * bx - nb + 1)) >> lb) +
                          ((dv * (2 * by - nb + 1)) >> lb);
        }
    }
    barrier();

    // ---- the box average down to the coded extent.
    // [REF] codec_impl.inc predict_tile(): a res_level > 0 tile predicts at
    // full extent and box-averages with the same kernel the encoder uses on
    // the source, so the residual is measured in one domain throughout.
    const bool ns = near_skip != 0 && p < kNearSkipPlanes;

    if (factor == 1) {
        // The identity: the samples are already at coded extent in the
        // scratch.  With no mean field there is nothing to compute, and the
        // only thing left is to hand them to the caller -- which for the
        // module whose scratch IS its destination is not a copy at all.
        if (!ns) {
#ifndef NXVW_WARP_EMIT_IS_SCRATCH
            for (int i = tid; i < npair; i += 256)
                nxvwWarpEmit(i, nxvwWarpScratchRead(i));
            barrier();
#endif
            return;
        }
        // With a mean field, output pair `i` is computed from the SAME word
        // `i` by the SAME thread, so there is no aliasing to stage around and
        // no barrier to take even when the destination is the scratch.
        for (int i = tid; i < npair; i += 256) {
            const int e = 2 * i;
            const int y = e >> lsize;
            const int x0 = e & (size - 1);
            const uint w = nxvwWarpScratchRead(i);
            int r0 = 0, r1 = 0;
            for (int h = 0; h < 2; ++h) {
                const int x = x0 + h;
                const int val = clamp(
                    int((w >> (uint(h) * 16u)) & 0xffffu) +
                        bilinearMeansW(nb, kPlanarMul * x + kPlanarOff,
                                       kPlanarMul * y + kPlanarOff) -
                        dcOff,
                    0, maxval);
                if (h == 0) r0 = val; else r1 = val;
            }
            nxvwWarpEmit(i, (uint(r0) & 0xffffu) | (uint(r1) << 16));
        }
        barrier();
        return;
    }

    // factor > 1, so size <= 32 and npair <= 512: two output pairs a thread,
    // held in registers until the whole workgroup has finished reading.
    const int n = factor * factor;
    const int ln = log2of(n);
    int o0a = 0, o0b = 0, o1a = 0, o1b = 0;
    const int i0 = tid, i1 = tid + 256;
    for (int slot = 0; slot < 2; ++slot) {
        const int i = (slot == 0) ? i0 : i1;
        if (i >= npair) continue;
        const int e = 2 * i;
        const int y = e >> lsize;
        const int x0 = e & (size - 1);
        int r0 = 0, r1 = 0;
        for (int h = 0; h < 2; ++h) {
            const int x = x0 + h;
            int acc = 0;
            for (int j = 0; j < factor; ++j)
                for (int k = 0; k < factor; ++k) {
                    const int fe = (y * factor + j) * full + x * factor + k;
                    acc += int((nxvwWarpScratchRead(fe >> 1) >>
                                ((uint(fe) & 1u) * 16u)) & 0xffffu);
                }
            int val = (acc + (n >> 1)) >> ln;
            if (ns) {
                // [SYN] 13.9: from `means` the tile is finished by exactly the
                // path 13.3 already defines -- the planar interpolation of
                // 7.2, then pred = clamp(W + planar(M) - dc_offset).  There is
                // no residual, so folding the combination in here is what lets
                // Pass B's hook stay one expression: an ordinary skipped
                // tile's mean field is flat at dc_offset, whose planar
                // interpolation is dc_offset exactly, so the same hook
                // reproduces clamp(W) for it.
                val = clamp(val +
                                bilinearMeansW(nb, kPlanarMul * x + kPlanarOff,
                                               kPlanarMul * y + kPlanarOff) -
                                dcOff,
                            0, maxval);
            }
            if (h == 0) r0 = val; else r1 = val;
        }
        if (slot == 0) { o0a = r0; o0b = r1; } else { o1a = r0; o1b = r1; }
    }
    // Nobody writes until everybody has read: the input and output index sets
    // overlap (full 64, size 32: output (1,0) writes 32, output (0,16) reads
    // 32), and the scratch is the destination as well as the source.
    barrier();
    if (i0 < npair)
        nxvwWarpEmit(i0, (uint(o0a) & 0xffffu) | (uint(o0b) << 16));
    if (i1 < npair)
        nxvwWarpEmit(i1, (uint(o1a) & 0xffffu) | (uint(o1b) << 16));
    barrier();
}

#endif  // NXVW_WARP_PRED_GLSL
