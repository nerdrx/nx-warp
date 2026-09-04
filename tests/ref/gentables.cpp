// Regenerates ref/src/default_tables.inc from measured symbol statistics.
//
//   nxv-gentables > ref/src/default_tables.inc
//
// This is a development tool, not part of the shipped codec.  The built-in
// probability tables are frozen bitstream constants: changing them changes
// every bitstream, so regenerating requires new conformance vectors and a
// SYNTAX.md note.  Run it only when the coefficient syntax itself changes.
//
// Method.  The `table_set` field is a free 3-bit index, and the reference
// encoder picks the cheapest of the eight sets per tile, so the eight sets
// should be eight *clusters of tile statistics*, not eight QP bands.  This
// tool collects per-tile symbol histograms over a spread of synthetic material
// (gradient, textured, checker, noise, flat; 4:2:0 and 4:4:4; every res_level;
// with and without transform skip) across the whole QP range, then runs a
// weighted k-means whose distance is exactly the quantity that matters: the
// number of bits the tile would cost under that set.  Lloyd iterations
// therefore monotonically reduce the total coded size of the corpus.
#include "test_util.h"
#include "nxvc/nxvc.h"

#include "common.h"
#include "entropy.h"

#include <cmath>

using namespace nxvc;

extern "C" int nxvc_debug_tile_histograms(const nxvc_config *cfg,
                                          const nxvc_image *img,
                                          const uint8_t *res_map, uint32_t *out,
                                          uint32_t max_tiles);

static constexpr int kN = kNumCtx * kNumSym;  // 192
static constexpr int kSets = 8;

struct Tile {
    std::vector<uint32_t> h;  // kN
    double weight;
};

// Cost in bits of coding histogram `h` with the centroid probabilities `p`.
static double cost(const uint32_t *h, const double *p) {
    double bits = 0;
    for (int i = 0; i < kN; ++i)
        if (h[i]) bits -= (double)h[i] * std::log2(p[i]);
    return bits;
}

// Turn accumulated counts into per-context probabilities with a uniform floor.
static void normalize_centroid(double *p) {
    for (int c = 0; c < kNumCtx; ++c) {
        double *row = p + c * kNumSym;
        double tot = 0;
        for (int s = 0; s < kNumSym; ++s) tot += row[s];
        for (int s = 0; s < kNumSym; ++s) {
            double q = tot > 0 ? row[s] / tot : 1.0 / kNumSym;
            // 1.5% uniform floor: an unseen symbol still costs a bounded
            // number of bits on content the corpus did not contain.
            row[s] = 0.985 * q + 0.015 / kNumSym;
        }
    }
}

int main() {
    const int W = 256, H = 192;
    struct Mat { int kind; int c444; double w; };
    const Mat mats[] = {
        {0, 0, 1.0}, {1, 0, 1.5}, {2, 0, 0.8}, {3, 0, 0.3}, {4, 0, 0.5},
        {0, 1, 0.7}, {1, 1, 1.0}, {2, 1, 0.5}, {3, 1, 0.2},
    };
    const double variant_w[3] = {1.0, 0.2, 0.7};  // plain, transform skip, res

    nxvc_tile_layout tl;
    nxvc_tile_layout_get(W, H, &tl);
    std::vector<uint8_t> res_cycle(tl.tile_count);
    for (uint32_t i = 0; i < tl.tile_count; ++i) res_cycle[i] = (uint8_t)(i % 3);

    std::vector<Tile> corpus;
    std::vector<uint32_t> buf((size_t)tl.tile_count * kN);
    for (const Mat &m : mats) {
        TestImage im = make_image(W, H, m.c444 != 0, m.kind, 700 + m.kind * 13);
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
        img.stride[0] = im.w; img.stride[1] = im.cw;
        img.stride[2] = im.cw; img.stride[3] = im.w;
        for (int qp = 0; qp < 64; ++qp) {
            for (int variant = 0; variant < 3; ++variant) {
                nxvc_config cfg;
                nxvc_config_default(&cfg);
                cfg.width = W; cfg.height = H;
                cfg.chroma = m.c444 ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
                cfg.base_qp = (uint32_t)qp;
                cfg.quant_matrix = 1;
                if (variant == 1) cfg.transform_skip = 1;
                int n = nxvc_debug_tile_histograms(
                    &cfg, &img, variant == 2 ? res_cycle.data() : nullptr,
                    buf.data(), tl.tile_count);
                if (n <= 0) continue;
                for (int t = 0; t < n; ++t) {
                    Tile tile;
                    tile.h.assign(buf.begin() + (size_t)t * kN,
                                  buf.begin() + (size_t)(t + 1) * kN);
                    uint64_t sum = 0;
                    for (uint32_t v : tile.h) sum += v;
                    if (sum == 0) continue;
                    tile.weight = m.w * variant_w[variant];
                    corpus.push_back(std::move(tile));
                }
            }
        }
    }
    std::fprintf(stderr, "corpus: %zu tiles\n", corpus.size());

    // --- initialize the eight centroids from QP octaves, so the clustering
    //     starts from a spread that already covers the range.
    std::vector<std::vector<double>> cent(kSets, std::vector<double>(kN, 0.0));
    for (size_t i = 0; i < corpus.size(); ++i) {
        int k = (int)((i * kSets) / corpus.size());
        for (int j = 0; j < kN; ++j)
            cent[k][j] += corpus[i].weight * corpus[i].h[j];
    }
    for (int k = 0; k < kSets; ++k) normalize_centroid(cent[k].data());

    // --- Lloyd iterations
    std::vector<int> assign(corpus.size(), 0);
    double prev = 0;
    for (int iter = 0; iter < 40; ++iter) {
        double total = 0;
        for (size_t i = 0; i < corpus.size(); ++i) {
            double best = 0;
            int bk = 0;
            for (int k = 0; k < kSets; ++k) {
                double c = cost(corpus[i].h.data(), cent[k].data());
                if (k == 0 || c < best) { best = c; bk = k; }
            }
            assign[i] = bk;
            total += corpus[i].weight * best;
        }
        std::vector<std::vector<double>> acc(kSets, std::vector<double>(kN, 0.0));
        std::vector<double> mass(kSets, 0.0);
        for (size_t i = 0; i < corpus.size(); ++i) {
            int k = assign[i];
            mass[k] += corpus[i].weight;
            for (int j = 0; j < kN; ++j)
                acc[k][j] += corpus[i].weight * corpus[i].h[j];
        }
        for (int k = 0; k < kSets; ++k) {
            if (mass[k] == 0) continue;  // keep an empty cluster where it is
            normalize_centroid(acc[k].data());
            cent[k] = acc[k];
        }
        std::fprintf(stderr, "iter %2d: %.0f kbit\n", iter, total / 1000.0);
        if (iter && prev - total < prev * 1e-5) break;
        prev = total;
    }

    // --- emit
    std::printf("// GENERATED by tests/ref/gentables.cpp.  Do not edit by hand.\n");
    std::printf("// Built-in probability table sets: [8][12][16], 10-bit, rows\n");
    std::printf("// sum to 1024.  The eight sets are k-means clusters of tile\n");
    std::printf("// symbol statistics; the encoder picks the cheapest per tile.\n");
    std::printf("const u16 kDefaultFreq[8][kNumCtx][kNumSym] = {\n");
    for (int k = 0; k < kSets; ++k) {
        std::printf("  { // set %d\n", k);
        for (int c = 0; c < kNumCtx; ++c) {
            u16 f[kNumSym];
            for (int s = 0; s < kNumSym; ++s) {
                int v = (int)(cent[k][c * kNumSym + s] * 1024.0 + 0.5);
                f[s] = (u16)(v < 1 ? 1 : v);
            }
            normalize_freqs(f);
            std::printf("    {");
            for (int s = 0; s < kNumSym; ++s)
                std::printf("%s%4d", s ? ", " : "", f[s]);
            std::printf("},\n");
        }
        std::printf("  },\n");
    }
    std::printf("};\n");
    return 0;
}
