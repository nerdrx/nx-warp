// CPU model of Pass W.  See inter_model.h for what it does and does not model.
//
// SPDX-License-Identifier: Apache-2.0
#include "inter_model.h"

#include <algorithm>
#include <vector>

#include "nxvc/warp.h"
#include "passB/passB_layout.h"
#include "passB/syntax_constants.h"

namespace nxvw {
namespace {

namespace nw = ::nxvc::warp;

inline int iclamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int sign_byte(uint32_t v) {
    int b = (int)(v & 255u);
    return b >= 128 ? b - 256 : b;
}

inline int log2_of(int n) {
    int k = 0;
    while ((1 << k) < n) ++k;
    return k;
}

// [REF] reconstruct.comp dequantStep / dequant, and codec.cpp's originals.
inline int dequant_step(int qp, int w) {
    return (kQStep[qp] * w + kQStepRound) >> kQStepShift;
}
inline int dequant(int q, int t) {
    return iclamp((q * t + kDequantRound) >> kDequantShift, kI16Min, kI16Max);
}

// [REF] transform.cpp bilinear_impl / reconstruct.comp bilinearMeans().
int bilinear_means(const int *m, int nb, int sx, int sy) {
    const int qx = sx >> kBilinFracBits, fx = sx & (kBilinOne - 1);
    const int qy = sy >> kBilinFracBits, fy = sy & (kBilinOne - 1);
    const int x0 = iclamp(qx, 0, nb - 1), x1 = iclamp(qx + 1, 0, nb - 1);
    const int r0 = iclamp(qy, 0, nb - 1) * nb, r1 = iclamp(qy + 1, 0, nb - 1) * nb;
    const int m00 = m[r0 + x0], m01 = m[r0 + x1];
    const int m10 = m[r1 + x0], m11 = m[r1 + x1];
    const int wx0 = kBilinOne - fx, wy0 = kBilinOne - fy;
    return (m00 * wx0 * wy0 + m01 * fx * wy0 + m10 * wx0 * fy + m11 * fx * fy +
            kBilinRound) >> kBilinShift;
}

}  // namespace

int inter_model_ring_at(const uint32_t *ring, int base, int stride, int w,
                        int h, int x, int y) {
    x = iclamp(x, 0, w - 1);
    y = iclamp(y, 0, h - 1);
    const uint32_t e = (uint32_t)(base + y * stride + x);
    return (int)((ring[e >> 1] >> ((e & 1u) * 16u)) & 0xffffu);
}

void inter_model_predict(const InterModelInput &in, int16_t *wpred) {
    const NxvwWarpPush &pc = in.push;
    const uint32_t *W = in.params;
    const uint32_t *hdr = W + NXVW_WARP_HDR_RING;

    // The library's predictor writes a fixed 64x64 block; the tile's own
    // extent is a prefix of it ([SYN] 13.7).
    std::vector<uint16_t> block((size_t)nw::kTile * nw::kTile, 0);
    std::vector<int> full_buf;
    std::vector<int> means(64, 0);

    for (int tile = 0; tile < pc.tileCount; ++tile) {
        const uint32_t *tb =
            W + NXVW_WARP_HDR_UINTS + (size_t)tile * NXVW_WARP_TILE_UINTS;
        const uint32_t w0 = tb[0];
        if (nxvw_wt_inter(w0) == 0) continue;
        const int eye = nxvw_wt_eye(w0);
        if (pc.eyeFilter >= 0 && eye != pc.eyeFilter) continue;

        const int mode = nxvw_wt_mode(w0);
        const int res_level = nxvw_wt_res_level(w0);
        const int chroma444 = nxvw_wt_chroma444(w0);
        const int alpha_mode = nxvw_wt_alpha_mode(w0);
        const int quad = nxvw_wt_quad(w0);
        const int near_skip = nxvw_wt_near_skip(w0);
        const int tx = (int)tb[1], ty = (int)tb[2];
        const int mvx = (int)tb[3], mvy = (int)tb[4];
        const uint32_t qbits = tb[5];
        const uint32_t refBase = tb[6];
        const int tileQp = (int)tb[7];
        const bool noref = refBase == 0xffffffffu;

        const nw::Mode wmode =
            (mode == kModeWarpSkip || mode == kModeWarpMv) ? nw::kModeWarp
                                                           : nw::kModeStatic;
        const int refEye = (mode == kModeStereo) ? 0 : eye;

        const int nplanes = pc.alphaPresent != 0 ? 4 : 3;
        int ncoded = nplanes;
        if (alpha_mode != kAlphaCoded) ncoded = std::min(ncoded, 3);

        for (int p = 0; p < ncoded; ++p) {
            const bool chroma = (p == 1 || p == 2);
            const int full = nxvw_inter_plane_full(p, pc.chroma420);
            const int sub = nxvw_inter_plane_sub(p, pc.chroma420);
            const int size = nxvw_plane_size(p, res_level, chroma444);
            const int factor = full / size;
            const bool ctChroma = (pc.colorTransform == kCtYCoCgR) && chroma;
            const int dcOff = ctChroma ? kDcOffsetChromaCT : kDcOffset8;
            const int maxval = ctChroma ? kMaxvalChromaCT : kMaxval8;
            const int pw = chroma ? pc.chromaW : pc.eyeW;
            const int ph = chroma ? pc.chromaH : pc.eyeH;
            int16_t *out = wpred + (size_t)tile * pc.wpredStrideI16 +
                           nxvw_wpred_plane_off(p, pc.chroma420);

            if (noref) {
                for (int i = 0; i < size * size; ++i) out[i] = (int16_t)dcOff;
                continue;
            }

            // The reference image: one eye's sub-picture of this plane of the
            // slot the tile names.  [REF] codec_impl.inc ref_image().
            const int planeOff = (int)hdr[4 + p];
            const int stride = (int)hdr[8 + p];
            const int base = (int)refBase + planeOff + refEye * pw;
            std::vector<uint16_t> img((size_t)stride * ph);
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x)
                    img[(size_t)y * stride + x] = (uint16_t)inter_model_ring_at(
                        in.ring, base, stride, pw, ph, x, y);
            nw::RefImage im{};
            im.data = img.data();
            im.width = pw;
            im.height = ph;
            im.stride = stride;
            im.channels = 1;
            im.max_value = maxval;

            nw::Homography H{};
            const uint32_t *m =
                W + (size_t)(eye * 2 + (sub - 1)) * NXVW_WARP_MAT_UINTS;
            for (int i = 0; i < 9; ++i) H.h[i] = (int32_t)m[i];
            H.ox = (int32_t)m[9];
            H.oy = (int32_t)m[10];

            // [SYN] 13.3 step 2 and 13.10: the plane's vectors, halved by the
            // subsampling factor, applied to the SUM of the tile vector and
            // the quadrant delta.
            int32_t mv4[4][2];
            for (int q = 0; q < 4; ++q) {
                int dx = 0, dy = 0;
                if (quad) {
                    const uint32_t qb = (qbits >> (q * 8)) & 255u;
                    dx = nxvw_sign_nibble(qb);
                    dy = nxvw_sign_nibble(qb >> 4);
                }
                int vx = mvx + dx, vy = mvy + dy;
                if (sub == 2) { vx >>= 1; vy >>= 1; }
                mv4[q][0] = vx;
                mv4[q][1] = vy;
            }

            // [REF] ref/src/inter.cpp warp_plane_tile_quad(): the library
            // emits a fixed 64x64 block at the tile's origin and the plane's
            // extent is its top-left corner.  The quadrant split is the
            // PLANE's own half extent.
            nw::warp_tile_quad(im, tx * full, ty * full, H, mv4, full / 2,
                               nw::kFilterBilinear, wmode, block.data(),
                               nw::kTile);
            full_buf.assign((size_t)full * full, 0);
            for (int y = 0; y < full; ++y)
                for (int x = 0; x < full; ++x)
                    full_buf[(size_t)y * full + x] =
                        (int)block[(size_t)y * nw::kTile + x];

            // [SYN] 13.9: the near-skip mean field, dequantised at the DC
            // plane's own step.
            const int nb = size >> 3;
            if (near_skip && p < kNearSkipPlanes) {
                int planeQp = tileQp;
                if (chroma) planeQp = iclamp(tileQp + pc.chromaQpOff, 0, 63);
                const uint32_t rec = tb[8 + p];
                const int t = dequant_step(nxvw_dc_qp(planeQp), kFlatWeight);
                const int d0 = dequant(sign_byte(rec), t);
                const int dh = dequant(sign_byte(rec >> 8), t);
                const int dv = dequant(sign_byte(rec >> 16), t);
                const int lb = log2_of(nb);
                means.assign((size_t)nb * nb, 0);
                for (int by = 0; by < nb; ++by)
                    for (int bx = 0; bx < nb; ++bx)
                        means[(size_t)by * nb + bx] =
                            dcOff + d0 + ((dh * (2 * bx - nb + 1)) >> lb) +
                            ((dv * (2 * by - nb + 1)) >> lb);
            }

            // [REF] codec_impl.inc predict_tile(): box-average to the coded
            // extent with the same kernel the encoder uses on the source.
            const int n = factor * factor;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    int acc = 0;
                    for (int j = 0; j < factor; ++j)
                        for (int k = 0; k < factor; ++k)
                            acc += full_buf[(size_t)(y * factor + j) * full +
                                            x * factor + k];
                    int val = (acc + n / 2) / n;
                    if (near_skip && p < kNearSkipPlanes)
                        val = iclamp(val +
                                         bilinear_means(means.data(), nb,
                                                        kPlanarMul * x +
                                                            kPlanarOff,
                                                        kPlanarMul * y +
                                                            kPlanarOff) -
                                         dcOff,
                                     0, maxval);
                    out[(size_t)y * size + x] = (int16_t)val;
                }
        }
    }
}

}  // namespace nxvw
