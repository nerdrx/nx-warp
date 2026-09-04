// vk.passA.ref_agreement - conformance against the normative CPU reference.
//
// This test is built only when ref/ is present.  It checks three things, in
// increasing strength:
//
//   1. Every syntax constant duplicated in syntax_constants.h equals the
//      value in ref/src (scan orders, LAST classes, level contexts, tile
//      geometry, escape parameters).
//   2. The test rANS encoder produces a payload BYTE-IDENTICAL to
//      nxvc::encode_units() for the same units and tables.  If this holds,
//      the round-trip tests are testing the real bitstream, not a private
//      dialect of it.
//   3. The Pass A CPU model decodes ref's own payload back to the original
//      coefficients.
//
// If tests/vectors/*.nxv exist they are reported; decoding them requires the
// container parser, which lives in ref's decoder rather than here.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// ref internals (ref/src is added to the include path by CMake).
#include "common.h"
#include "entropy.h"

#include "passA_test_corpus.h"

using namespace nxwarp_passA;
using namespace nxwarp_passA::test;

namespace {

int g_fail = 0;

void expect(bool ok, const char *what) {
    if (!ok) {
        std::printf("  MISMATCH: %s\n", what);
        ++g_fail;
    }
}

// --- 1. constants ----------------------------------------------------------
void check_constants() {
    std::printf("[constants]\n");
    expect(kRansL == nxvc::kRansL, "kRansL");
    expect(kProbBits == nxvc::kProbBits, "kProbBits");
    expect(kNumCtx == nxvc::kNumCtx, "kNumCtx");
    expect(kNumSym == nxvc::kNumSym, "kNumSym");
    expect(kCtxCbfLuma == nxvc::kCtxCbfLuma, "kCtxCbfLuma");
    expect(kCtxCbfChroma == nxvc::kCtxCbfChroma, "kCtxCbfChroma");
    expect(kCtxLastLuma == nxvc::kCtxLastLuma, "kCtxLastLuma");
    expect(kCtxLastChroma == nxvc::kCtxLastChroma, "kCtxLastChroma");
    expect(kCtxLevelBase == nxvc::kCtxLevelBase, "kCtxLevelBase");
    expect(kEscSym == nxvc::kEscSym, "kEscSym");
    expect(kEscOrder == nxvc::kEscOrder, "kEscOrder");
    expect(kEscMaxPrefix == nxvc::kEscMaxPrefix, "kEscMaxPrefix");
    expect(kTileHeaderBytes == nxvc::kTileHeaderBytes, "kTileHeaderBytes");
    expect(kTileSize == nxvc::kTile, "kTileSize");
    expect(kBlockSize == nxvc::kBlock, "kBlockSize");

    for (int i = 0; i < 16; ++i) {
        expect(kLastBase[i] == nxvc::kLastBase[i], "kLastBase");
        expect(kLastRawBits[i] == nxvc::kLastRawBits[i], "kLastRawBits");
    }
    for (int b = 0; b < 4; ++b)
        for (int c = 0; c < 3; ++c)
            expect(kLevelCtx[b * 3 + c] == nxvc::kLevelCtx[b][c], "kLevelCtx");
    for (int i = 0; i < 64; ++i)
        expect(kZigzag8[i] == nxvc::kZigzag8[i], "kZigzag8");
    for (int i = 0; i < 16; ++i)
        expect(kZigzag4[i] == nxvc::kZigzag4[i], "kZigzag4");

    // Derived helpers.
    for (int p = 0; p < 64; ++p) {
        expect(nxs_band_of(p) == nxvc::band_of(p), "band_of");
        expect(nxs_last_class_of(p) == nxvc::last_class_of(p), "last_class_of");
        for (int pc = 0; pc < 3; ++pc)
            expect(nxs_level_ctx(p, pc) == nxvc::level_ctx(p, pc), "level_ctx");
    }
    for (int m = 0; m < 40; ++m)
        expect(nxs_level_class(m) == nxvc::level_class(m), "level_class");

    // Scan-table selection: nxs_scan_id + scan_index must equal ref's tables.
    for (int tskip = 0; tskip < 2; ++tskip)
        for (int n : {64, 16, 4, 1}) {
            const uint16_t *ref_scan = nxvc::scan_table(n, tskip != 0);
            int id = nxs_scan_id(n, tskip);
            for (int p = 0; p < n; ++p)
                expect(scan_index(id, p) == ref_scan[p], "scan_table");
        }

    // Tile geometry.
    for (int res = 0; res <= 2; ++res)
        for (int c444 = 0; c444 < 2; ++c444) {
            nxvc::TileGeom tg = nxvc::tile_geom(res, c444 != 0);
            expect(nxs_plane_size(0, res, c444) == tg.coded_size, "plane_size Y");
            expect(nxs_plane_size(1, res, c444) == tg.chroma_size, "plane_size U");
            expect(nxs_plane_size(3, res, c444) == tg.alpha_size, "plane_size A");
        }
    std::printf("  %s\n", g_fail ? "FAILED" : "all constants agree with ref");
}

// --- ref table set -> our flat cum ----------------------------------------
void load_ref_tables(Tables &tabs, std::vector<uint32_t> &flat) {
    flat.assign(size_t(kNumTableSets) * kNumCtx * kNumSym, 0);
    for (int k = 0; k < kNumTableSets; ++k) {
        nxvc::TableSet ts{};
        nxvc::build_default_set(ts, k);
        for (int c = 0; c < kNumCtx; ++c)
            for (int s = 0; s < kNumSym; ++s) {
                tabs.freq[k][c][s] = ts.ctx[c].freq[s];
                flat[size_t((k * kNumCtx + c) * kNumSym + s)] = ts.ctx[c].cum[s];
            }
    }
    if (!finalize(tabs)) {
        std::printf("  ref default tables are not normalised!\n");
        ++g_fail;
    }
    // finalize() must reproduce ref's own cum values exactly.
    for (int k = 0; k < kNumTableSets; ++k)
        for (int c = 0; c < kNumCtx; ++c)
            for (int s = 0; s < kNumSym; ++s)
                expect(tabs.cum[k][c][s] ==
                           flat[size_t((k * kNumCtx + c) * kNumSym + s)],
                       "cum from ref freq");
}

// --- 2 and 3: encoder byte-equality and model decode -----------------------
void check_streams() {
    std::printf("[streams]\n");
    Tables tabs{};
    std::vector<uint32_t> flat;
    load_ref_tables(tabs, flat);

    size_t checked = 0, byte_diff = 0, coef_diff = 0;

    for (uint32_t iter = 0; iter < 64; ++iter) {
        Rng rng(0x5eed0000u + iter);

        TileShape shape;
        shape.res_level = int(rng.below(3));
        shape.chroma444 = int(rng.below(2));
        shape.alpha_mode = (iter % 4 == 0) ? kAlphaModeCoded : 0;
        shape.frame_nplanes = shape.alpha_mode == kAlphaModeCoded ? 4 : 3;
        shape.tskip = int(rng.below(2));
        shape.table_set = int(rng.below(kNumTableSets));
        shape.tile_index = int(iter);

        std::vector<UnitInfo> units;
        int ncoef = build_units(shape, units);
        std::vector<int16_t> coef;
        make_tile_coefs(shape, units, ncoef, rng, 900, 700, 900, coef);

        // --- ref's encoder, over the same units ---------------------------
        std::vector<int16_t> ref_coef = coef;
        std::vector<nxvc::Unit> runits(units.size());
        for (size_t i = 0; i < units.size(); ++i) {
            runits[i].coef = ref_coef.data() + units[i].coef_base;
            runits[i].ncoef = uint16_t(units[i].ncoef);
            // Take transform-skip from the unit's own scan id: the DC-plane
            // unit never uses the raster scan even when it holds 64 values.
            runits[i].scan = nxvc::scan_table(
                units[i].ncoef, units[i].scan_id == kScanRaster8);
            runits[i].ctx_cbf = uint8_t(units[i].ctx_cbf);
            runits[i].ctx_last = uint8_t(units[i].ctx_last);
        }
        nxvc::TableSet ts{};
        nxvc::build_default_set(ts, shape.table_set);

        std::vector<uint8_t> ref_payload;
        if (!nxvc::encode_units(runits.data(), int(runits.size()), int(kLanes),
                                ts, ref_payload)) {
            std::printf("  ref encode_units failed on iter %u\n", iter);
            ++g_fail;
            continue;
        }

        // --- our test encoder ---------------------------------------------
        std::vector<uint8_t> ours;
        if (!encode_tile(shape, units, coef.data(), tabs, ours)) {
            std::printf("  our encode_tile failed on iter %u\n", iter);
            ++g_fail;
            continue;
        }
        const uint8_t *our_payload = ours.data() + kTileHeaderBytes;
        size_t our_len = ours.size() - kTileHeaderBytes;

        if (our_len != ref_payload.size() ||
            std::memcmp(our_payload, ref_payload.data(), our_len) != 0) {
            if (byte_diff == 0)
                std::printf(
                    "  payload differs on iter %u (ours %zu B, ref %zu B)\n",
                    iter, our_len, ref_payload.size());
            ++byte_diff;
        }

        // --- decode ref's payload with the Pass A model --------------------
        std::vector<uint8_t> bits;
        bits.assign(ours.begin(), ours.begin() + kTileHeaderBytes);
        // Re-stamp payload_len for ref's payload length.
        uint32_t w0 = 0;
        std::memcpy(&w0, bits.data(), 4);
        w0 &= ~(kThPayloadLenMask << kThPayloadLenShift);
        w0 |= (uint32_t(ref_payload.size()) & kThPayloadLenMask)
              << kThPayloadLenShift;
        std::memcpy(bits.data(), &w0, 4);
        bits.insert(bits.end(), ref_payload.begin(), ref_payload.end());
        for (int i = 0; i < 16; ++i) bits.push_back(0);

        TileDesc desc{0, uint32_t(kTileHeaderBytes + ref_payload.size()), 0, 0};
        std::vector<int16_t> got(size_t(ncoef), 0);
        std::vector<uint32_t> cbf(kCbfWordsPerTile, 0);
        uint32_t status = 0;

        Inputs in;
        in.bits = bits.data();
        in.bits_size = bits.size();
        in.tiles = &desc;
        in.num_tiles = 1;
        in.tables = flat.data();
        in.frame_nplanes = uint32_t(shape.frame_nplanes);
        in.coef_stride = uint32_t(ncoef);
        in.cbf_words = kCbfWordsPerTile;
        in.read_ptr_mode = kReadPtrBallot;

        std::vector<uint32_t> modes(kModeWordsPerTile, 0);
        Outputs out;
        out.coef = got.data();
        out.cbf = cbf.data();
        out.status = &status;
        out.modes = modes.data();
        decode(in, out);

        if (status != kStatusOk) {
            std::printf("  model status %u on iter %u\n", status, iter);
            ++g_fail;
        }
        for (int i = 0; i < ncoef; ++i)
            if (got[size_t(i)] != coef[size_t(i)]) { ++coef_diff; break; }
        ++checked;
    }

    std::printf("  tiles=%zu payload_byte_diff=%zu coef_diff=%zu\n", checked,
                byte_diff, coef_diff);
    if (byte_diff || coef_diff) ++g_fail;
    if (!byte_diff)
        std::printf(
            "  our test encoder is byte-identical to nxvc::encode_units\n");
}

// --- optional conformance vectors ------------------------------------------
void report_vectors() {
    namespace fs = std::filesystem;
    const char *dir = NXVW_PASSA_VECTOR_DIR;
    std::vector<std::string> v;
    std::error_code ec;
    if (fs::is_directory(dir, ec))
        for (const auto &e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".nxv") v.push_back(e.path().string());
    std::printf("[vectors] %s: %zu .nxv file(s)\n", dir, v.size());
    if (v.empty())
        std::printf(
            "  none present; ref-encoder agreement above covers the same "
            "ground at the payload level\n");
    else
        for (const auto &p : v) std::printf("  %s\n", p.c_str());
}

}  // namespace

int main() {
    check_constants();
    check_streams();
    report_vectors();
    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
