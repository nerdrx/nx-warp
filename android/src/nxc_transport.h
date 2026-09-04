// The transport seam.
//
// ===========================================================================
//  READ THIS BEFORE TOUCHING transport/
// ===========================================================================
// docs/TRANSPORT.md is normative and describes a C++ library, `nxvc_transport`,
// living in transport/. At the time this client shell was written that directory
// did not exist yet, so the client talks to the abstract interfaces below and
// ships a stub implementation (nxc_transport_stub.cpp) that implements the
// subset of TRANSPORT.md the client needs to be measurable:
//
//   implemented by the stub          | TRANSPORT.md
//   ---------------------------------+---------------------------------------
//   24-byte header parse + validation| 2, 12
//   tile directory parse + sum check | 3.1, 12
//   run homogeneity / range checks   | 3.2, 12
//   duplicate suppression            | 7.2
//   per-path loss from path_seq gaps | 8.3
//   feedback packet, all 3 bitmap    | 8.1, 8.2, 8.3
//     encodings (ALL / RLE / RAW)    |
//   ---------------------------------+---------------------------------------
//   NOT implemented by the stub      |
//   AEAD open, HKDF key schedule,    | 4  -- see kStubAeadIsIdentity below
//     nonce extension, replay window |
//   Reed-Solomon FEC recovery        | 6  -- parity datagrams are counted, dropped
//   fragment reassembly              | 3.4
//   multipath scheduling / stall     | 10
//   client shadow / reference model  | 9  -- server side, not the client's job
//
// THE SWAP. When transport/ exists:
//   1. Add `add_subdirectory(../transport ...)` to android/CMakeLists.txt and
//      link `nxvc_transport`.
//   2. Delete nxc_transport_stub.cpp and nxc_wire.h. Everything in nxc_wire.h is
//      a transcription of TRANSPORT.md 2/3/8 and the library owns those types.
//   3. Write a ~100 line nxc_transport_nxt.cpp that implements IDepacketizer by
//      forwarding to `nxt::Receiver`: submit() -> Receiver::on_datagram(),
//      the library's placement callback -> ITileSink::place(), and
//      build_feedback() -> nxt::FeedbackGenerator::generate().
//   4. Supply the AEAD keys: create_stub_depacketizer() grows a key argument, or
//      is replaced by the library's own factory. The client's receive thread is
//      already the right place to do it -- PAPER 4.1 puts decryption on the CPU
//      network thread deliberately.
// Nothing outside these three files knows the wire format, by construction.
#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

#include "nxc_config.h"

namespace nxc {

// The stub's "AEAD open" is the identity function: the blaster sends plaintext.
// A real session MUST NOT use it. Flipped to false by the transport/ backend.
inline constexpr bool kStubAeadIsIdentity = true;

// ---------------------------------------------------------------- placement

// One run of tiles, already authenticated (in the real library) and parsed,
// handed to the frame ring. Pointers are valid only for the duration of the
// place() call; the sink copies what it needs.
struct PlacedRun {
    uint16_t frame_id;
    uint16_t pose_seq;
    uint16_t tile_first;
    uint8_t  tile_count;
    uint8_t  layer_id;
    uint8_t  tile_class;
    uint8_t  band;
    uint8_t  ref_delta;      // 3 = intra
    bool     keyframe;
    bool     partial_frame;
    bool     recovered;      // arrived via FEC (TRANSPORT.md 7.3 bit 27)

    const uint32_t* dir;           // tile_count packed directory words (3.1)
    const uint8_t*  bitstream;     // concatenated tile bitstreams, in tile order
    uint32_t        bitstream_len;
    const uint8_t*  pose_hdr;      // 26 bytes (3.3) or nullptr

    uint32_t tx_ts;                // server clock, us
    uint16_t enc_us;
    uint64_t rx_ts_us;             // client clock, us
};

struct ITileSink {
    virtual ~ITileSink() = default;
    virtual void place(const PlacedRun& run) = 0;
};

// ---------------------------------------------------------------- counters

// TRANSPORT.md section 12, one counter per row, plus receive-path counters the
// library does not own.
struct TransportCounters {
    uint64_t rx_datagrams   = 0;
    uint64_t rx_bytes       = 0;
    uint64_t placed_runs    = 0;
    uint64_t placed_tiles   = 0;

    uint64_t bad_version    = 0;
    uint64_t bad_caps       = 0;
    uint64_t auth_fail      = 0;
    uint64_t replay         = 0;
    uint64_t bad_directory  = 0;
    uint64_t bad_range      = 0;
    uint64_t stale_frame    = 0;
    uint64_t short_datagram = 0;
    uint64_t duplicate      = 0;
    uint64_t parity_dropped = 0;   // stub only: no FEC, parity is discarded
    uint64_t fec_recovered  = 0;
    uint64_t fec_failed     = 0;

    // Per path (NXT_MAX_PATHS = 2), from path_seq gaps, before FEC (8.3).
    uint64_t path_rx[2]     = {0, 0};
    uint64_t path_lost[2]   = {0, 0};
    uint64_t path_reorder[2]= {0, 0};
};

// ---------------------------------------------------------------- feedback

// What the frame ring hands the feedback generator for one band. The generator
// owns the encoding choice (ALL / RLE / RAW, TRANSPORT.md 8.2 and D9).
struct BandFeedbackInput {
    uint16_t frame_id;
    uint8_t  band;
    uint16_t tiles_in_band;
    bool     complete;
    bool     deadline_missed;
    uint32_t rx_ts_first;
    uint32_t rx_ts_last;
    uint16_t decode_us;
    uint16_t conceal_tiles;
    uint16_t late_tiles;
    uint8_t  fec_recovered;
    uint8_t  fec_failed;
    // One bit per tile of the band, LSB first within a byte: set iff the tile
    // was received AND decoded without error (8.2).
    const uint8_t* received_bitmap;
    uint32_t       bitmap_bytes;
};

struct FeedbackFlags {
    bool deadline_moved = false;
    bool path0_stalled  = false;
    bool path1_stalled  = false;
    bool rekey_req      = false;
};

// ---------------------------------------------------------------- interface

struct IDepacketizer {
    virtual ~IDepacketizer() = default;

    // One datagram straight off the wire. Returns true if anything was placed.
    // Never blocks, never allocates (TRANSPORT.md 12, "the library never blocks").
    virtual bool submit(const uint8_t* dgram, size_t len, uint64_t rx_ts_us,
                        ITileSink* sink) = 0;

    // TRANSPORT.md 8: build one feedback packet covering up to 3 band records,
    // newest first. `bands` points at `band_count` inputs, newest at index 0.
    // Returns bytes written, or 0 if it does not fit.
    virtual size_t build_feedback(const BandFeedbackInput* bands, size_t band_count,
                                  const FeedbackFlags& flags,
                                  const uint8_t path_loss_q8[2],
                                  const uint8_t path_rtt_ms[2],
                                  uint8_t* out, size_t out_cap) = 0;

    virtual const TransportCounters& counters() const = 0;
    virtual void set_negotiated_caps(uint8_t caps) = 0;
};

// The stub. `caps` is what a real handshake would have negotiated; the client
// defaults it to CAP_POSE_HDR | CAP_RLE_FEEDBACK so the blaster's datagrams pass
// the 2.2 check.
std::unique_ptr<IDepacketizer> create_stub_depacketizer(const StreamConfig& cfg,
                                                        bool touch_payload);

}  // namespace nxc
