// vk.passB.ref_conformance
//
// Checks the Pass B CPU model against the normative CPU reference in ref/.
// Every constant and every rounding rule that the GPU kernel depends on is
// exercised here through ref's own code, so if ref/ (or docs/SYNTAX.md, once
// it lands) changes a constant, this test fails and points at exactly which
// line of vk/decoder/passB/syntax_constants.h is stale.
//
// Two levels:
//   1. primitives   -- idct8x8, the Q4 bilinear, the quantizer step table, the
//                      weighting matrices, the YCoCg-R inverse.
//   2. whole plane  -- an oracle assembled here out of ref's own primitives,
//                      following ref/src/codec.cpp reconstruct_plane() and
//                      codec_impl.inc store_tile(), compared against the model
//                      over randomly generated coefficient sets.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

// ref internals (normative)
#include "common.h"
#include "transform.h"
#include "nxvc/nxvc.h"

// Pass B
#include "passB_layout.h"
#include "passB_model.h"
#include "syntax_constants.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++g_fail;                                        \
        }                                                    \
    } while (0)

namespace {

int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ref's reconstruct_plane() is file-static, so the oracle re-states it here.
// It calls ref's own idct8x8 / bilinear_q4_i32 / kQStep / kWeight, which is
// what makes it a conformance oracle rather than a second copy of the model.
int ref_dequant_step(int qp, int w) {
    return (nxvc::kQStep[qp] * w + 8) >> 4;
}
int ref_dequant(int q, int t) { return nxvc::clamp16((q * t + 8) >> 4); }

void ref_reconstruct_plane(int size, int qp, const nxvc::u8 *wmat, int dc_off,
                           int maxval, int tskip, const int16_t *coefs,
                           std::vector<int> &samples) {
    const int nb = size / 8;
    const int ndc = nb * nb;
    samples.assign((size_t)size * size, 0);

    int dcqp = qp >> 1;  // [REF] codec.cpp dc_qp_of()
    int tdc = ref_dequant_step(dcqp, 16);
    std::vector<int> dc(ndc);
    for (int i = 0; i < ndc; ++i) dc[i] = ref_dequant(coefs[i], tdc);
    if (nb == 8) {
        nxvc::i32 in[64], out[64];
        for (int i = 0; i < 64; ++i) in[i] = dc[i];
        nxvc::idct8x8(in, out);
        for (int i = 0; i < 64; ++i) dc[i] = out[i];
    }
    std::vector<int> means(ndc);
    for (int i = 0; i < ndc; ++i)
        means[i] = iclamp(dc_off + dc[i], 0, maxval);

    std::vector<int> pred((size_t)size * size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            pred[(size_t)y * size + x] = nxvc::bilinear_q4_i32(
                means.data(), nb, nb, nb, 2 * x - 7, 2 * y - 7);

    const int16_t *bc = coefs + ndc;
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            const int16_t *c = bc + ((size_t)by * nb + bx) * 64;
            nxvc::i32 res[64];
            if (tskip) {
                int t = ref_dequant_step(qp, 16);
                for (int i = 0; i < 64; ++i) res[i] = ref_dequant(c[i], t);
            } else {
                nxvc::i32 dq[64];
                for (int i = 0; i < 64; ++i)
                    dq[i] = ref_dequant(c[i], ref_dequant_step(qp, wmat[i]));
                nxvc::idct8x8(dq, res);
            }
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    int y = by * 8 + j, x = bx * 8 + i;
                    samples[(size_t)y * size + x] = iclamp(
                        pred[(size_t)y * size + x] + res[j * 8 + i], 0, maxval);
                }
        }
}

// ref's upsampled(): half-phase Q4 bilinear from the coded plane to `full`.
int ref_upsampled(const std::vector<int> &s, int size, int full, int x, int y) {
    if (size == full) return s[(size_t)y * size + x];
    int factor = full / size;
    int mul = 16 / factor, off = mul / 2 - 8;
    return nxvc::bilinear_q4_i32(s.data(), size, size, size, mul * x + off,
                                 mul * y + off);
}

// ------------------------------------------------------------- level 1
void test_primitives() {
    // Quantizer step table and weighting matrices.
    for (int qp = 0; qp < 64; ++qp)
        CHECK(nxvw::kQStep[qp] == (int)nxvc::kQStep[qp],
              "kQStep[%d]: passB %d, ref %u", qp, nxvw::kQStep[qp],
              (unsigned)nxvc::kQStep[qp]);
    for (int m = 0; m < 4; ++m)
        for (int i = 0; i < 64; ++i)
            CHECK(nxvw::kWeightFlat[m * 64 + i] == (int)nxvc::kWeight[m][i],
                  "kWeight[%d][%d]: passB %d, ref %u", m, i,
                  nxvw::kWeightFlat[m * 64 + i], (unsigned)nxvc::kWeight[m][i]);

    // Matrix resolution: luma m, chroma 3 unless m == 0.
    for (int m = 0; m < 4; ++m) {
        int w[128];
        nxvw::model_resolve_matrices(m, nullptr, w);
        for (int i = 0; i < 64; ++i) {
            CHECK(w[i] == (int)nxvc::kWeight[m][i], "resolve luma m=%d i=%d", m, i);
            CHECK(w[64 + i] == (int)nxvc::kWeight[m == 0 ? 0 : 3][i],
                  "resolve chroma m=%d i=%d", m, i);
        }
    }

    // Dequant formula over the whole legal range.
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> dq(-32768, 32767);
    for (int qp = 0; qp < 64; ++qp)
        for (int w : {1, 16, 17, 31, 32}) {
            int t_ref = ref_dequant_step(qp, w);
            int t_mod = nxvw::model_dequant_step(qp, w);
            CHECK(t_ref == t_mod, "dequant_step(%d,%d): ref %d, model %d", qp, w,
                  t_ref, t_mod);
            for (int k = 0; k < 64; ++k) {
                int q = dq(rng);
                CHECK(ref_dequant(q, t_ref) == nxvw::model_dequant(q, t_mod),
                      "dequant(%d, %d)", q, t_ref);
            }
        }

    // 8x8 inverse transform, including saturating inputs.
    for (int trial = 0; trial < 4000; ++trial) {
        int mag = (trial % 4 == 0) ? 32767 : 900;
        std::uniform_int_distribution<int> dc(-mag, mag);
        nxvc::i32 in[64];
        int inm[64];
        for (int i = 0; i < 64; ++i) { in[i] = dc(rng); inm[i] = in[i]; }
        nxvc::i32 outr[64];
        int outm[64];
        nxvc::idct8x8(in, outr);
        nxvw::model_idct8x8(inm, outm);
        for (int i = 0; i < 64; ++i)
            CHECK(outr[i] == outm[i], "idct8x8 trial %d pos %d: ref %d, model %d",
                  trial, i, outr[i], outm[i]);
    }

    // Q4 bilinear, on a random plane, including out-of-range coordinates.
    {
        int plane[64];
        std::uniform_int_distribution<int> dv(0, 511);
        for (int i = 0; i < 64; ++i) plane[i] = dv(rng);
        std::uniform_int_distribution<int> dcoord(-40, 160);
        for (int k = 0; k < 20000; ++k) {
            int sx = dcoord(rng), sy = dcoord(rng);
            CHECK(nxvc::bilinear_q4_i32(plane, 8, 8, 8, sx, sy) ==
                      nxvw::model_bilinear_q4(plane, 8, 8, 8, sx, sy),
                  "bilinear(%d,%d)", sx, sy);
        }
    }

    // YCoCg-R inverse against ref's public entry point.
    {
        std::uniform_int_distribution<int> dy(0, 255), dc2(0, 511);
        for (int k = 0; k < 20000; ++k) {
            uint8_t Y = (uint8_t)dy(rng);
            uint16_t Co = (uint16_t)dc2(rng), Cg = (uint16_t)dc2(rng);
            uint8_t r, g, b;
            nxvc_ycocgr_inverse(&Y, &Co, &Cg, &r, &g, &b, 1);
            int y = Y, co = (int)Co - nxvw::kDcOffsetChromaCT;
            int cg = (int)Cg - nxvw::kDcOffsetChromaCT;
            int t = y - (cg >> 1);
            int G = cg + t, B = t - (co >> 1), R = B + co;
            CHECK(iclamp(R, 0, 255) == r && iclamp(G, 0, 255) == g &&
                      iclamp(B, 0, 255) == b,
                  "ycocgr_inverse(%d,%d,%d)", y, co, cg);
        }
    }
}

// ------------------------------------------------------- level 1b: gain
// The integer IDCT must be the orthonormal DCT-III to within its rounding.
// A wrong shift in the two-stage chain shows up as a power-of-two gain error,
// which is exactly what PAPER 1.4's "7 then 12" would produce -- the shift
// chain comes from docs/SYNTAX.md 6.3 (7 then 13, total shift 20, total gain
// 1), never from the paper.
void test_idct_gain_against_float() {
    // SYNTAX 6.3 states the property outright: a DC coefficient of 1024
    // reconstructs a flat 128.
    {
        int in[64] = {0}, out[64];
        in[0] = 1024;
        nxvw::model_idct8x8(in, out);
        for (int i = 0; i < 64; ++i)
            CHECK(out[i] == 128, "DC gain: coef 1024 -> sample %d at %d (want 128)",
                  out[i], i);
    }

    double cs[8][8];
    for (int x = 0; x < 8; ++x)
        for (int u = 0; u < 8; ++u)
            cs[x][u] = (u == 0 ? std::sqrt(0.125) : 0.5) *
                       std::cos((2.0 * x + 1.0) * u * M_PI / 16.0);

    // Magnitude kept low enough that the NORMATIVE clamp16 between the two
    // passes never fires: a full block of +-512 saturates the intermediate and
    // would make this comparison meaningless.  Saturation behaviour itself is
    // covered by the bit-exact comparison against ref/ above.
    std::mt19937 rng(4242);
    std::uniform_int_distribution<int> dc(-128, 128);
    double num = 0.0, den = 0.0;
    double worst = 0.0;
    for (int trial = 0; trial < 3000; ++trial) {
        int in[64], out[64];
        for (int i = 0; i < 64; ++i) in[i] = dc(rng);
        nxvw::model_idct8x8(in, out);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                double f = 0.0;
                for (int u = 0; u < 8; ++u)
                    for (int v = 0; v < 8; ++v)
                        f += in[u * 8 + v] * cs[y][u] * cs[x][v];
                double g = out[y * 8 + x];
                num += g * f;
                den += f * f;
                double e = std::fabs(g - f);
                if (e > worst) worst = e;
            }
    }
    double scale = num / den;
    CHECK(std::fabs(scale - 1.0) < 0.001,
          "IDCT gain vs float orthonormal DCT-III is %.6f, want 1.0 "
          "(a wrong shift shows up here as 0.5 or 2.0)", scale);
    // Rounding of the 9-bit constants plus two shifts; a handful of LSBs.
    CHECK(worst < 4.0, "IDCT worst absolute error vs float is %.3f", worst);
}

// ------------------------------------------------------------- level 2
// A whole 4:4:4 YCoCg-R tile, reconstructed by the oracle and by the model,
// compared as RGB.  This is the reference decoder's exact output path
// (reconstruct_plane + store_tile) for a 4:4:4 stream.
void test_tile_against_ref() {
    std::mt19937 rng(99);
    std::uniform_int_distribution<int> dCoef(-600, 600);
    std::uniform_int_distribution<int> dZero(0, 3);

    for (int res_level = 0; res_level <= 2; ++res_level)
        for (int tskip = 0; tskip <= 1; ++tskip)
            for (int qp : {0, 11, 24, 40, 63})
                for (int matrix : {0, 1, 2, 3}) {
                    int weights[128];
                    nxvw::model_resolve_matrices(matrix, nullptr, weights);
                    nxvc::u8 wl[64], wc[64];
                    for (int i = 0; i < 64; ++i) {
                        wl[i] = (nxvc::u8)weights[i];
                        wc[i] = (nxvc::u8)weights[64 + i];
                    }

                    const int size = 64 >> res_level;
                    const int nb = size / 8;
                    const int ndc = nb * nb;
                    const int per = ndc + ndc * 64;

                    int stride = nxvw::nxvw_coef_stride_i16(0, 0);
                    std::vector<int16_t> coef((size_t)stride, 0);
                    for (int p = 0; p < 3; ++p) {
                        int16_t *d = coef.data() + (size_t)p * per;
                        for (int i = 0; i < ndc; ++i) *d++ = (int16_t)dCoef(rng);
                        for (int b = 0; b < ndc; ++b)
                            for (int i = 0; i < 64; ++i)
                                *d++ = (int16_t)((i == 0 || dZero(rng) == 0)
                                                     ? dCoef(rng) : 0);
                    }

                    // ---- oracle
                    std::vector<int> s[3];
                    for (int p = 0; p < 3; ++p) {
                        bool chroma = (p == 1 || p == 2);
                        ref_reconstruct_plane(size, qp, chroma ? wc : wl,
                                              chroma ? 256 : 128,
                                              chroma ? 511 : 255, tskip,
                                              coef.data() + (size_t)p * per, s[p]);
                    }
                    std::vector<uint8_t> oracle(64 * 64 * 4);
                    for (int y = 0; y < 64; ++y)
                        for (int x = 0; x < 64; ++x) {
                            int Y = ref_upsampled(s[0], size, 64, x, y);
                            int Co = ref_upsampled(s[1], size, 64, x, y) - 256;
                            int Cg = ref_upsampled(s[2], size, 64, x, y) - 256;
                            int t = Y - (Cg >> 1);
                            int G = Cg + t, B = t - (Co >> 1), R = B + Co;
                            size_t o = ((size_t)y * 64 + x) * 4;
                            oracle[o + 0] = (uint8_t)iclamp(R, 0, 255);
                            oracle[o + 1] = (uint8_t)iclamp(G, 0, 255);
                            oracle[o + 2] = (uint8_t)iclamp(B, 0, 255);
                            oracle[o + 3] = 255;
                        }

                    // ---- model
                    nxvw::NxvwTileRec rec{};
                    rec.w0 = 0;
                    rec.w1 = (uint32_t)nxvw::kModeIntra |
                             ((uint32_t)res_level << 3) | (1u << 5) |
                             ((uint32_t)(tskip & 1) << 23);
                    rec.w2 = 1u << 8;
                    rec.w3 = 0xffffffffu;
                    nxvw::PassBInput in;
                    in.push.imageW = 64;
                    in.push.imageH = 64;
                    in.push.tilesX = 1;
                    in.push.baseQp = qp;
                    in.push.colorTransform = nxvw::kCtYCoCgR;
                    in.push.chroma420 = 0;
                    in.push.alphaPresent = 0;
                    in.push.coefStrideI16 = stride;
                    in.tilesX = 1;
                    in.tilesY = 1;
                    in.coef = coef.data();
                    in.recs = &rec;
                    in.weights = weights;
                    std::vector<uint8_t> model(64 * 64 * 4);
                    nxvw::passB_reconstruct_rgba8(in, model.data());

                    int bad = 0;
                    for (size_t i = 0; i < model.size(); ++i)
                        if (model[i] != oracle[i]) ++bad;
                    CHECK(bad == 0,
                          "tile mismatch: res=%d tskip=%d qp=%d matrix=%d -> %d "
                          "differing bytes",
                          res_level, tskip, qp, matrix, bad);
                }
}

}  // namespace

int main() {
    test_primitives();
    test_idct_gain_against_float();
    test_tile_against_ref();
    if (g_fail == 0)
        std::printf("vk.passB.ref_conformance: PASS\n");
    else
        std::printf("vk.passB.ref_conformance: FAIL (%d checks)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
