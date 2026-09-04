// The decoder integration point.
//
// ===========================================================================
//  THIS IS THE SEAM THE REAL VULKAN DECODER PLUGS INTO
// ===========================================================================
// vk/ is being written in parallel and is not available yet. This header is the
// contract the client shell commits to, chosen to match PAPER 3.2 exactly so
// that the real decoder is a drop-in:
//
//   PAPER 3.2.1  "Two dispatches, not one"  -> record_pass_a + record_pass_b
//   PAPER 3.2.2  Pass A: interleaved rANS with a shared read pointer,
//                workgroup size 64                    -> kPassAWorkgroup
//   PAPER 3.2.3  Pass B: one workgroup per 64x64 tile,
//                workgroup size 256                   -> kPassBWorkgroup
//   PAPER 3.2.6  same SPIR-V everywhere, specialization constants for subgroup
//                size; refuse subgroups smaller than 8 -> DecoderCreateInfo
//   PAPER 4.1    the CPU writes plaintext straight into a host-visible ring
//                buffer the decode dispatch reads      -> DecodeSubmit::bitstream
//
// The signature is deliberately "SSBO of tile runs in, storage image out". The
// client owns the buffers and the command buffer; the decoder only records.
//
// The placeholder implementation in nxc_decoder_placeholder.cpp records nothing
// for Pass A and a trivial "paint each tile by its state" compute shader for
// Pass B, which is enough to exercise the whole ring -> deadline -> present path
// end to end before any entropy decoding exists.
#pragma once

#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

#include "nxc_config.h"

namespace nxc {

// PAPER 3.2.2 / 3.2.3.
inline constexpr uint32_t kPassAWorkgroup = 64;
inline constexpr uint32_t kPassBWorkgroup = 256;
// PAPER 3.2.6: "refuse subgroups smaller than 8 (Mali Bifrost at 4 is
// unsupported for the pure-compute path; it gets hybrid)".
inline constexpr uint32_t kMinSubgroupSize = 8;

// One element of the tile-run SSBO that both passes consume. 16 bytes, so a
// full frame's worth is 2312 * 16 = 37 KB.
struct TileRunGpu {
    uint32_t tile_index;    // linear index, row * cols + col (TRANSPORT.md 1)
    uint32_t byte_offset;   // offset of this tile's bitstream in the bitstream buffer
    uint32_t dir_word;      // the packed 4-byte directory entry (TRANSPORT.md 3.1):
                            // len, qp, mode, res_level, lossless, chroma444, alpha
    uint32_t meta;          // the packed per-tile metadata word (TRANSPORT.md 7.3):
                            // pose_seq, age, state, late, recovered
};

struct DecoderCreateInfo {
    VkDevice          device          = VK_NULL_HANDLE;
    VkPhysicalDevice  physical_device = VK_NULL_HANDLE;
    StreamConfig      stream;
    // Queried by the client at device creation and passed through as a
    // specialization constant (PAPER 3.2.6). 0 means "not queried".
    uint32_t          subgroup_size   = 0;

    // The decoder's output is 2-plane 4:2:0 YCbCr, matching what the WiVRn NX
    // client already samples out of MediaCodec (PAPER 3.5): a full-resolution
    // luma plane and a half-resolution plane of interleaved Cb,Cr. Writing RGBA
    // here would mean a conversion on the way out of the decoder and a second
    // one on the way into WiVRn's compositor path, and would not match the
    // hybrid mode's base layer at all.
    //
    // These are the formats of the images the client allocates and hands over in
    // DecodeSubmit. Both are written as storage images, which needs
    // shaderStorageImageExtendedFormats; the client verifies that at init.
    VkFormat          luma_format     = VK_FORMAT_R8_UNORM;
    VkFormat          chroma_format   = VK_FORMAT_R8G8_UNORM;
};

// Everything one decode of one frame needs. The client fills this and calls the
// two record_ functions into a command buffer it owns; the decoder inserts its
// own barriers *between* its dispatches but not around the whole thing.
struct DecodeSubmit {
    // The host-visible bitstream ring the network thread wrote plaintext into
    // (PAPER 4.1: "writing plaintext straight into a host-visible ring buffer
    // the decoder dispatch reads").
    VkBuffer   bitstream        = VK_NULL_HANDLE;
    VkDeviceSize bitstream_offset = 0;
    VkDeviceSize bitstream_size   = 0;

    // SSBO of TileRunGpu, `tile_run_count` entries.
    VkBuffer   tile_runs        = VK_NULL_HANDLE;
    uint32_t   tile_run_count   = 0;

    // SSBO of per-tile metadata for the current ring slot, one u32 per tile
    // position of the frame (TRANSPORT.md 7.3). Pass B reads it to decide
    // whether a position is decoded, stale or concealed.
    VkBuffer   tile_meta        = VK_NULL_HANDLE;
    uint32_t   tile_meta_count  = 0;

    // The 4-slot reference ring (PAPER 4.3 item 1), two planes per slot. Empty
    // in the placeholder: the real decoder allocates and owns these images, the
    // client only tells it which slot the current frame occupies.
    VkImageView ref_luma[4]     = {VK_NULL_HANDLE, VK_NULL_HANDLE,
                                   VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView ref_chroma[4]   = {VK_NULL_HANDLE, VK_NULL_HANDLE,
                                   VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t    current_slot    = 0;

    // Where the reconstructed frame goes: 2-plane 4:2:0.
    //   output_luma    R8_UNORM,  output_width x output_height
    //   output_chroma  R8G8_UNORM, (output_width / 2) x (output_height / 2),
    //                  Cb in the R component, Cr in G
    VkImageView output_luma     = VK_NULL_HANDLE;
    VkImageView output_chroma   = VK_NULL_HANDLE;
    uint32_t    output_width    = 0;   // luma width
    uint32_t    output_height   = 0;   // luma height

    uint16_t    frame_id        = 0;
    uint16_t    pose_seq        = 0;
};

class Decoder {
public:
    virtual ~Decoder() = default;

    virtual bool create(const DecoderCreateInfo& ci) = 0;
    virtual void destroy() = 0;

    // PAPER 3.2.2. Entropy decode: bitstream -> coefficient SSBO. Workgroup 64,
    // one workgroup per tile, eight interleaved rANS lanes sharing a read
    // pointer (PAPER 6.3).
    virtual void record_pass_a(VkCommandBuffer cb, const DecodeSubmit& s) = 0;

    // PAPER 3.2.3. Reconstruct: one workgroup of 256 per 64x64 tile. Inverse
    // transform, warp prediction from the reference slot, write both output
    // planes and the new reference.
    virtual void record_pass_b(VkCommandBuffer cb, const DecodeSubmit& s) = 0;

    virtual const char* name() const = 0;

    // True once the real decoder is in place; the HUD says which one is running
    // so a screenshot of a measurement is never ambiguous.
    virtual bool is_placeholder() const = 0;
};

// The placeholder: Pass A is a no-op, Pass B paints each tile by its state
// (fresh / stale / concealed / undecodable / empty) into the two output planes.
// See PAPER 4.3, "the HUD heatmap shows age per tile" -- this *is* that heatmap,
// standing in for a picture until there is one. It writes real 4:2:0 YCbCr, so
// the client's present pass and the chroma subsampling are exercised too.
std::unique_ptr<Decoder> create_placeholder_decoder();

}  // namespace nxc
