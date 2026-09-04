// Host-side container parse for the NX Warp Vulkan decoder.
//
// Everything above the entropy-coded tile payload is parsed here, on the CPU:
// the 64-byte stream header and its TLV area, the 40-byte frame header with
// its optional quantization matrices and probability-table deltas, the
// 12-byte tile-row headers with their skip bitmaps, and the 8-byte tile
// headers.  What comes out is exactly the two descriptions the kernels need:
//
//   * Pass A's per-tile descriptor (byte offset and length of the tile inside
//     the frame, plus the destination coefficient / CBF offsets), grouped by
//     the tile's rANS lane count so one dispatch covers one lane count, and
//   * Pass B's NxvwTileRec array in raster order, plus its push constants.
//
// Nothing in here touches Vulkan, which is what lets the parser be unit
// tested on its own and compiled for a platform with no loader.
//
// NORMATIVE SOURCE: docs/SYNTAX.md sections 2, 3, 4 and 9.4, and
// ref/src/codec_impl.inc (nxvc_decoder_parse_stream_header,
// parse_frame_header, nxvc_decoder_decode_frame) which is the executable
// specification.  Where this file and ref/ disagree, ref/ wins.
#ifndef NXVC_VKDEC_PARSE_H
#define NXVC_VKDEC_PARSE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nxvc/nxvc_vk.h"
#include "passA/passA_model.h"
#include "passB/passB_layout.h"

namespace nxvcvk {

using nxwarp_passA::TileDesc;
using nxvw::NxvwPassBPush;
using nxvw::NxvwTileRec;

// ------------------------------------------------------------------ stream
struct StreamInfo {
    uint32_t magic = 0, version = 0, profile = 0, level = 0, tile_size = 0;
    uint32_t width = 0, height = 0, eyes = 0, bit_depth = 0, num_layers = 0;
    uint32_t chroma = 0, color_transform = 0, color_space = 0, alpha = 0;
    uint64_t tools = 0;
    uint32_t ext_len = 0;
    // Derived, [REF] derive_geometry().
    uint32_t tiles_x = 0, tiles_y = 0, tile_count = 0;
    uint32_t cw = 0, ch = 0;   // chroma plane dimensions
    int nplanes() const { return alpha ? 4 : 3; }
};

nxvc_vkd_status parse_stream_header(const uint8_t *buf, size_t len,
                                    StreamInfo &out, size_t *consumed);

// ------------------------------------------------------------------- frame
// One Pass A dispatch: the tiles of `lanes` rANS lanes each.  `first` is the
// index of the group's first descriptor and is a multiple of
// nxs_tiles_per_group(lanes), so the dispatch can be issued with
// vkCmdDispatchBase and needs no extra push constant.  `limit` is the value
// Pass A's `num_tiles` push constant takes, so descriptor slots past the
// group's own tiles are inert.
struct LaneGroup {
    uint32_t lanes = 8;
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t limit = 0;   // first + count
    uint32_t groups = 0;  // workgroups to dispatch
};

struct FrameParse {
    uint32_t frame_number = 0;
    uint32_t base_qp = 0;
    int32_t chroma_qp_off = 0, alpha_qp_off = 0;
    uint32_t quant_matrix = 0;
    uint32_t frame_bytes = 0;
    // [v3] Frame-uniform tool state, derived from the stream's tool bits and
    // the frame header's flags: the coded context count (12 or 16), whether
    // every block carries an intra mode, whether that mode predicts the
    // DC-plane residual (frame flags bit 2), and whether signs are hidden.
    int nctx = 12;
    int intra_dir = 0, dir_layer = 0, sdh = 0;
    uint32_t tools = 0;   // Pass A's `tools` push constant, kToolFlag*

    // Pass A inputs.
    std::vector<TileDesc> desc;      // padded and grouped, see LaneGroup
    std::vector<uint32_t> desc_tile; // desc slot -> tile index, ~0u for pad
    std::vector<LaneGroup> groups;   // one Pass A dispatch each
    std::vector<uint32_t> cum;       // 8 * 16 * 16 cumulative frequencies
    uint32_t frame_nplanes = 3;
    uint32_t coef_stride = 0;        // int16 elements per tile slot
    uint32_t cbf_words = 0;

    // Pass B inputs.
    std::vector<NxvwTileRec> recs;   // tile_count, raster order
    // Four 128-entry sets of 64 luma then 64 chroma weights, Q4.  Set 0 is
    // the frame's pair; sets 1..3 are the built-in pairs a tile's wm_id
    // selects (docs/SYNTAX.md 4.1, tool bit WM_ID).
    int32_t weights[512] = {};
    NxvwPassBPush push{};

    // Coefficient slots that carry no coded data and must be zeroed by the
    // host (skipped tiles get no Pass A descriptor).  Tile indices.
    std::vector<uint32_t> zero_tiles;

    // Reporting.
    uint32_t tiles_skipped = 0, tiles_tskip = 0;
    uint64_t payload_bytes = 0;
    bool any_alpha_coded = false;    // a tile has alpha_mode == 2
};

// Parse one frame unit.  `allow_skipped` mirrors
// NXVC_VKD_FLAG_ALLOW_SKIPPED_TILES: without it a non-zero row skip bitmap is
// refused exactly as ref/ refuses it (NXVC_ERR_UNSUPPORTED, "no references in
// v1").
nxvc_vkd_status parse_frame(const StreamInfo &si, const uint8_t *buf,
                            size_t len, bool allow_skipped, FrameParse &out);

// Probability tables, exposed so the conformance test can diff them against
// ref/src/tables.cpp.  `cum` is filled with 8 * 16 * 16 entries; cum[16] is
// implicitly 1024 and is not stored, which is the layout Pass A's binding 2
// expects.  `nctx` is 12 or 16 and selects the built-in family; contexts past
// it are filled from context 0 and never selected, as ref's
// build_default_set() does.
void build_default_tables(std::vector<uint32_t> &cum, int nctx = 12);
// docs/SYNTAX.md 9.4: nctx x 16 five-bit log-domain deltas, so 120 bytes for
// the 12-context model and 160 for the 16-context one.
bool parse_table_set(const uint8_t *bits, int set_index, int nctx,
                     uint32_t *cum_of_set);

// [REF] resolve_matrices(): 64 luma weights then 64 chroma weights, Q4.
// out512[0..127] is the frame's luma/chroma pair; out512[k*128 ..] for
// k = 1..3 is the built-in pair a tile with wm_id == k uses.
void resolve_matrices(uint32_t quant_matrix, const uint8_t *custom128,
                      int32_t out512[512]);

}  // namespace nxvcvk

#endif  // NXVC_VKDEC_PARSE_H
