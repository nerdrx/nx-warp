// vk.passA.model_roundtrip - encode with the test rANS encoder, decode with
// the CPU model of rans_decode.comp, and require an exact match.  No Vulkan.
//
// This is the test that pins the syntax machine: if the shader and the model
// ever disagree with ref, this fails first and cheapest.
#include <cstdio>
#include <cstdlib>

#include "passA_test_corpus.h"

using namespace nxwarp_passA;
using namespace nxwarp_passA::test;

namespace {

int check(const char *name, const CorpusConfig &cfg) {
    Corpus c;
    if (!build_corpus(cfg, c)) {
        std::printf("[%s] FAIL: could not build corpus\n", name);
        return 1;
    }

    int failures = 0;
    for (uint32_t mode : {kReadPtrBallot, kReadPtrLdsFallback}) {
        std::vector<int16_t> coef(c.expect_coef.size(), int16_t(0x5555));
        std::vector<uint32_t> cbf(c.expect_cbf.size(), 0xdeadbeefu);
        std::vector<uint32_t> status(c.tiles.size(), 0xffffffffu);

        Inputs in = corpus_inputs(c, mode);
        std::vector<uint32_t> modes(size_t(c.tiles.size()) *
                                    kModeWordsPerTile, 0);
        Outputs out;
        out.coef = coef.data();
        out.cbf = cbf.data();
        out.status = status.data();
        out.modes = modes.data();
        decode(in, out);

        size_t bad_status = 0, bad_coef = 0, bad_cbf = 0;
        for (size_t i = 0; i < status.size(); ++i)
            if (status[i] != kStatusOk) ++bad_status;
        for (size_t i = 0; i < coef.size(); ++i)
            if (coef[i] != c.expect_coef[i]) ++bad_coef;
        for (size_t i = 0; i < cbf.size(); ++i)
            if (cbf[i] != c.expect_cbf[i]) ++bad_cbf;

        const char *m = mode == kReadPtrBallot ? "ballot" : "lds";
        std::printf("[%s/%s] tiles=%u status_bad=%zu coef_bad=%zu cbf_bad=%zu\n",
                    name, m, uint32_t(c.tiles.size()), bad_status, bad_coef,
                    bad_cbf);
        if (bad_status || bad_coef || bad_cbf) {
            ++failures;
            // Point at the first offending tile to make triage cheap.
            for (size_t i = 0; i < status.size(); ++i)
                if (status[i] != kStatusOk) {
                    std::printf("    first bad status: tile %zu code %u\n", i,
                                status[i]);
                    break;
                }
            for (size_t i = 0; i < coef.size(); ++i)
                if (coef[i] != c.expect_coef[i]) {
                    std::printf(
                        "    first bad coef: tile %zu index %zu got %d want %d\n",
                        i / c.coef_stride, i % c.coef_stride, int(coef[i]),
                        int(c.expect_coef[i]));
                    break;
                }
        }
    }
    return failures;
}

}  // namespace

int main() {
    int failures = 0;

    {   // The headline shape: 2048 tiles, 4:2:0, mixed res / tskip / table set.
        CorpusConfig cfg;
        cfg.num_tiles = 2048;
        cfg.seed = 12345;
        Corpus probe;
        build_corpus(cfg, probe);
        std::printf("corpus: %.3f symbols/pixel, %zu payload bytes\n",
                    double(probe.total_symbols) / double(probe.total_pixels),
                    probe.bits.size());
        failures += check("mixed420", cfg);
    }

    {   // Mixed res_level: 64x64, 32x32 and 16x16 coded tiles in one dispatch.
        CorpusConfig cfg;
        cfg.num_tiles = 512;
        cfg.seed = 24680;
        cfg.allow_res_level = true;
        failures += check("mixed_res", cfg);
    }

    {   // 4:4:4 with a coded alpha plane: the widest geometry.
        CorpusConfig cfg;
        cfg.num_tiles = 256;
        cfg.seed = 777;
        cfg.allow_chroma444 = true;
        cfg.allow_alpha = true;
        cfg.allow_res_level = true;
        failures += check("chroma444_alpha", cfg);
    }

    {   // Dense, escape-heavy content at a single resolution.
        CorpusConfig cfg;
        cfg.num_tiles = 256;
        cfg.seed = 99;
        cfg.allow_res_level = false;
        cfg.cbf_prob = 1024;
        cfg.density = 1024;
        cfg.mean_last = 1010;
        failures += check("dense", cfg);
    }

    {   // Sparse: many empty units, lanes finishing at very different rounds.
        CorpusConfig cfg;
        cfg.num_tiles = 256;
        cfg.seed = 424242;
        cfg.allow_res_level = true;
        cfg.cbf_prob = 60;
        cfg.density = 100;
        cfg.mean_last = 200;
        failures += check("sparse", cfg);
    }

    std::printf(failures ? "FAILED (%d)\n" : "PASSED (%d)\n", failures);
    return failures ? 1 : 0;
}
