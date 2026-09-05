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
#include "inter/inter_layout.h"
#include "inter/inter_state.h"
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
    // Derived, [REF] derive_geometry().  `tiles_x` is cols_per_eye and `cw`
    // and `ch` are per-eye chroma dimensions, exactly as [SYN] 3.3 defines
    // them: **a picture is one eye**.  `cols` is the transport's column count
    // over the eye pair, `eyes * tiles_x`, and it is what indexes a tile:
    // `tile = row * cols + eye * tiles_x + tile_index`.
    uint32_t tiles_x = 0, tiles_y = 0, tile_count = 0;
    uint32_t cols = 0;
    uint32_t cw = 0, ch = 0;   // chroma plane dimensions, PER EYE
    int nplanes() const { return alpha ? 4 : 3; }
};

// `tools_mask` is the set of tool bits THIS decoder will accept, which is not
// always the set it implements -- see tools_supported_for().  Zero means "use
// the build-wide mask", which is what every caller without a device wants.
nxvc_vkd_status parse_stream_header(const uint8_t *buf, size_t len,
                                    StreamInfo &out, size_t *consumed,
                                    uint64_t tools_mask = 0);

// The tool bits a decoder running on this device may accept.
//
// It is the build-wide mask minus anything the device cannot be trusted with.
// Today that is exactly one bit, and it is not a performance judgement: on the
// Adreno 650, XFORM_LARGE (27) decodes 16x16 and 32x32 streams wrong, and on
// the 4:4:4 32x32 conformance vector it does not decode them at all -- it
// WEDGES, a fence that never signals, with no kgsl fault and no GPU reset.
// A decoder that advertises a tool it hangs on is not merely slow at it; it
// invites a conformant encoder to send a stream that kills the session.
//
// `vendor_id` is VkPhysicalDeviceProperties::vendorID and `device_name` its
// deviceName.  Both are checked: the vendor id is the reliable half, the name
// is what catches a Qualcomm part behind a translation layer that reports
// someone else's id.
uint64_t tools_supported_for(uint32_t vendor_id, const char *device_name);

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
    // [minor 6] XFORM_4X4_SPLIT (tool bit 19) and INTRA_CFL (tool bit 24).
    int split4 = 0, cfl = 0;
    // [minor 6] TAB_V2 (tool bit 26): a transmitted table set is variable
    // length, each context preceded by a `row_coded` flag.
    int tab_v2 = 0;
    // [minor 6] XFORM_LARGE (tool bit 27): tiles may set xform_size != 0.
    int xform_large = 0;
    // [minor 6] The width of one table set in the `cum` upload, and Pass A's
    // specialisation constant 4: 16 under the v1/v2 models, 27 under CTX_V3.
    int ctx_stride = 16;
    uint32_t tools = 0;   // Pass A's `tools` push constant, kToolFlag*
    // [entropy-lite] Stream tool bit 30, docs/TOOLBITS.md 8.  Frame-uniform:
    // it selects the ENTROPY_MODE Pass A is specialised on, and with it the
    // dispatch shape -- ONE workgroup per tile, no lane grouping, no
    // probability tables.
    bool entropy_lite = false;

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

    // ------------------------------------------------- Phase 2 ([SYN] 13)
    // Frame-uniform inter state, from the stream's tool bits and the frame
    // header's flags and `ref_slots`.
    int inter = 0;          // stream tool bit 10
    int warp_tool = 0;      // stream tool bit 11
    int stereo_tool = 0;    // stream tool bit 12
    int near_skip_tool = 0; // stream tool bit 28
    int quad_tool = 0;      // stream tool bit 29
    int warp_present = 0;   // frame flags bit 3
    uint32_t ref_slots = 0, flags = 0;
    WarpMatrix warp[2];     // warp_ext(), one 36-byte record per eye
    // One Pass W record per tile, raster order over the eye pair.  `refBase`
    // still holds the ring SLOT INDEX (0..3, or 0xffffffff for "this decoder
    // holds no usable reference"); the runtime multiplies it by the slot
    // stride once, when it uploads the buffer, because the stride is a
    // property of the allocation rather than of the bitstream.
    std::vector<nxvw::NxvwWarpTile> warp_tiles;
    uint32_t cur_slot = 0;  // the slot this frame writes, frame_number mod 4
    bool any_inter = false;        // some tile needs Pass W at all
    bool any_stereo_tile = false;  // ... and Pass W / Pass B must run per eye

    // Reporting.
    uint32_t tiles_skipped = 0, tiles_tskip = 0;
    uint32_t tiles_concealed = 0;   // tiles nxvc_vk_decoder_mark_missing named
    uint64_t payload_bytes = 0;
    bool any_alpha_coded = false;    // a tile has alpha_mode == 2
};

// Parse one frame unit.  `allow_skipped` mirrors
// NXVC_VKD_FLAG_ALLOW_SKIPPED_TILES: on a stream with no INTER tool bit a
// non-zero row skip bitmap is refused without it, exactly as ref/ refuses it.
//
// `ic` is the decoder's inter state ([SYN] 13.5 and 13.2).  It is read AND
// written: the parse resolves `ref_sel` against the ring, substitutes the
// concealment predictor for every tile
// `nxvc_vk_decoder_mark_missing()` named, and advances the per-tile
// prediction state exactly where ref/'s decoder advances it.  Pass NULL for a
// decoder with no inter state, and an inter mode is then refused with
// UNSUPPORTED, which is what the intra-only decoder did.
nxvc_vkd_status parse_frame(const StreamInfo &si, const uint8_t *buf,
                            size_t len, bool allow_skipped, FrameParse &out,
                            InterCtx *ic = nullptr);

// Probability tables, exposed so the conformance test can diff them against
// ref/src/tables.cpp.  `cum` is filled with 8 * 16 * 16 entries; cum[16] is
// implicitly 1024 and is not stored, which is the layout Pass A's binding 2
// The tool bits this decoder implements; the C ABI's
// nxvc_vk_decoder_tools_supported() returns it.
uint64_t tools_supported();

// expects.  `nctx` is 12, 16 or 27 and selects the built-in family; contexts
// past it are filled from context 0 and never selected, as ref's
// build_default_set() does.
void build_default_tables(std::vector<uint32_t> &cum, int nctx = 12);
// parse_table_set() is not declared here: it takes the file-local bit reader,
// and nothing outside nxvc_vkdec_parse.cpp calls it.

// [REF] resolve_matrices(): 64 luma weights then 64 chroma weights, Q4.
// out512[0..127] is the frame's luma/chroma pair; out512[k*128 ..] for
// k = 1..3 is the built-in pair a tile with wm_id == k uses.
void resolve_matrices(uint32_t quant_matrix, const uint8_t *custom128,
                      int32_t out512[512]);

}  // namespace nxvcvk

#endif  // NXVC_VKDEC_PARSE_H
