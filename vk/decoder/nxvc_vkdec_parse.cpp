// Host-side container parse.  See nxvc_vkdec_parse.h.
//
// The structure follows ref/src/codec_impl.inc one function at a time so the
// two can be diffed: parse_stream_header, parse_frame_header (matrices and
// probability tables included), then the tile-row / tile walk of
// nxvc_decoder_decode_frame.  Every check the reference makes is made here,
// with the same precedence, so a stream this decoder accepts is exactly a
// stream `nxv-dec` accepts.
#include "nxvc_vkdec_parse.h"

#include <algorithm>
#include <cstring>

#include "passA/syntax_constants.h"
#include "passB/syntax_constants.h"

namespace nxvcvk {

using namespace nxwarp_passA;

namespace {

// ---------------------------------------------------------------------------
// The normative default probability tables and the custom-table delta
// multipliers.  default_tables.inc is included verbatim from ref/ rather than
// copied, so a retrained table set can never drift between the CPU reference
// and the GPU decoder.  [REF] ref/src/tables.cpp.
// ---------------------------------------------------------------------------
namespace reftab {
using u16 = uint16_t;
constexpr int kNumCtx = 12;
constexpr int kNumSym = 16;
#include "../../ref/src/default_tables.inc"

// [REF] tables.cpp kDeltaMul: round(256 * 2^(d/4)) for d in [-16, 15], Q8.
constexpr u16 kDeltaMul[32] = {
    16,   19,   23,   27,   32,   38,   45,   54,   64,   76,  91,
    108,  128,  152,  181,  215,  256,  304,  362,  431,  512, 609,
    724,  861,  1024, 1218, 1448, 1722, 2048, 2435, 2896, 3444,
};
}  // namespace reftab

// [REF] tables.cpp normalize_freqs(): deterministic normalization of a
// 16-entry frequency row to sum 1024 with every entry >= 1.  This is the one
// place in the decode path that divides; it runs once per frame per
// transmitted table set, never per symbol (docs/SYNTAX.md 1).
void normalize_freqs(uint16_t f[16]) {
    int32_t sum = 0;
    for (int s = 0; s < 16; ++s) {
        if (f[s] < 1) f[s] = 1;
        sum += f[s];
    }
    if (sum == 1024) return;
    int32_t g[16];
    int32_t total = 0;
    for (int s = 0; s < 16; ++s) {
        g[s] = (int32_t)(((int32_t)f[s] * 1024) / sum);
        if (g[s] < 1) g[s] = 1;
        if (g[s] > 1009) g[s] = 1009;
        total += g[s];
    }
    while (total < 1024) {
        int best = 0;
        for (int s = 1; s < 16; ++s)
            if (g[s] > g[best]) best = s;
        g[best]++;
        total++;
    }
    while (total > 1024) {
        int best = -1;
        for (int s = 0; s < 16; ++s)
            if (g[s] > 1 && (best < 0 || g[s] > g[best])) best = s;
        if (best < 0) break;
        g[best]--;
        total--;
    }
    for (int s = 0; s < 16; ++s) f[s] = (uint16_t)g[s];
}

// [REF] tables.cpp finalize_ctx(), minus slot2sym: Pass A finds the symbol by
// a branchless binary search over cum[] instead of indexing a 1024-entry
// table, because 8 sets x 12 contexts of those would not fit in LDS.
bool finalize_cum(const uint16_t freq[16], uint32_t cum[16]) {
    int32_t c = 0;
    for (int s = 0; s < 16; ++s) {
        if (freq[s] == 0) return false;
        cum[s] = (uint32_t)c;
        c += freq[s];
    }
    return c == 1024;
}

// -------------------------------------------------------------- bit reader
// [REF] codec.cpp BitR: MSB-first, reads zero past the end.
struct BitR {
    const uint8_t *p;
    size_t n;
    size_t bit = 0;
    uint32_t get(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; ++i) {
            uint32_t b = 0;
            if (bit >> 3 < n) b = (p[bit >> 3] >> (7 - (bit & 7))) & 1;
            ++bit;
            v = (v << 1) | b;
        }
        return v;
    }
};

// ------------------------------------------------------- byte reader (LE)
struct BR {
    const uint8_t *p;
    size_t n;
    size_t i = 0;
    bool ok = true;
    uint32_t u8v() {
        if (i + 1 > n) { ok = false; return 0; }
        return p[i++];
    }
    uint32_t u16v() {
        uint32_t a = u8v(), b = u8v();
        return a | (b << 8);
    }
    uint32_t u32v() {
        uint32_t a = u16v(), b = u16v();
        return a | (b << 16);
    }
    uint64_t u64v() {
        uint64_t a = u32v(), b = u32v();
        return a | (b << 32);
    }
};

int32_t iclamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr size_t kStreamHeaderBytes = 64;
constexpr size_t kFrameHeaderBytes = 40;
constexpr size_t kTileRowHeaderBytes = 12;

// Tool bits this decoder implements.  Identical to NXVC_TOOLS_SUPPORTED in
// <nxvc/nxvc.h>; restated here so the GPU decoder's forward-compatibility
// gate is visible in one place and can diverge from the reference's when a
// tool lands on one side first.
constexpr uint64_t kToolsSupported =
    (1ull << 0) |  // INTRA_DC_PLANE
    (1ull << 1) |  // TRANSFORM_SKIP
    (1ull << 2) |  // RES_LEVEL
    (1ull << 3) |  // CHROMA444
    (1ull << 4) |  // ALPHA
    (1ull << 5) |  // LOSSLESS
    (1ull << 6) |  // CUSTOM_TABLES
    (1ull << 7) |  // NSUB_VAR
    (1ull << 8) |  // PER_TILE_CHROMA
    (1ull << 9);   // YCOCGR
constexpr uint64_t kToolNsubVar = 1ull << 7;
constexpr uint64_t kToolResLevel = 1ull << 2;
constexpr uint64_t kToolTransformSkip = 1ull << 1;
constexpr uint64_t kToolPerTileChroma = 1ull << 8;

}  // namespace

// ---------------------------------------------------------------- tables
void build_default_tables(std::vector<uint32_t> &cum) {
    cum.assign((size_t)kNumTableSets * kNumCtx * kNumSym, 0);
    for (int set = 0; set < kNumTableSets; ++set)
        for (int c = 0; c < kNumCtx; ++c) {
            uint32_t *dst = &cum[((size_t)set * kNumCtx + c) * kNumSym];
            uint32_t acc = 0;
            for (int s = 0; s < kNumSym; ++s) {
                dst[s] = acc;
                acc += reftab::kDefaultFreq[set][c][s];
            }
        }
}

bool parse_table_set(const uint8_t *bits120, int set_index,
                     uint32_t *cum_of_set) {
    BitR br{bits120, 120, 0};
    for (int c = 0; c < kNumCtx; ++c) {
        uint16_t f[kNumSym];
        for (int s = 0; s < kNumSym; ++s) {
            uint32_t d = br.get(5);
            int32_t def = reftab::kDefaultFreq[set_index][c][s];
            int32_t v = (def * (int32_t)reftab::kDeltaMul[d] + 128) >> 8;
            f[s] = (uint16_t)iclamp(v, 1, 32767);
        }
        normalize_freqs(f);
        if (!finalize_cum(f, cum_of_set + (size_t)c * kNumSym)) return false;
    }
    return true;
}

void resolve_matrices(uint32_t quant_matrix, const uint8_t *custom128,
                      int32_t out128[128]) {
    if (quant_matrix == 255 && custom128) {
        for (int i = 0; i < 64; ++i) {
            out128[i] = iclamp(custom128[i], 1, 32);
            out128[64 + i] = iclamp(custom128[64 + i], 1, 32);
        }
        return;
    }
    int m = (int)iclamp((int32_t)quant_matrix, 0, 3);
    int mc = m == 0 ? 0 : 3;
    for (int i = 0; i < 64; ++i) {
        out128[i] = nxvw::kWeightFlat[m * 64 + i];
        out128[64 + i] = nxvw::kWeightFlat[mc * 64 + i];
    }
}

// --------------------------------------------------------- stream header
nxvc_vkd_status parse_stream_header(const uint8_t *buf, size_t len,
                                    StreamInfo &si, size_t *consumed) {
    if (!buf) return NXVC_VKD_ERR_ARG;
    if (len < kStreamHeaderBytes) return NXVC_VKD_ERR_TRUNCATED;
    BR br{buf, len, 0, true};
    si = StreamInfo{};
    si.magic = br.u32v();
    if (si.magic != 0x3156584Eu) return NXVC_VKD_ERR_VERSION;
    si.version = br.u8v();
    if (si.version != 1) return NXVC_VKD_ERR_VERSION;
    si.profile = br.u8v();
    si.level = br.u8v();
    si.tile_size = br.u8v();
    if (si.tile_size & 0xfe) return NXVC_VKD_ERR_BITSTREAM;
    if (si.tile_size & 1) return NXVC_VKD_ERR_UNSUPPORTED;  // 32x32 profile
    si.width = br.u16v();
    si.height = br.u16v();
    si.eyes = br.u8v();
    si.bit_depth = br.u8v();
    si.num_layers = br.u8v();
    si.chroma = br.u8v();
    for (int i = 0; i < 4; ++i) br.u32v();  // layer_desc
    si.tools = br.u64v();
    si.alpha = br.u8v();
    si.color_transform = br.u8v();
    si.color_space = br.u8v();
    br.i = 62;
    si.ext_len = br.u16v();
    if (!br.ok) return NXVC_VKD_ERR_TRUNCATED;
    if (len < kStreamHeaderBytes + si.ext_len) return NXVC_VKD_ERR_TRUNCATED;
    if (si.width < 16 || si.height < 16 || si.width > 4096 ||
        si.height > 4096 || (si.width & 1) || (si.height & 1))
        return NXVC_VKD_ERR_BITSTREAM;
    if (si.eyes != 1 || si.bit_depth != 8 || si.num_layers != 1)
        return NXVC_VKD_ERR_UNSUPPORTED;
    if (si.chroma > 1 || si.color_transform > 1 || si.alpha > 1 ||
        si.color_space > 3)
        return NXVC_VKD_ERR_BITSTREAM;
    if ((si.color_space == 3) != (si.color_transform == 1))
        return NXVC_VKD_ERR_BITSTREAM;
    if (si.tools & ~kToolsSupported) return NXVC_VKD_ERR_VERSION;

    // TLV area: every unrecognised type is skipped (docs/SYNTAX.md 2.1).
    size_t p = kStreamHeaderBytes, end = kStreamHeaderBytes + si.ext_len;
    while (p + 4 <= end) {
        uint32_t tl = (uint32_t)buf[p + 2] | ((uint32_t)buf[p + 3] << 8);
        size_t adv = 4 + tl + ((4 - (tl & 3)) & 3);
        if (p + adv > end) return NXVC_VKD_ERR_BITSTREAM;
        p += adv;
    }
    if (p != end) return NXVC_VKD_ERR_BITSTREAM;

    si.tiles_x = (si.width + 63) / 64;
    si.tiles_y = (si.height + 63) / 64;
    si.tile_count = si.tiles_x * si.tiles_y;
    if (si.chroma == 0) {
        si.cw = (si.width + 1) / 2;
        si.ch = (si.height + 1) / 2;
    } else {
        si.cw = si.width;
        si.ch = si.height;
    }
    if (si.tiles_x > 64) return NXVC_VKD_ERR_BITSTREAM;
    if (consumed) *consumed = kStreamHeaderBytes + si.ext_len;
    return NXVC_VKD_OK;
}

// ----------------------------------------------------------------- frame
nxvc_vkd_status parse_frame(const StreamInfo &si, const uint8_t *buf,
                            size_t len, bool allow_skipped, FrameParse &fp) {
    if (!buf) return NXVC_VKD_ERR_ARG;
    if (len < kFrameHeaderBytes) return NXVC_VKD_ERR_TRUNCATED;

    // --- frame header --------------------------------------------------
    BR br{buf, len, 0, true};
    fp = FrameParse{};
    fp.frame_number = br.u16v();
    br.i += 26;  // pose, opaque to the codec (docs/SYNTAX.md 3.2)
    fp.base_qp = br.u8v();
    fp.chroma_qp_off = (int8_t)br.u8v();
    fp.alpha_qp_off = (int8_t)br.u8v();
    fp.quant_matrix = br.u8v();
    uint32_t tables_present = br.u8v();
    br.u8v();  // ref_slots (Phase 2)
    br.u8v();  // flags
    br.u8v();  // reserved
    uint32_t frame_bytes = br.u32v();
    if (!br.ok) return NXVC_VKD_ERR_TRUNCATED;
    if (fp.base_qp > 63) return NXVC_VKD_ERR_BITSTREAM;
    if (fp.quant_matrix > 3 && fp.quant_matrix != 255)
        return NXVC_VKD_ERR_BITSTREAM;
    if (frame_bytes < kFrameHeaderBytes || frame_bytes > len)
        return NXVC_VKD_ERR_TRUNCATED;
    fp.frame_bytes = frame_bytes;

    size_t off = kFrameHeaderBytes;
    const uint8_t *custom = nullptr;
    if (fp.quant_matrix == 255) {
        if (off + 128 > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
        custom = buf + off;
        off += 128;
    }
    resolve_matrices(fp.quant_matrix, custom, fp.weights);

    build_default_tables(fp.cum);
    for (int k = 0; k < 8; ++k) {
        if (!(tables_present & (1u << k))) continue;
        if (off + 120 > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
        if (!parse_table_set(buf + off, k,
                             &fp.cum[(size_t)k * kNumCtx * kNumSym]))
            return NXVC_VKD_ERR_BITSTREAM;
        off += 120;
    }

    // --- geometry ------------------------------------------------------
    const bool chroma420 = si.chroma == 0;
    const int nplanes = si.nplanes();
    fp.frame_nplanes = (uint32_t)nplanes;
    fp.coef_stride =
        (uint32_t)nxvw::nxvw_coef_stride_i16(chroma420 ? 1 : 0, si.alpha ? 1 : 0);
    fp.cbf_words = kCbfWordsPerTile;

    const uint32_t ntiles = si.tile_count;
    fp.recs.assign(ntiles, NxvwTileRec{0, 0, 0, 0xffffffffu});

    // Per lane count, the descriptors of the tiles that use it.  nsub_log2 is
    // 0..5, so at most six Pass A dispatches; the usual frame has one.
    std::vector<TileDesc> by_lane[6];
    std::vector<uint32_t> lane_tile[6];

    // --- tile rows -----------------------------------------------------
    for (uint32_t row = 0; row < si.tiles_y; ++row) {
        if (off + kTileRowHeaderBytes > frame_bytes)
            return NXVC_VKD_ERR_TRUNCATED;
        BR rb{buf, frame_bytes, off, true};
        uint32_t fn = rb.u16v();
        uint32_t ri = rb.u8v();
        uint32_t tcount = rb.u8v();
        uint64_t skip = rb.u64v();
        if (!rb.ok) return NXVC_VKD_ERR_TRUNCATED;
        if (fn != fp.frame_number || ri != row) return NXVC_VKD_ERR_BITSTREAM;
        // [REF] "no references in v1": the reference decoder refuses any
        // skipped tile outright.  With NXVC_VKD_FLAG_ALLOW_SKIPPED_TILES the
        // decoder instead emits a WARP_SKIP record over a zeroed coefficient
        // slot, which is deterministic and is the shape the Phase 2 inter
        // predictor replaces.
        if (skip != 0 && !allow_skipped) return NXVC_VKD_ERR_UNSUPPORTED;
        {
            uint32_t expect = 0;
            for (uint32_t i = 0; i < si.tiles_x; ++i)
                if (!((skip >> i) & 1)) ++expect;
            if (tcount != expect) return NXVC_VKD_ERR_BITSTREAM;
        }
        off = rb.i;

        for (uint32_t k = 0; k < si.tiles_x; ++k) {
            const uint32_t tindex = row * si.tiles_x + k;
            if ((skip >> k) & 1) {
                // WARP_SKIP: no header, no payload, no coefficients.
                fp.recs[tindex].w0 = (k & 0xfffu) << 4;
                fp.recs[tindex].w1 = 0;  // mode == WARP_SKIP, res_level 0
                fp.recs[tindex].w2 = 255u;  // alpha_value 255, present 0
                fp.zero_tiles.push_back(tindex);
                ++fp.tiles_skipped;
                continue;
            }
            if (off + 8 > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
            BR tb{buf, frame_bytes, off, true};
            uint32_t w0 = tb.u32v(), w1 = tb.u32v();

            const uint32_t layer = w0 & 3u;
            const uint32_t eye = (w0 >> 2) & 1u;
            const uint32_t tile_index = (w0 >> 4) & 0xfffu;
            const uint32_t payload_len = (w0 >> 16) & 0xffffu;
            const uint32_t mode = w1 & 7u;
            const uint32_t res_level = (w1 >> 3) & 3u;
            const uint32_t chroma444 = (w1 >> 5) & 1u;
            const uint32_t alpha_mode = (w1 >> 6) & 3u;
            const uint32_t nsub_log2 = (w1 >> 17) & 7u;
            const uint32_t mv_present = (w1 >> 20) & 1u;
            const uint32_t tskip = (w1 >> 23) & 1u;

            // Exactly the reference's checks, in the reference's order.
            if ((w0 >> 3) & 1) return NXVC_VKD_ERR_BITSTREAM;
            if (w1 >> 26) return NXVC_VKD_ERR_BITSTREAM;
            if (layer != 0 || eye != 0) return NXVC_VKD_ERR_UNSUPPORTED;
            if (mode > 4) return NXVC_VKD_ERR_BITSTREAM;
            if (mode != 3) return NXVC_VKD_ERR_UNSUPPORTED;  // INTRA only
            if (res_level > 2) return NXVC_VKD_ERR_BITSTREAM;
            if (alpha_mode == 3) return NXVC_VKD_ERR_BITSTREAM;
            if (nsub_log2 > 5) return NXVC_VKD_ERR_BITSTREAM;
            if (nsub_log2 != 3 && !(si.tools & kToolNsubVar))
                return NXVC_VKD_ERR_BITSTREAM;
            if (res_level != 0 && !(si.tools & kToolResLevel))
                return NXVC_VKD_ERR_BITSTREAM;
            if (tskip && !(si.tools & kToolTransformSkip))
                return NXVC_VKD_ERR_BITSTREAM;
            if (!chroma444 && si.chroma == 1 &&
                !(si.tools & kToolPerTileChroma))
                return NXVC_VKD_ERR_BITSTREAM;
            if (chroma444 && si.chroma != 1) return NXVC_VKD_ERR_BITSTREAM;
            if (alpha_mode != 0 && !si.alpha) return NXVC_VKD_ERR_BITSTREAM;
            if (tile_index != k) return NXVC_VKD_ERR_BITSTREAM;

            const size_t hdr_off = off;
            off = tb.i;
            uint32_t alpha_value = 255;
            if (mv_present) {
                if (off + 2 > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
                off += 2;
            }
            if (alpha_mode == 1) {
                if (off + 1 > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
                alpha_value = buf[off];
                off += 1;
            }
            if (off + payload_len > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
            off += payload_len;

            TileDesc d{};
            d.bits_offset = (uint32_t)hdr_off;
            d.bits_length = (uint32_t)(off - hdr_off);
            d.coef_offset = tindex * fp.coef_stride;
            d.cbf_offset = tindex * fp.cbf_words;
            by_lane[nsub_log2].push_back(d);
            lane_tile[nsub_log2].push_back(tindex);

            fp.recs[tindex].w0 = w0;
            fp.recs[tindex].w1 = w1;
            fp.recs[tindex].w2 = (alpha_value & 0xffu) | (1u << 8);
            fp.payload_bytes += payload_len;
            if (tskip) ++fp.tiles_tskip;
            if (alpha_mode == 2) fp.any_alpha_coded = true;
        }
    }
    if (off != frame_bytes) return NXVC_VKD_ERR_BITSTREAM;

    // --- Pass A dispatch groups ----------------------------------------
    // Each group starts at a multiple of its own tiles-per-group so the
    // dispatch can be issued with vkCmdDispatchBase and the kernel's
    // `tile = gl_WorkGroupID.x * TILES_PER_GROUP + slot` lands on the right
    // descriptor with no extra push constant.  The padding slots between
    // groups are never visited: the previous group's `num_tiles` limit stops
    // short of them and the next group's base starts past them.
    for (int ns = 0; ns < 6; ++ns) {
        if (by_lane[ns].empty()) continue;
        const uint32_t lanes = 1u << ns;
        const uint32_t tpg = nxs_tiles_per_group(lanes);
        uint32_t first = (uint32_t)fp.desc.size();
        first = ((first + tpg - 1) / tpg) * tpg;
        fp.desc.resize(first);
        fp.desc_tile.resize(first, 0xffffffffu);
        fp.desc.insert(fp.desc.end(), by_lane[ns].begin(), by_lane[ns].end());
        fp.desc_tile.insert(fp.desc_tile.end(), lane_tile[ns].begin(),
                            lane_tile[ns].end());
        LaneGroup g{};
        g.lanes = lanes;
        g.first = first;
        g.count = (uint32_t)by_lane[ns].size();
        g.limit = first + g.count;
        g.groups = (g.count + tpg - 1) / tpg;
        fp.groups.push_back(g);
    }

    // --- Pass B push constants -----------------------------------------
    NxvwPassBPush &p = fp.push;
    p.imageW = (int)si.width;
    p.imageH = (int)si.height;
    p.tilesX = (int)si.tiles_x;
    p.baseQp = (int)fp.base_qp;
    p.chromaQpOff = fp.chroma_qp_off;
    p.alphaQpOff = fp.alpha_qp_off;
    p.coefStrideI16 = (int)fp.coef_stride;
    p.colorTransform = (int)si.color_transform;
    p.chroma420 = chroma420 ? 1 : 0;
    p.alphaPresent = si.alpha ? 1 : 0;
    const int c420 = chroma420 ? 1 : 0;
    p.planeWords0 = nxvw::nxvw_plane_store_words(0, c420);
    p.planeWords1 = nxvw::nxvw_plane_store_words(1, c420);
    p.planeWords2 = nxvw::nxvw_plane_store_words(2, c420);
    // The alpha slot in the shared sample store is only reached when a tile
    // actually codes an alpha plane (alpha_mode == 2).  A stream that carries
    // alpha as opaque or constant needs no slot for it, which is the
    // difference between a 4:4:4 tile fitting a 32 KB device and not.
    p.planeWords3 = (si.alpha && fp.any_alpha_coded)
                        ? nxvw::nxvw_plane_store_words(3, c420)
                        : 0;
    return NXVC_VKD_OK;
}

}  // namespace nxvcvk
