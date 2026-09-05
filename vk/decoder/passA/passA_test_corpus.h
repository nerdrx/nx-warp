// Test-only: builds a corpus of encoded tiles plus the coefficients they
// must decode back to.  Shared by the Vulkan harness and the ctest binaries
// so both exercise byte-identical input for a given seed.
#ifndef NXWARP_PASSA_TEST_CORPUS_H
#define NXWARP_PASSA_TEST_CORPUS_H

#include <cstdio>
#include <vector>

#include "passA_model.h"
#include "passA_test_gen.h"

namespace nxwarp_passA {
namespace test {

struct CorpusConfig {
    uint32_t num_tiles = 2048;
    uint64_t seed = 1;
    bool allow_chroma444 = false;
    bool allow_alpha = false;
    bool allow_res_level = false;
    // Tuned so a full-resolution 4:2:0 tile lands near 0.5 symbols/pixel.
    uint32_t cbf_prob = 1024;
    uint32_t density = 760;
    uint32_t mean_last = 916;
    // [entropy-lite] kEntropyRans or kEntropyLiteFixed: which tool the tile
    // payloads are encoded with.  The unit list, the coefficients and the RNG
    // stream are identical either way, so the two corpora hold exactly the
    // same tiles and are directly comparable.
    uint32_t entropy = kEntropyRans;
    // [entropy-lite] INTRA_DIR: add one mode unit per coded plane and give it
    // random intra modes.  Only the Lite encoder codes mode units.
    bool intra_dir = false;
    uint32_t mpm_prob = 700;
};

struct Corpus {
    std::vector<uint8_t> bits;
    std::vector<TileDesc> tiles;
    std::vector<int16_t> expect_coef;
    std::vector<uint32_t> expect_cbf;
    std::vector<uint32_t> table_flat;  // kNumTableSets*kNumCtx*kNumSym
    Tables tabs{};
    uint32_t coef_stride = 0;
    uint32_t cbf_words = kCbfWordsPerTile;
    uint32_t frame_nplanes = 3;
    // [v3] Pass A's `tools` push constant.  The corpus generator emits v1
    // syntax only, so it stays 0; the v2 intra tools are covered end to end by
    // the conformance vectors instead.
    uint32_t tools = 0;
    uint64_t total_symbols = 0;  // entropy operations across the corpus
    uint64_t total_pixels = 0;
    // [entropy-lite] Which tool encoded the payloads, and the intra modes the
    // decoder must reproduce, packed exactly as Pass A's binding 6.
    uint32_t entropy = kEntropyRans;
    std::vector<uint32_t> expect_modes;
};

inline bool build_corpus(const CorpusConfig &cfg, Corpus &out) {
    make_tables(out.tabs, cfg.seed ^ 0xa5a5a5a5u);
    out.table_flat.resize(size_t(kNumTableSets) * kNumCtx * kNumSym);
    for (int k = 0; k < kNumTableSets; ++k)
        for (int c = 0; c < kNumCtx; ++c)
            for (int s = 0; s < kNumSym; ++s)
                out.table_flat[size_t((k * kNumCtx + c) * kNumSym + s)] =
                    out.tabs.cum[k][c][s];

    out.frame_nplanes = cfg.allow_alpha ? 4 : 3;
    out.entropy = cfg.entropy;
    out.tools = cfg.intra_dir ? kToolFlagIntraDir : 0u;

    // First pass: shapes, coefficients and the required coefficient stride.
    std::vector<TileShape> shapes(cfg.num_tiles);
    std::vector<std::vector<UnitInfo>> units(cfg.num_tiles);
    std::vector<std::vector<int16_t>> coefs(cfg.num_tiles);
    std::vector<std::vector<uint8_t>> modes(cfg.num_tiles);
    std::vector<int> ncoefs(cfg.num_tiles);
    uint32_t stride = 0;
    uint32_t max_units = 0;

    Rng rng(cfg.seed);
    for (uint32_t t = 0; t < cfg.num_tiles; ++t) {
        TileShape &s = shapes[t];
        s.frame_nplanes = int(out.frame_nplanes);
        s.res_level = cfg.allow_res_level ? int(rng.below(3)) : 0;
        s.chroma444 = cfg.allow_chroma444 ? int(rng.below(2)) : 0;
        s.alpha_mode = cfg.allow_alpha ? kAlphaModeCoded : 0;
        s.tskip = int(rng.below(2));
        s.table_set = int(rng.below(kNumTableSets));
        s.tile_index = int(t & kThTileIndexMask);
        s.tools = int(out.tools);
        ncoefs[t] = build_units(s, units[t]);
        if (uint32_t(ncoefs[t]) > stride) stride = uint32_t(ncoefs[t]);
        if (units[t].size() > max_units) max_units = uint32_t(units[t].size());
        make_tile_coefs(s, units[t], ncoefs[t], rng, cfg.cbf_prob, cfg.density,
                        cfg.mean_last, coefs[t]);
        if (cfg.intra_dir)
            make_tile_modes(units[t], rng, cfg.mpm_prob, modes[t]);
        out.total_pixels += uint64_t(kTileSize) * kTileSize;
    }
    if (max_units > uint32_t(kMaxUnitsPerTile)) return false;
    out.coef_stride = stride;

    // Second pass: encode, and lay out the expected outputs.
    out.expect_coef.assign(size_t(cfg.num_tiles) * stride, 0);
    out.expect_cbf.assign(size_t(cfg.num_tiles) * out.cbf_words, 0);
    out.expect_modes.assign(size_t(cfg.num_tiles) * kModeWordsPerTile, 0);
    out.tiles.resize(cfg.num_tiles);
    out.bits.clear();
    out.bits.reserve(size_t(cfg.num_tiles) * 512);

    std::vector<uint8_t> tilebuf;
    for (uint32_t t = 0; t < cfg.num_tiles; ++t) {
        uint64_t ops = 0;
        const bool enc_ok =
            cfg.entropy == kEntropyLiteFixed
                ? encode_tile_lite(shapes[t], units[t], coefs[t].data(),
                                   cfg.intra_dir ? modes[t].data() : nullptr,
                                   tilebuf, &ops)
                : encode_tile(shapes[t], units[t], coefs[t].data(), out.tabs,
                              tilebuf, &ops);
        if (!enc_ok) return false;
        out.total_symbols += ops;
        while (out.bits.size() & 3u) out.bits.push_back(0);
        out.tiles[t].bits_offset = uint32_t(out.bits.size());
        out.tiles[t].bits_length = uint32_t(tilebuf.size());
        out.tiles[t].coef_offset = t * stride;
        out.tiles[t].cbf_offset = t * out.cbf_words;
        out.tiles[t].mode_offset = t * kModeWordsPerTile;
        out.tiles[t].unit_len_offset = t * kUnitLenWordsPerTile;
        out.bits.insert(out.bits.end(), tilebuf.begin(), tilebuf.end());

        int16_t *dst = out.expect_coef.data() + size_t(t) * stride;
        for (int i = 0; i < ncoefs[t]; ++i) dst[i] = coefs[t][size_t(i)];

        uint32_t *cbf = out.expect_cbf.data() + size_t(t) * out.cbf_words;
        for (uint32_t u = 0; u < units[t].size(); ++u) {
            const UnitInfo &ui = units[t][u];
            bool any = false;
            for (int i = 0; i < ui.ncoef; ++i)
                if (coefs[t][size_t(ui.coef_base + i)] != 0) { any = true; break; }
            if (any) cbf[u / 32u] |= 1u << (u & 31u);
        }

        // [entropy-lite] The intra modes, packed as binding 6 carries them.
        if (cfg.intra_dir) {
            uint32_t *mw = out.expect_modes.data() + size_t(t) * kModeWordsPerTile;
            for (const UnitInfo &ui : units[t]) {
                if (ui.kind != 1) continue;
                int p = ui.mode_base / int(kModesPerPlane);
                int n = ui.nbx * ui.nbx;
                for (int b = 0; b < n; ++b)
                    mw[nxs_mode_word(p, b)] |=
                        (uint32_t(modes[t][size_t(ui.mode_base + b)]) & kModeMask)
                        << nxs_mode_shift(b);
            }
        }
    }
    // Padding so the shader's uint-addressed loads never run past the buffer.
    for (int i = 0; i < 16; ++i) out.bits.push_back(0);
    return true;
}

// Fills `in` to point at `c`.  `mode` is kReadPtrBallot or kReadPtrLdsFallback.
// `sparse` selects the coefficient layout; the corpus' expected coefficients
// are the dense ones, so the default here is 0 rather than the shipping 1.
inline Inputs corpus_inputs(const Corpus &c, uint32_t mode,
                            uint32_t sparse = 0) {
    Inputs in;
    in.sparse = sparse;
    in.bits = c.bits.data();
    in.bits_size = c.bits.size();
    in.tiles = c.tiles.data();
    in.num_tiles = uint32_t(c.tiles.size());
    in.tables = c.table_flat.data();
    in.frame_nplanes = c.frame_nplanes;
    in.coef_stride = c.coef_stride;
    in.cbf_words = c.cbf_words;
    in.read_ptr_mode = mode;
    // [entropy-lite] The corpus knows which tool it encoded with, and which
    // frame-uniform tools its unit lists assume.
    in.tools = c.tools;
    in.entropy_mode = c.entropy;
    return in;
}

}  // namespace test
}  // namespace nxwarp_passA

#endif  // NXWARP_PASSA_TEST_CORPUS_H
