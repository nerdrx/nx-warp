// CPU model of reconstruct.comp.  Read the two side by side: every arithmetic
// line here has a one-to-one counterpart in the shader.
#include "passB_model.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "syntax_constants.h"

namespace nxvw {
namespace {

inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clamp16(int v) { return iclamp(v, kI16Min, kI16Max); }

// ------------------------------------------------------------- 1D IDCT
// Mirrors idct8_1d() in the shader, which mirrors ref/src/transform.cpp.
inline void idct8_1d(const int *x, int *y) {
    int t0 = (x[0] + x[4]) * kC4;
    int t1 = (x[0] - x[4]) * kC4;
    int t2 = x[2] * kS2 - x[6] * kC2;
    int t3 = x[2] * kC2 + x[6] * kS2;
    int e0 = t0 + t3, e3 = t0 - t3;
    int e1 = t1 + t2, e2 = t1 - t2;
    int A = x[1] * kA1 + x[7] * kA7;
    int B = x[1] * kA7 - x[7] * kA1;
    int C = x[3] * kA3 + x[5] * kA5;
    int D = x[3] * kA5 - x[5] * kA3;
    int O0 = A + C;
    int O3 = B - D;
    int P = A - C, Q = B + D;
    int O1 = ((P + Q) * kC4 + kOddRound) >> kOddShift;
    int O2 = ((P - Q) * kC4 + kOddRound) >> kOddShift;
    y[0] = e0 + O0; y[7] = e0 - O0;
    y[1] = e1 + O1; y[6] = e1 - O1;
    y[2] = e2 + O2; y[5] = e2 - O2;
    y[3] = e3 + O3; y[4] = e3 - O3;
}

// ------------------------------------------------------------- bilinear
inline int bilinear(const int *src, int w, int h, int stride, int sx, int sy) {
    int x0 = sx >> kBilinFracBits, y0 = sy >> kBilinFracBits;
    int fx = sx & (kBilinOne - 1), fy = sy & (kBilinOne - 1);
    int x1 = iclamp(x0 + 1, 0, w - 1);
    int y1 = iclamp(y0 + 1, 0, h - 1);
    x0 = iclamp(x0, 0, w - 1);
    y0 = iclamp(y0, 0, h - 1);
    int p00 = src[y0 * stride + x0], p01 = src[y0 * stride + x1];
    int p10 = src[y1 * stride + x0], p11 = src[y1 * stride + x1];
    int wx0 = kBilinOne - fx, wy0 = kBilinOne - fy;
    return (p00 * wx0 * wy0 + p01 * fx * wy0 + p10 * wx0 * fy + p11 * fx * fy +
            kBilinRound) >> kBilinShift;
}

// One tile's reconstructed planes at their coded resolution.
struct TilePlanes {
    int size[4] = {0, 0, 0, 0};
    int full[4] = {64, 64, 64, 64};
    std::array<std::vector<int>, 4> s;
};

inline int planeAtFull(const TilePlanes &tp, int p, int px, int py) {
    int size = tp.size[p], full = tp.full[p];
    if (size == full) return tp.s[p][(size_t)py * size + px];
    int mul = (16 * size) / full;
    int off = mul / 2 - 8;
    return bilinear(tp.s[p].data(), size, size, size, mul * px + off,
                    mul * py + off);
}

inline int planeAtDisplay(const TilePlanes &tp, int p, int x, int y) {
    int full = tp.full[p];
    if (full == 64) return planeAtFull(tp, p, x, y);
    int sx = 8 * x - 4, sy = 8 * y - 4;
    int x0 = sx >> kBilinFracBits, y0 = sy >> kBilinFracBits;
    int fx = sx & (kBilinOne - 1), fy = sy & (kBilinOne - 1);
    int x1 = iclamp(x0 + 1, 0, full - 1);
    int y1 = iclamp(y0 + 1, 0, full - 1);
    x0 = iclamp(x0, 0, full - 1);
    y0 = iclamp(y0, 0, full - 1);
    int p00 = planeAtFull(tp, p, x0, y0);
    int p01 = planeAtFull(tp, p, x1, y0);
    int p10 = planeAtFull(tp, p, x0, y1);
    int p11 = planeAtFull(tp, p, x1, y1);
    int wx0 = kBilinOne - fx, wy0 = kBilinOne - fy;
    return (p00 * wx0 * wy0 + p01 * fx * wy0 + p10 * wx0 * fy + p11 * fx * fy +
            kBilinRound) >> kBilinShift;
}

// Reconstruct one tile's planes.  Same order of operations as the kernel.
void reconstruct_tile(const PassBInput &in, int tile, TilePlanes &tp,
                      int &alpha_mode, int &alpha_value) {
    const NxvwPassBPush &pcp = in.push;
    const NxvwTileRec &rec = in.recs[tile];
    int res_level = nxvw_rec_res_level(rec.w1);
    int chroma444 = pcp.chroma420 != 0 ? 0 : nxvw_rec_chroma444(rec.w1);
    alpha_mode = nxvw_rec_alpha_mode(rec.w1);
    alpha_value = nxvw_rec_alpha_value(rec.w2);
    int tskip = nxvw_rec_tskip(rec.w1);
    int mode = nxvw_rec_mode(rec.w1);
    int qp = iclamp(pcp.baseQp + nxvw_rec_qp_delta(rec.w1), 0, 63);
    bool intra = (mode == kModeIntra);  // INTER HOOK, see reconstruct.comp

    int nplanes = pcp.alphaPresent != 0 ? 4 : 3;
    int ncoded = nplanes;
    if (alpha_mode != kAlphaCoded) ncoded = std::min(ncoded, 3);

    for (int p = 0; p < 4; ++p) {
        tp.size[p] = nxvw_plane_size(p, res_level, chroma444);
        tp.full[p] = ((p == 1 || p == 2) && pcp.chroma420 != 0) ? 32 : 64;
        tp.s[p].assign((size_t)tp.size[p] * tp.size[p], 0);
    }

    const int16_t *coefBase = in.coef + (size_t)tile * pcp.coefStrideI16;

    for (int p = 0; p < ncoded; ++p) {
        bool chroma = (p == 1 || p == 2);
        int size = tp.size[p];
        int nb = size >> 3;
        int ndc = nb * nb;
        int planeQp = qp;
        if (chroma) planeQp = iclamp(qp + pcp.chromaQpOff, 0, 63);
        else if (p == 3) planeQp = iclamp(qp + pcp.alphaQpOff, 0, 63);
        const int *wmat = in.weights + (chroma ? 64 : 0);
        bool ctChroma = (pcp.colorTransform == kCtYCoCgR) && chroma;
        int dcOff = ctChroma ? kDcOffsetChromaCT : kDcOffset8;
        int maxval = ctChroma ? kMaxvalChromaCT : kMaxval8;

        // --- DC plane (PAPER 3.2.4)
        int dcqp = nxvw_dc_qp(planeQp);  // [marked edit] qp >> 1, was qp - 6
        int tdc = model_dequant_step(dcqp, kFlatWeight);
        std::vector<int> dc(ndc);
        for (int i = 0; i < ndc; ++i) dc[i] = model_dequant(coefBase[i], tdc);
        if (nb == 8) {
            int out[64];
            model_idct8x8(dc.data(), out);
            for (int i = 0; i < 64; ++i) dc[i] = out[i];
        }
        std::vector<int> means(ndc);
        for (int i = 0; i < ndc; ++i)
            means[i] = iclamp(dcOff + dc[i], 0, maxval);

        // --- residual blocks
        const int16_t *bc = coefBase + ndc;
        for (int b = 0; b < ndc; ++b) {
            int bx = b % nb, by = b / nb;
            const int16_t *c = bc + (size_t)b * 64;
            int res[64];
            if (tskip) {
                int t = model_dequant_step(planeQp, kFlatWeight);
                for (int i = 0; i < 64; ++i) res[i] = model_dequant(c[i], t);
            } else {
                int dq[64];
                for (int i = 0; i < 64; ++i)
                    dq[i] = model_dequant(c[i], model_dequant_step(planeQp, wmat[i]));
                model_idct8x8(dq, res);
            }
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    int x = bx * 8 + i, y = by * 8 + j;
                    int pred = intra ? bilinear(means.data(), nb, nb, nb,
                                                kPlanarMul * x + kPlanarOff,
                                                kPlanarMul * y + kPlanarOff)
                                     : 0;
                    tp.s[p][(size_t)y * size + x] =
                        iclamp(pred + res[j * 8 + i], 0, maxval);
                }
        }
        coefBase += ndc + ndc * 64;
    }
}

struct Rgba {
    int r, g, b, a;
};

inline Rgba tile_pixel(const PassBInput &in, const TilePlanes &tp, int alpha_mode,
                       int alpha_value, int x, int y) {
    int P0 = planeAtDisplay(tp, 0, x, y);
    int P1 = planeAtDisplay(tp, 1, x, y);
    int P2 = planeAtDisplay(tp, 2, x, y);
    int R, G, B;
    if (in.push.colorTransform == kCtYCoCgR) {
        int Y = P0;
        int Co = P1 - kDcOffsetChromaCT;
        int Cg = P2 - kDcOffsetChromaCT;
        int t = Y - (Cg >> 1);
        G = Cg + t;
        B = t - (Co >> 1);
        R = B + Co;
    } else {
        R = P0; G = P1; B = P2;
    }
    Rgba o;
    o.r = iclamp(R, 0, 255);
    o.g = iclamp(G, 0, 255);
    o.b = iclamp(B, 0, 255);
    o.a = 255;
    if (alpha_mode == kAlphaConstant) o.a = alpha_value;
    else if (alpha_mode == kAlphaCoded)
        o.a = iclamp(planeAtDisplay(tp, 3, x, y), 0, 255);
    return o;
}

template <typename Store>
void run(const PassBInput &in, Store store) {
    TilePlanes tp;
    for (int ty = 0; ty < in.tilesY; ++ty)
        for (int tx = 0; tx < in.tilesX; ++tx) {
            int tile = ty * in.push.tilesX + tx;
            int alpha_mode = 0, alpha_value = 255;
            reconstruct_tile(in, tile, tp, alpha_mode, alpha_value);
            int ox = tx * 64, oy = ty * 64;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    int gx = ox + x, gy = oy + y;
                    if (gx >= in.push.imageW || gy >= in.push.imageH) continue;
                    store(gx, gy, tile_pixel(in, tp, alpha_mode, alpha_value, x, y));
                }
        }
}

}  // namespace

// ------------------------------------------------------------- exported
int model_dequant_step(int qp, int w) {
    return (kQStep[qp] * w + kQStepRound) >> kQStepShift;
}
int model_dequant(int q, int t) {
    return clamp16((q * t + kDequantRound) >> kDequantShift);
}
int model_bilinear_q4(const int *src, int w, int h, int stride, int sx, int sy) {
    return bilinear(src, w, h, stride, sx, sy);
}

void model_idct8x8(const int src[64], int dst[64]) {
    int tmp[64];
    int in[8], out[8];
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = src[r * 8 + c];
        idct8_1d(in, out);
        for (int c = 0; c < 8; ++c)
            tmp[c * 8 + r] = clamp16((out[c] + kIdctRound1) >> kIdctShift1);
    }
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = tmp[r * 8 + c];
        idct8_1d(in, out);
        for (int c = 0; c < 8; ++c)
            dst[c * 8 + r] = clamp16((out[c] + kIdctRound2) >> kIdctShift2);
    }
}

void model_resolve_matrices(int quant_matrix, const uint8_t *custom128,
                            int *out128) {
    if (quant_matrix == 255 && custom128) {
        for (int i = 0; i < 64; ++i) {
            out128[i] = iclamp(custom128[i], 1, 32);
            out128[64 + i] = iclamp(custom128[64 + i], 1, 32);
        }
    } else {
        int m = iclamp(quant_matrix, 0, 3);
        int cm = (m == 0) ? 0 : 3;
        for (int i = 0; i < 64; ++i) {
            out128[i] = kWeightFlat[m * 64 + i];
            out128[64 + i] = kWeightFlat[cm * 64 + i];
        }
    }
}

void passB_reconstruct_rgba8(const PassBInput &in, uint8_t *out) {
    const int W = in.push.imageW;
    run(in, [&](int x, int y, Rgba p) {
        uint8_t *d = out + ((size_t)y * W + x) * 4;
        d[0] = (uint8_t)p.r;
        d[1] = (uint8_t)p.g;
        d[2] = (uint8_t)p.b;
        d[3] = (uint8_t)p.a;
    });
}

void passB_reconstruct_ycbcr420(const PassBInput &in, uint8_t *luma,
                                uint8_t *cbcr) {
    const int W = in.push.imageW, H = in.push.imageH;
    const int cw = (W + 1) >> 1, ch = (H + 1) >> 1;
    TilePlanes tp;
    for (int ty = 0; ty < in.tilesY; ++ty)
        for (int tx = 0; tx < in.tilesX; ++tx) {
            int tile = ty * in.push.tilesX + tx;
            int alpha_mode = 0, alpha_value = 255;
            reconstruct_tile(in, tile, tp, alpha_mode, alpha_value);
            int ox = tx * 64, oy = ty * 64;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    int gx = ox + x, gy = oy + y;
                    if (gx >= W || gy >= H) continue;
                    luma[(size_t)gy * W + gx] =
                        (uint8_t)iclamp(planeAtFull(tp, 0, x, y), 0, 255);
                }
            int cox = tx * 32, coy = ty * 32;
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x) {
                    int gx = cox + x, gy = coy + y;
                    if (gx >= cw || gy >= ch) continue;
                    cbcr[((size_t)gy * cw + gx) * 2 + 0] =
                        (uint8_t)iclamp(planeAtFull(tp, 1, x, y), 0, 255);
                    cbcr[((size_t)gy * cw + gx) * 2 + 1] =
                        (uint8_t)iclamp(planeAtFull(tp, 2, x, y), 0, 255);
                }
        }
}

void passB_reconstruct_rgb10a2(const PassBInput &in, uint32_t *out) {
    const int W = in.push.imageW;
    run(in, [&](int x, int y, Rgba p) {
        uint32_t r = (uint32_t)((p.r << 2) | (p.r >> 6));
        uint32_t g = (uint32_t)((p.g << 2) | (p.g >> 6));
        uint32_t b = (uint32_t)((p.b << 2) | (p.b >> 6));
        uint32_t a = (uint32_t)(p.a >> 6);
        out[(size_t)y * W + x] = r | (g << 10) | (b << 20) | (a << 30);
    });
}

}  // namespace nxvw
