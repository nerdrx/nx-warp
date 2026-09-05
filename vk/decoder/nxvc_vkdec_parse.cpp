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
// The .inc declares both built-in families; these names are the ones it uses.
constexpr int kNumCtxV1 = 12;
constexpr int kNumCtxV2 = 16;
constexpr int kNumCtxV3 = 27;
constexpr int kNumSym = 16;
#include "../../ref/src/default_tables.inc"

// [REF] tables.cpp default_freq(): which family a stream's context count
// selects.  Contexts 0..11 keep their meaning in both models but not their
// statistics, because under CTX_V2 they no longer see the DC plane.
inline u16 default_freq(int nctx, int set_index, int c, int s) {
    if (set_index < 0) set_index = 0;
    if (set_index > 7) set_index = 7;
    if (nctx >= kNumCtxV3) return kDefaultFreqV3[set_index][c][s];
    if (nctx >= kNumCtxV2) return kDefaultFreqV2[set_index][c][s];
    return kDefaultFreq[set_index][c][s];
}

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
    (1ull << 9) |  // YCOCGR
    (1ull << 17) | // INTRA_DIR: directional intra          [v3]
    (1ull << 19) | // XFORM_4X4_SPLIT: per-block 4x4 split  [minor 6]
    (1ull << 20) | // WM_ID: per-tile weighting-matrix override
    (1ull << 21) | // CTX_V2: the 16-context entropy model  [v3]
    (1ull << 22) | // SIGN_HIDE: sign data hiding           [v3]
    (1ull << 24) | // INTRA_CFL: chroma from luma           [minor 6]
    (1ull << 25) | // CTX_V3: the 27-context entropy model  [minor 6]
    (1ull << 26) | // TAB_V2: variable-length table sets    [minor 6]
    // ------------------------------------------------ Phase 2 ([SYN] 13)
    (1ull << 10) | // INTER: inter modes                    [inter]
    (1ull << 11) | // WARP: pose-warped prediction          [inter]
    (1ull << 12) | // STEREO: inter-view prediction         [inter]
    (1ull << 28) | // NEAR_SKIP: row-header corrections     [inter]
    (1ull << 29);  // QUAD_MV: four quadrant vectors        [inter]
// Deliberately absent: bit 27 XFORM_LARGE and bit 30 ENTROPY_LITE.  Pass B's
// block loop is written for 64-coefficient blocks; until it carries the 16x16
// and 32x32 forms this decoder refuses such a stream at the handshake rather
// than mis-decoding it.  docs/TOOLBITS.md 7.  Bit 23 FILTER_CATMULL_ROM and
// bit 14 BITDEPTH10 are reject-in-v1 ([SYN] 2.3) and must stay out.
constexpr uint64_t kToolLossless = 1ull << 5;
constexpr uint64_t kToolIntraDir = 1ull << 17;
constexpr uint64_t kToolCtxV2 = 1ull << 21;
constexpr uint64_t kToolCtxV3 = 1ull << 25;
constexpr uint64_t kToolTabV2 = 1ull << 26;
constexpr uint64_t kToolCustomTables = 1ull << 6;
constexpr uint64_t kToolXformLarge = 1ull << 27;
constexpr uint64_t kToolSplit4 = 1ull << 19;
constexpr uint64_t kToolCfl = 1ull << 24;
constexpr uint64_t kToolSignHide = 1ull << 22;
constexpr uint64_t kToolNsubVar = 1ull << 7;
constexpr uint64_t kToolResLevel = 1ull << 2;
constexpr uint64_t kToolTransformSkip = 1ull << 1;
constexpr uint64_t kToolPerTileChroma = 1ull << 8;
constexpr uint64_t kToolWmId = 1ull << 20;
constexpr uint64_t kToolBitDepth10 = 1ull << 14;
constexpr uint64_t kToolInter = 1ull << 10;
constexpr uint64_t kToolWarp = 1ull << 11;
constexpr uint64_t kToolStereo = 1ull << 12;
constexpr uint64_t kToolNearSkip = 1ull << 28;
constexpr uint64_t kToolQuadMv = 1ull << 29;

}  // namespace

// The decoder's half of the docs/SYNTAX.md 2.3 handshake.  One definition,
// exported so the C ABI and the conformance harness read the same number the
// stream-header check does; kPhase1Tools in
// tests/vk-decoder/conformance/test_vk_decoder_conformance.cpp must equal it.
uint64_t tools_supported() { return kToolsSupported; }

// ---------------------------------------------------------------- tables
// [REF] tables.cpp build_default_set(): the table object carries every context
// of the model the stream selected.  Contexts beyond its coded count are never
// selected, but they are filled (from context 0) so the object is well formed.
//
// [minor 6] The STRIDE is the model's own width, not the widest model's.  It
// used to be the widest, so that the host had one layout to build whichever
// model a stream picked -- and when CTX_V3 took the widest from 16 contexts to
// 27 that made every v1 and v2 stream carry a 13824-byte shared table instead
// of an 8192-byte one, which on an Adreno 650 is the difference between two
// resident workgroups and one.  Pass A takes the stride as specialisation
// constant 4 instead.
int table_stride(int nctx) { return nctx >= kNumCtxV3 ? kNumCtxV3 : kNumCtxV2; }

void build_default_tables(std::vector<uint32_t> &cum, int nctx) {
    if (nctx < kNumCtxV1) nctx = kNumCtxV1;
    const int stride = table_stride(nctx);
    cum.assign((size_t)kNumTableSets * kNumCtx * kNumSym, 0);
    for (int set = 0; set < kNumTableSets; ++set)
        for (int c = 0; c < stride; ++c) {
            const int src = c < nctx ? c : 0;
            uint32_t *dst = &cum[((size_t)set * stride + c) * kNumSym];
            uint32_t acc = 0;
            for (int s = 0; s < kNumSym; ++s) {
                dst[s] = acc;
                acc += reftab::default_freq(nctx, set, src, s);
            }
        }
}

// docs/SYNTAX.md 9.4: nctx x 16 five-bit log-domain deltas, MSB-first, each
// against the built-in default row of the same (set, context).  Under TAB_V2
// (tool bit 26) every context is preceded by a one-bit `row_coded` flag and an
// uncoded row IS the default -- the point of the tool being that a row whose
// trained version does not save more than the 80 bits it costs should not be
// sent.  Contexts beyond `nctx` keep the defaults build_default_tables()
// already wrote.  [REF] codec_impl.inc parse_table_set(), line for line.
static bool parse_table_set(BitR &br, int set_index, int nctx, int tab_v2,
                     uint32_t *cum_of_set) {
    for (int c = 0; c < nctx; ++c) {
        uint16_t f[kNumSym];
        const bool coded = tab_v2 ? br.get(1) != 0 : true;
        for (int s = 0; s < kNumSym; ++s) {
            int32_t def = reftab::default_freq(nctx, set_index, c, s);
            if (!coded) { f[s] = (uint16_t)def; continue; }
            int32_t v =
                (def * (int32_t)reftab::kDeltaMul[br.get(5)] + 128) >> 8;
            f[s] = (uint16_t)iclamp(v, 1, 32767);
        }
        if (coded) normalize_freqs(f);
        if (!finalize_cum(f, cum_of_set + (size_t)c * kNumSym)) return false;
    }
    return true;
}

void resolve_matrices(uint32_t quant_matrix, const uint8_t *custom128,
                      int32_t out512[512]) {
    // Set 0: the frame's own pair.
    if (quant_matrix == 255 && custom128) {
        for (int i = 0; i < 64; ++i) {
            out512[i] = iclamp(custom128[i], 1, 32);
            out512[64 + i] = iclamp(custom128[64 + i], 1, 32);
        }
    } else {
        int m = (int)iclamp((int32_t)quant_matrix, 0, 3);
        int mc = m == 0 ? 0 : 3;
        for (int i = 0; i < 64; ++i) {
            out512[i] = nxvw::kWeightFlat[m * 64 + i];
            out512[64 + i] = nxvw::kWeightFlat[mc * 64 + i];
        }
    }
    // Sets 1..3: [REF] TileCoder::setup(), wm_id != 0 uses kWeight[wm_id] for
    // luma and alpha and kWeight[3] for chroma, whatever the frame carries.
    for (int k = 1; k <= 3; ++k)
        for (int i = 0; i < 64; ++i) {
            out512[k * 128 + i] = nxvw::kWeightFlat[k * 64 + i];
            out512[k * 128 + 64 + i] = nxvw::kWeightFlat[3 * 64 + i];
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
    if (si.eyes < 1 || si.eyes > 2 || si.bit_depth != 8 || si.num_layers != 1)
        return NXVC_VKD_ERR_UNSUPPORTED;
    // [inter] A stereo frame is `eyes` PICTURES, not one double-width picture
    // ([SYN] 3.3), and this decoder merges them into one raster of 64-pixel
    // columns: `tilesX = eyes * cols_per_eye`, `imageW = eyes * width`.  That
    // is exact only when each eye's last tile column is full, i.e. when the
    // width is a multiple of 64; otherwise eye 1 starts at pixel `width`
    // rather than at `cols_per_eye * 64` and every tile-to-pixel mapping in
    // Pass B would need a per-eye x origin.  Refusing the case is honest and
    // one line; mis-mapping it silently is not.  vk/decoder/README.md.
    if (si.eyes == 2 && (si.width & 63)) return NXVC_VKD_ERR_UNSUPPORTED;
    // [REF] Annex D D-16: version 1 is 8-bit, and tool bit 14 has no defined
    // sample domain, quantiser scaling or clamp in it.
    if (si.tools & kToolBitDepth10) return NXVC_VKD_ERR_VERSION;
    // [REF] Annex D D-1: `warp_present` and the warped tile modes need the
    // WARP tool, and WARP without INTER says nothing (r27).
    if ((si.tools & kToolWarp) && !(si.tools & kToolInter))
        return NXVC_VKD_ERR_BITSTREAM;
    if ((si.tools & kToolStereo) && si.eyes != 2)
        return NXVC_VKD_ERR_BITSTREAM;
    if (si.chroma > 1 || si.color_transform > 1 || si.alpha > 1 ||
        si.color_space > 3)
        return NXVC_VKD_ERR_BITSTREAM;
    if ((si.color_space == 3) != (si.color_transform == 1))
        return NXVC_VKD_ERR_BITSTREAM;
    // [REF] docs/SYNTAX.md 2: YCoCg-R chroma is 9-bit and biased by 256 and
    // the transform runs before subsampling, so a 4:2:0 YCoCg-R stream would
    // push 9-bit chroma through an 8-bit plane.  r15 pins the refusal.
    if (si.color_transform == 1 && si.chroma != 1) return NXVC_VKD_ERR_BITSTREAM;
    if (si.tools & ~kToolsSupported) return NXVC_VKD_ERR_VERSION;
    // [SYN] 2.3: hiding a sign spends one level step, so a lossless stream
    // cannot carry it and a decoder that accepted both would not know which.
    // r17 pins the refusal.
    if ((si.tools & kToolLossless) && (si.tools & kToolSignHide))
        return NXVC_VKD_ERR_BITSTREAM;
    // [SYN] 9.9: v3 REFINES the v2 model rather than replacing it -- it reuses
    // v2's DC-plane and mode splits and only says how the rest is conditioned
    // -- so CTX_V3 without CTX_V2 names no model at all.
    if ((si.tools & kToolCtxV3) && !(si.tools & kToolCtxV2))
        return NXVC_VKD_ERR_BITSTREAM;
    // [SYN] 9.4: the row-skip flag only exists inside a transmitted table set.
    if ((si.tools & kToolTabV2) && !(si.tools & kToolCustomTables))
        return NXVC_VKD_ERR_BITSTREAM;

    // TLV area: every unrecognised type is skipped (docs/SYNTAX.md 2.1).
    size_t p = kStreamHeaderBytes, end = kStreamHeaderBytes + si.ext_len;
    while (p + 4 <= end) {
        uint32_t tl = (uint32_t)buf[p + 2] | ((uint32_t)buf[p + 3] << 8);
        size_t adv = 4 + tl + ((4 - (tl & 3)) & 3);
        if (p + adv > end) return NXVC_VKD_ERR_BITSTREAM;
        p += adv;
    }
    if (p != end) return NXVC_VKD_ERR_BITSTREAM;

    // [SYN] 3.3: cols_per_eye = ceil(width / 64), rows = ceil(height / 64),
    // cols = eyes * cols_per_eye, and a frame holds `eyes * rows` tile-row
    // structures ordered row-major, eye-minor.
    si.tiles_x = (si.width + 63) / 64;
    si.tiles_y = (si.height + 63) / 64;
    si.cols = si.eyes * si.tiles_x;
    si.tile_count = si.cols * si.tiles_y;
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
                            size_t len, bool allow_skipped, FrameParse &fp,
                            InterCtx *ic) {
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
    fp.ref_slots = br.u8v();
    const uint32_t flags = br.u8v();
    fp.flags = flags;
    br.u8v();  // reserved
    // [v3] The three v2 intra tools are stream-level and frame-uniform.
    // [SYN] 3.1 / 7.5: frame flags bit 2 selects the layered form of
    // directional intra and is meaningless -- therefore illegal -- without
    // tool bit 17.  r14 pins the refusal, and this check sits exactly where
    // ref/src/codec_impl.inc parse_frame_header() makes it, before
    // `frame_bytes` is even read, so the status a stream gets is the same.
    fp.nctx = (si.tools & kToolCtxV3)   ? kNumCtxV3
              : (si.tools & kToolCtxV2) ? kNumCtxV2
                                        : kNumCtxV1;
    fp.tab_v2 = (si.tools & kToolTabV2) ? 1 : 0;
    fp.xform_large = (si.tools & kToolXformLarge) ? 1 : 0;
    fp.ctx_stride = table_stride(fp.nctx);
    fp.intra_dir = (si.tools & kToolIntraDir) ? 1 : 0;
    fp.sdh = (si.tools & kToolSignHide) ? 1 : 0;
    fp.split4 = (si.tools & kToolSplit4) ? 1 : 0;
    fp.cfl = (si.tools & kToolCfl) ? 1 : 0;
    fp.dir_layer = (flags >> 2) & 1;
    if (fp.dir_layer && !fp.intra_dir) return NXVC_VKD_ERR_BITSTREAM;
    // [SYN] 7.7: CFL is a mode inside the CTX_V2 mode symbol of the REPLACE
    // form of directional intra, and none of the three is optional for it.
    // [REF] codec_impl.inc parse_frame_header(), same place, same order.
    if (fp.cfl && (!fp.intra_dir || fp.nctx < kNumCtxV2 || fp.dir_layer))
        return NXVC_VKD_ERR_BITSTREAM;
    // [inter] Frame-uniform Phase 2 state.  [REF] codec_impl.inc
    // parse_frame_header(), same place, same order, same statuses.
    fp.inter = (si.tools & kToolInter) ? 1 : 0;
    fp.warp_tool = (si.tools & kToolWarp) ? 1 : 0;
    fp.stereo_tool = (si.tools & kToolStereo) ? 1 : 0;
    fp.near_skip_tool = (si.tools & kToolNearSkip) ? 1 : 0;
    fp.quad_tool = (si.tools & kToolQuadMv) ? 1 : 0;
    fp.warp_present = (flags >> 3) & 1;
    if (flags & 0xf0) return NXVC_VKD_ERR_BITSTREAM;   // reserved bits 4-7
    // Annex D D-1: warp_present requires the WARP tool bit (r21 is the other
    // direction, a warped tile without the flag).
    if (fp.warp_present && !fp.warp_tool) return NXVC_VKD_ERR_BITSTREAM;
    // Annex D D-10: `ref_slots` is a bitmask and in version 1 it must name
    // exactly the slot this frame's number addresses.  Only an inter stream
    // has a ring for it to describe (r25).
    if (fp.inter && fp.ref_slots != (1u << (fp.frame_number & 3u)))
        return NXVC_VKD_ERR_BITSTREAM;
    if (fp.inter && !ic) return NXVC_VKD_ERR_UNSUPPORTED;
    fp.cur_slot = fp.frame_number & 3u;
    uint32_t frame_bytes = br.u32v();
    if (!br.ok) return NXVC_VKD_ERR_TRUNCATED;
    if (fp.base_qp > 63) return NXVC_VKD_ERR_BITSTREAM;
    if (fp.quant_matrix > 3 && fp.quant_matrix != 255)
        return NXVC_VKD_ERR_BITSTREAM;
    if (frame_bytes < kFrameHeaderBytes || frame_bytes > len)
        return NXVC_VKD_ERR_TRUNCATED;
    fp.frame_bytes = frame_bytes;

    size_t off = kFrameHeaderBytes;
    // --- warp_ext(), [SYN] 3.1.1 ---------------------------------------
    // 36 bytes per eye, nine little-endian int32, ascending eye order,
    // immediately after the 40-byte frame header and before the custom
    // matrices.  Annex D D-1 states four MUST-reject conditions and all four
    // are checked here, before a single sample is predicted -- which is what
    // lets Pass W's fixed 32-iteration restoring divide use a uint32
    // remainder (r18, r19, r20).
    if (fp.warp_present) {
        const size_t need = 36u * si.eyes;
        if (off + need > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
        BR wr{buf, frame_bytes, off, true};
        for (uint32_t eye = 0; eye < si.eyes; ++eye) {
            for (int i = 0; i < 9; ++i)
                fp.warp[eye].h[i] = (int32_t)wr.u32v();
            if (fp.warp[eye].h[8] != nxvw::kWarpH22)
                return NXVC_VKD_ERR_BITSTREAM;
            for (int i = 0; i < 9; ++i)
                if (fp.warp[eye].h[i] < -(int32_t)nxvw::kWarpEntryMax ||
                    fp.warp[eye].h[i] > (int32_t)nxvw::kWarpEntryMax)
                    return NXVC_VKD_ERR_BITSTREAM;
            // `den` is affine in (cx, cy), so the four picture corners bound
            // the whole picture.  Accumulated in 64 bits and required to fit
            // int32 and to lie in [2^28, 2^30).
            const int32_t ox = (int32_t)(si.width >> 1);
            const int32_t oy = (int32_t)(si.height >> 1);
            const int32_t cxs[2] = {-ox, (int32_t)si.width - ox};
            const int32_t cys[2] = {-oy, (int32_t)si.height - oy};
            for (int a = 0; a < 2; ++a)
                for (int b = 0; b < 2; ++b) {
                    const int64_t den = (int64_t)fp.warp[eye].h[6] * cxs[a] +
                                        (int64_t)fp.warp[eye].h[7] * cys[b] +
                                        (int64_t)fp.warp[eye].h[8];
                    if (den < (int64_t)nxvw::kWarpDenMin ||
                        den >= (int64_t)nxvw::kWarpDenMax)
                        return NXVC_VKD_ERR_BITSTREAM;
                }
        }
        if (!wr.ok) return NXVC_VKD_ERR_TRUNCATED;
        off += need;
    }
    const uint8_t *custom = nullptr;
    if (fp.quant_matrix == 255) {
        if (off + 128 > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
        custom = buf + off;
        off += 128;
    }
    resolve_matrices(fp.quant_matrix, custom, fp.weights);

    build_default_tables(fp.cum, fp.nctx);
    // [SYN] 9.4.  Without TAB_V2 every transmitted set is exactly
    // `nctx * 16 * 5` bits -- a whole number of bytes, 120 under the v1
    // context model, 160 under CTX_V2 and 270 under CTX_V3.  With TAB_V2 a
    // set is VARIABLE length, because each context is preceded by a
    // `row_coded` flag and a row nobody gained by training is left at the
    // built-in default; so all the transmitted sets are read as one bit
    // sequence, zero-padded to a byte boundary once at the end.
    //
    // Reading them through one BitR either way is what keeps the two forms
    // one piece of code: without TAB_V2 the reader simply lands on a byte
    // boundary after every set on its own.  r16 is a CTX_V2 stream whose set
    // runs past the frame.
    if (tables_present) {
        BitR bitr{buf + off, frame_bytes - off, 0};
        for (int k = 0; k < 8; ++k) {
            if (!(tables_present & (1u << k))) continue;
            if (!parse_table_set(bitr, k, fp.nctx, fp.tab_v2,
                                 &fp.cum[(size_t)k * table_stride(fp.nctx) *
                                         kNumSym]))
                return NXVC_VKD_ERR_BITSTREAM;
        }
        if (bitr.bit > (frame_bytes - off) * 8) return NXVC_VKD_ERR_TRUNCATED;
        off += (bitr.bit + 7) / 8;
    }

    // --- geometry ------------------------------------------------------
    const bool chroma420 = si.chroma == 0;
    const int nplanes = si.nplanes();
    fp.frame_nplanes = (uint32_t)nplanes;
    fp.coef_stride =
        (uint32_t)nxvw::nxvw_coef_stride_i16(chroma420 ? 1 : 0, si.alpha ? 1 : 0);
    fp.cbf_words = kCbfWordsPerTile;
    fp.tools = (fp.nctx >= kNumCtxV2 ? kToolFlagCtxV2 : 0u) |
               (fp.intra_dir ? kToolFlagIntraDir : 0u) |
               (fp.sdh ? kToolFlagSignHide : 0u) |
               (fp.split4 ? kToolFlagSplit4 : 0u) |
               (fp.cfl ? kToolFlagCfl : 0u) |
               (fp.nctx >= kNumCtxV3 ? kToolFlagCtxV3 : 0u) |
               (fp.xform_large ? kToolFlagXformLarge : 0u);

    const uint32_t ntiles = si.tile_count;
    fp.recs.assign(ntiles, NxvwTileRec{0, 0, 0, 0xffffffffu});
    fp.warp_tiles.assign(ntiles, nxvw::NxvwWarpTile{});
    // [SYN] 13.5: the whole prediction state is cleared when `tile_map_reset`
    // is set.  Annex D D-9.
    if (ic && (flags & 1u))
        for (auto &ps : ic->state) ps = PredState{};

    // Per lane count, the descriptors of the tiles that use it.  nsub_log2 is
    // 0..5, so at most six Pass A dispatches; the usual frame has one.
    std::vector<TileDesc> by_lane[6];
    std::vector<uint32_t> lane_tile[6];

    // [inter] Resolve `ref_sel` against the ring: the slot index, or
    // 0xffffffff when this decoder does not hold the frame the tile names.
    // [REF] codec_impl.inc `ref_for`.
    auto ref_slot_of = [&](int ref_sel) -> uint32_t {
        const int s = ic->ring.resolve(fp.frame_number, ref_sel);
        return s < 0 ? 0xffffffffu : (uint32_t)s;
    };
    auto pack_ns = [](const int8_t *c) -> uint32_t {
        return (uint32_t)(uint8_t)c[0] | ((uint32_t)(uint8_t)c[1] << 8) |
               ((uint32_t)(uint8_t)c[2] << 16);
    };
    // One Pass W record.  `qmv` and `ns` are null when the tile carries no
    // quadrant vectors and no near-skip correction.
    auto emit_warp = [&](uint32_t t, uint32_t eye, uint32_t col, uint32_t rw,
                         int mode, int mv_x, int mv_y, uint32_t refslot,
                         int res_level, int chroma444, int alpha_mode,
                         const uint8_t *qmv, const int8_t (*ns)[3], int qp) {
        nxvw::NxvwWarpTile &wt = fp.warp_tiles[t];
        wt.w0 = (uint32_t)mode | (1u << 3) | (eye << 4) |
                ((uint32_t)res_level << 5) | ((uint32_t)chroma444 << 7) |
                ((uint32_t)alpha_mode << 8) | ((qmv ? 1u : 0u) << 10) |
                ((ns ? 1u : 0u) << 11);
        wt.tx = (int)col;
        wt.ty = (int)rw;
        wt.mvx = mv_x;
        wt.mvy = mv_y;
        wt.quad = qmv ? ((uint32_t)qmv[0] | ((uint32_t)qmv[1] << 8) |
                         ((uint32_t)qmv[2] << 16) | ((uint32_t)qmv[3] << 24))
                      : 0u;
        wt.refBase = refslot;
        wt.qp = qp;
        wt.ns0 = ns ? pack_ns(ns[0]) : 0u;
        wt.ns1 = ns ? pack_ns(ns[1]) : 0u;
        wt.ns2 = ns ? pack_ns(ns[2]) : 0u;
        fp.any_inter = true;
        if (mode == kModeStereo) fp.any_stereo_tile = true;
    };

    // --- tile rows -----------------------------------------------------
    // [SYN] 3.3: a frame contains `eyes * rows` tile-row structures, ordered
    // row-major, eye-minor.  The eye is POSITIONAL and is not a field of the
    // row header.  That order is what puts the whole left-eye row ahead of
    // the right-eye row of the same index, which is what makes a STEREO
    // tile's dependency satisfiable.
    for (uint32_t row = 0; row < si.tiles_y; ++row) {
      for (uint32_t eyeR = 0; eyeR < si.eyes; ++eyeR) {
        if (off + kTileRowHeaderBytes > frame_bytes)
            return NXVC_VKD_ERR_TRUNCATED;
        BR rb{buf, frame_bytes, off, true};
        uint32_t fn = rb.u16v();
        uint32_t ri = rb.u8v();
        uint32_t tc8 = rb.u8v();
        uint64_t skip = rb.u64v();
        if (!rb.ok) return NXVC_VKD_ERR_TRUNCATED;
        if (fn != fp.frame_number || ri != row) return NXVC_VKD_ERR_BITSTREAM;
        // [SYN] 3.3: bit 7 of the byte is `dc_present`, the count is bits 6:0.
        const uint32_t dc_present = tc8 >> 7;
        const uint32_t tcount = tc8 & 0x7fu;
        if (dc_present && !fp.near_skip_tool) return NXVC_VKD_ERR_BITSTREAM;
        // [REF] the skip bitmap covers one tile row of one eye, so the bits
        // above cols_per_eye must be zero (r08).
        if (si.tiles_x < 64 && (skip >> si.tiles_x) != 0)
            return NXVC_VKD_ERR_BITSTREAM;
        uint32_t nskip = 0;
        for (uint32_t i = 0; i < si.tiles_x; ++i) nskip += (skip >> i) & 1u;
        // [REF] a skip references a frame a stream without the INTER tool bit
        // cannot have, which makes it a malformed stream rather than an
        // unimplemented one.  NXVC_VKD_FLAG_ALLOW_SKIPPED_TILES is the escape
        // hatch that predates the inter path: it emits a WARP_SKIP record over
        // a zeroed coefficient slot, which is deterministic and is exactly
        // what the predictor now fills in.
        if (nskip && !fp.inter && !allow_skipped) return NXVC_VKD_ERR_BITSTREAM;
        if (nskip && fp.inter && !fp.warp_present) return NXVC_VKD_ERR_BITSTREAM;
        if (tcount != si.tiles_x - nskip) return NXVC_VKD_ERR_BITSTREAM;
        // [SYN] 3.3: `dc_bitmap` follows the skip bitmap, then one nine-byte
        // correction per set bit in ascending column order, before the first
        // tile structure.  Each constraint below is BITSTREAM (r42, r43).
        uint64_t dcmap = 0;
        if (dc_present) {
            dcmap = rb.u64v();
            if (!rb.ok) return NXVC_VKD_ERR_TRUNCATED;
            // An all-zero bitmap would be two encodings of one stream.
            if (dcmap == 0) return NXVC_VKD_ERR_BITSTREAM;
            if (si.tiles_x < 64 && (dcmap >> si.tiles_x) != 0)
                return NXVC_VKD_ERR_BITSTREAM;
            // Every corrected tile is a skipped tile: the correction replaces
            // a skipped tile's flat mean field and there is nothing for it to
            // correct on a coded one.
            if (dcmap & ~skip) return NXVC_VKD_ERR_BITSTREAM;
        }
        off = rb.i;
        int8_t dcrec[64][nxvw::kNearSkipPlanes][3] = {};
        for (uint32_t c = 0; c < si.tiles_x; ++c) {
            if (!((dcmap >> c) & 1u)) continue;
            if (off + (size_t)nxvw::kNearSkipBytes > frame_bytes)
                return NXVC_VKD_ERR_TRUNCATED;
            for (int q = 0; q < nxvw::kNearSkipPlanes; ++q)
                for (int j = 0; j < 3; ++j)
                    dcrec[c][q][j] = (int8_t)buf[off + 3 * q + j];
            off += (size_t)nxvw::kNearSkipBytes;
        }

        for (uint32_t k = 0; k < si.tiles_x; ++k) {
            const uint32_t tindex = row * si.cols + eyeR * si.tiles_x + k;
            const bool lost = ic && fp.inter && ic->is_missing(tindex);
            // A skipped or concealed tile derives every parameter rather than
            // coding it: res_level 0, the stream's own chroma, alpha_mode 0,
            // ref_sel 0, and the vector is the tile's stored `last_mv`
            // ([SYN] 3.3 and 13.6).  [REF] reconstruct_skip_tile().
            const uint32_t derived_c444 = (si.chroma == 1) ? 1u : 0u;
            auto conceal_tile = [&](bool near_ok) {
                fp.recs[tindex].w0 = ((k & 0xfffu) << 4) | (eyeR << 2);
                fp.recs[tindex].w1 = derived_c444 << 5;
                fp.recs[tindex].w2 = 255u;   // alpha_value 255, present 0
                fp.zero_tiles.push_back(tindex);
                if (!fp.inter) return;
                const PredState &ps = ic->state[tindex];
                // [SYN] 13.6: a tile the client did NOT receive is concealed
                // WITHOUT its correction.  The correction travelled in a row
                // header the transport does not replicate, and applying it
                // would make the decoder's picture depend on bytes it may
                // never have seen -- exactly the divergence the shadow
                // contract exists to prevent.
                const bool use_ns = near_ok && !lost;
                emit_warp(tindex, eyeR, k, row, kModeWarpSkip, ps.last_mv_x,
                          ps.last_mv_y, ref_slot_of(0), 0, (int)derived_c444, 0,
                          nullptr, use_ns ? dcrec[k] : nullptr,
                          iclamp((int)fp.base_qp, 0, 63));
            };
            if ((skip >> k) & 1) {
                // WARP_SKIP: no header, no payload, no coefficients.
                conceal_tile(((dcmap >> k) & 1u) != 0);
                ++fp.tiles_skipped;
                // Losing a skipped tile is a no-op -- concealment IS the
                // WARP_SKIP predictor with the same vector -- but it is still
                // counted, so a caller can account for every tile it marked.
                if (lost) ++fp.tiles_concealed;
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
            const uint32_t qp_delta_raw = (w1 >> 8) & 0x3fu;
            const int qp_delta =
                (int)(qp_delta_raw >= 32 ? (int)qp_delta_raw - 64
                                         : (int)qp_delta_raw);
            const uint32_t nsub_log2 = (w1 >> 17) & 7u;
            const uint32_t mv_present = (w1 >> 20) & 1u;
            const uint32_t ref_sel = (w1 >> 21) & 3u;
            const uint32_t tskip = (w1 >> 23) & 1u;
            const uint32_t wm_id = (w1 >> 26) & 3u;
            const uint32_t split4x4 = (w1 >> 28) & 1u;
            const uint32_t xform_size = (w1 >> 29) & 3u;
            const uint32_t quad_mv = (w1 >> 31) & 1u;

            // Exactly the reference's checks, in the reference's order.
            // Word1 has no reserved bits left: 28 is `split4x4`, 29-30
            // `xform_size` and 31 `quad_mv`, so the reserved-bit vector r09
            // moved to word0 bit 3 (docs/TOOLBITS.md 4.1).
            if ((w0 >> 3) & 1) return NXVC_VKD_ERR_BITSTREAM;
            if (layer != 0) return NXVC_VKD_ERR_UNSUPPORTED;
            // Annex D D-3: the `eye` field must agree with the eye the tile's
            // position in the frame derives.
            if (eye != eyeR) return NXVC_VKD_ERR_BITSTREAM;
            if (mode > 4) return NXVC_VKD_ERR_BITSTREAM;         // r22
            if (mode != kModeIntra && !fp.inter)
                return NXVC_VKD_ERR_UNSUPPORTED;
            if (mode_needs_warp((int)mode) && !fp.warp_present)
                return NXVC_VKD_ERR_BITSTREAM;                    // r21
            if (mode == kModeStereo) {
                if (!fp.stereo_tool || eye != 1)
                    return NXVC_VKD_ERR_BITSTREAM;                // r28
                if (!mv_present) return NXVC_VKD_ERR_BITSTREAM;
            }
            // Annex D D-12: ref_sel 3 is reserved; INTRA and STEREO must
            // carry 0 and the decoding process ignores it (r23, r24).
            if (ref_sel == 3) return NXVC_VKD_ERR_BITSTREAM;
            if ((mode == kModeIntra || mode == kModeStereo) && ref_sel != 0)
                return NXVC_VKD_ERR_BITSTREAM;
            if (res_level > 2) return NXVC_VKD_ERR_BITSTREAM;
            if (alpha_mode == 3) return NXVC_VKD_ERR_BITSTREAM;
            if (nsub_log2 > 5) return NXVC_VKD_ERR_BITSTREAM;
            if (nsub_log2 != 3 && !(si.tools & kToolNsubVar))
                return NXVC_VKD_ERR_BITSTREAM;
            if (res_level != 0 && !(si.tools & kToolResLevel))
                return NXVC_VKD_ERR_BITSTREAM;
            if (tskip && !(si.tools & kToolTransformSkip))
                return NXVC_VKD_ERR_BITSTREAM;
            if (wm_id != 0 && !(si.tools & kToolWmId))
                return NXVC_VKD_ERR_BITSTREAM;
            // [minor 6] docs/TOOLBITS.md 4.2 / SYNTAX.md 4.1: the split flag
            // needs its tool, and is mutually exclusive with transform skip,
            // whose 64 coded values are samples in raster order and have no
            // sub-block structure.
            if (split4x4 && !(si.tools & kToolSplit4))
                return NXVC_VKD_ERR_BITSTREAM;
            if (split4x4 && tskip) return NXVC_VKD_ERR_BITSTREAM;
            // [SYN] 4.1 / 6.7: xform_size 3 is reserved; a nonzero value needs
            // tool bit 27 and is mutually exclusive with transform skip; and
            // the 4x4 split is a subdivision OF the 8x8 transform, so it is
            // meaningful only at xform_size == 0 (docs/TOOLBITS.md 4.2).
            if (xform_size == 3) return NXVC_VKD_ERR_BITSTREAM;
            if (xform_size != 0 && !(si.tools & kToolXformLarge))
                return NXVC_VKD_ERR_BITSTREAM;
            if (xform_size != 0 && tskip) return NXVC_VKD_ERR_BITSTREAM;
            if (split4x4 && xform_size != 0) return NXVC_VKD_ERR_BITSTREAM;
            // A frame that carries its own matrices leaves no room for a
            // built-in override: the two would silently disagree.
            if (wm_id != 0 && fp.quant_matrix == 255)
                return NXVC_VKD_ERR_BITSTREAM;
            if (!chroma444 && si.chroma == 1 &&
                !(si.tools & kToolPerTileChroma))
                return NXVC_VKD_ERR_BITSTREAM;
            if (chroma444 && si.chroma != 1) return NXVC_VKD_ERR_BITSTREAM;
            if (alpha_mode != 0 && !si.alpha) return NXVC_VKD_ERR_BITSTREAM;
            if (tile_index != k) return NXVC_VKD_ERR_BITSTREAM;
            // --- syntax v1.6: QUAD_MV ([SYN] 13.10).  NEAR_SKIP is not a
            // tile-header bit at all: its record and its bitmap are in the
            // tile-ROW header and were validated there (r40, r41).
            if (quad_mv && !fp.quad_tool) return NXVC_VKD_ERR_BITSTREAM;
            if (quad_mv && !(mode == kModeWarpMv || mode == kModeStaticMv))
                return NXVC_VKD_ERR_BITSTREAM;

            const size_t hdr_off = off;
            off = tb.i;
            uint32_t alpha_value = 255;
            int mv_x = 0, mv_y = 0;
            uint32_t disparity = 0;
            uint8_t qmv[4] = {};
            if (mv_present) {
                if (off + 2 > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
                if (mode == kModeStereo) {
                    disparity = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8);
                    // Annex D D-4: bits 15:12 are reserved (r29).
                    if (disparity & 0xf000u) return NXVC_VKD_ERR_BITSTREAM;
                } else {
                    mv_x = (int)(int8_t)buf[off];
                    mv_y = (int)(int8_t)buf[off + 1];
                }
                off += 2;
            }
            if (quad_mv) {
                if (off + 4 > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
                for (int q = 0; q < 4; ++q) qmv[q] = buf[off + q];
                off += 4;
            }
            if (alpha_mode == 1) {
                if (off + 1 > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
                alpha_value = buf[off];
                off += 1;
            }
            if (off + payload_len > frame_bytes) return NXVC_VKD_ERR_TRUNCATED;
            off += payload_len;

            // [inter] A tile the client did not receive: the bytes are parsed
            // -- so the frame stays self-delimiting -- and then discarded,
            // because the client does not hold them.  [SYN] 13.6 runs
            // instead, with `last_mv`, and the prediction state does not
            // advance.  [REF] codec_impl.inc, the `tile_lost(t)` branch.
            if (lost) {
                conceal_tile(false);
                ++fp.tiles_concealed;
                ++fp.tiles_skipped;
                continue;
            }

            // [inter] The reference this tile predicts from.  A STEREO tile
            // reads THIS frame's slot -- the eye-0 sub-picture Pass B is
            // filling in -- and every other inter mode reads the slot
            // `ref_sel` names, whose absence is a malformed stream rather
            // than something to conceal.
            uint32_t refslot = 0xffffffffu;
            if (mode != kModeIntra) {
                refslot = (mode == kModeStereo) ? fp.cur_slot
                                                : ref_slot_of((int)ref_sel);
                if (refslot == 0xffffffffu) return NXVC_VKD_ERR_BITSTREAM;
                emit_warp(tindex, eyeR, k, row, (int)mode,
                          mode == kModeStereo ? (int)disparity : mv_x,
                          mode == kModeStereo ? 0 : mv_y, refslot,
                          (int)res_level, (int)chroma444, (int)alpha_mode,
                          quad_mv ? qmv : nullptr, nullptr,
                          iclamp((int)fp.base_qp + qp_delta, 0, 63));
            }
            if (ic)
                update_pred_state(ic->state[tindex], (int)mode, mv_x, mv_y,
                                  (int)disparity);

            TileDesc d{};
            d.bits_offset = (uint32_t)hdr_off;
            d.bits_length = (uint32_t)(off - hdr_off);
            d.coef_offset = tindex * fp.coef_stride;
            d.cbf_offset = tindex * fp.cbf_words;
            d.mode_offset = tindex * kModeRegionUints;
            d.unit_len_offset = tindex * kUnitLenWordsPerTile;
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
    }
    if (off != frame_bytes) return NXVC_VKD_ERR_BITSTREAM;
    // [inter] The slot this frame writes now holds this frame.  A frame
    // overwrites the slot its own number names ([SYN] 13.2), so the slot's
    // previous contents are gone whether or not every tile of it was coded.
    if (ic && fp.inter) {
        ic->ring.valid[fp.cur_slot] = 1;
        ic->ring.frame_number[fp.cur_slot] = (uint16_t)fp.frame_number;
    }

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
    // [inter] The eye pair is one raster of 64-pixel columns: `tilesX` is the
    // transport's `cols` and `imageW` spans both eyes.  parse_stream_header()
    // refuses `eyes == 2` with a width that is not a multiple of 64, which is
    // exactly the condition under which that merge is exact.
    p.imageW = (int)(si.width * si.eyes);
    p.imageH = (int)si.height;
    p.tilesX = (int)si.cols;
    p.baseQp = (int)fp.base_qp;
    p.chromaQpOff = fp.chroma_qp_off;
    p.alphaQpOff = fp.alpha_qp_off;
    p.coefStrideI16 = (int)fp.coef_stride;
    p.colorTransform = (int)si.color_transform;
    p.chroma420 = chroma420 ? 1 : 0;
    p.alphaPresent = si.alpha ? 1 : 0;
    p.intraDir = fp.intra_dir;
    p.dirLayer = fp.dir_layer;
    // [sparse] The caller's choice; parse_frame() has no opinion.  Set after
    // the call by the decoder, which owns the flag.
    p.sparse = 1;
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
