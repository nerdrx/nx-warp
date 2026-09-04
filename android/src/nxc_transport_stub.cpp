// Stub implementation of IDepacketizer.
//
// See the long comment at the top of nxc_transport.h for exactly which parts of
// docs/TRANSPORT.md this covers and which it does not, and for the three-step
// procedure that replaces this file with the real nxvc_transport library.
//
// The parts that ARE here are implemented against the normative text rather than
// approximated, so that the client's counters, its placement behaviour and its
// feedback packets are already wire-correct and the swap is a link change, not a
// behaviour change.
#include "nxc_transport.h"
#include "nxc_wire.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace nxc {
namespace {

// Direct-mapped duplicate table (TRANSPORT.md 7.2). A collision evicts, which
// can only ever cause a *missed* duplicate detection, never a false one, because
// the full key is compared. Suppression is an optimisation for statistics
// everywhere except FEC accounting, which the stub does not do.
class DupTable {
public:
    static constexpr uint32_t kBits = 14;              // 16384 entries, 128 KB
    static constexpr uint32_t kMask = (1u << kBits) - 1;

    DupTable() : keys_(1u << kBits, 0) {}

    bool seen_or_insert(uint64_t key) {
        // Key 0 is used as "empty"; fold it away rather than special-casing.
        if (key == 0) key = 1;
        uint64_t h = key * 0x9e3779b97f4a7c15ull;
        uint32_t i = uint32_t(h >> (64 - kBits)) & kMask;
        if (keys_[i] == key) return true;
        keys_[i] = key;
        return false;
    }
    void clear() { std::fill(keys_.begin(), keys_.end(), 0ull); }

private:
    std::vector<uint64_t> keys_;
};

class StubDepacketizer final : public IDepacketizer {
public:
    StubDepacketizer(const StreamConfig& cfg, bool touch_payload)
        : cfg_(cfg), touch_payload_(touch_payload) {
        // What a handshake would have negotiated for a stub session. CAP_FEC is
        // deliberately absent: the stub cannot recover a group, so accepting the
        // bit would be a lie (TRANSPORT.md 2.2 makes an unnegotiated bit a drop).
        caps_ = kCapPoseHdr | kCapRleFeedback | kCapJumbo | kCapMultipath;
        dir_.resize(256);
    }

    void set_negotiated_caps(uint8_t caps) override { caps_ = caps; }
    const TransportCounters& counters() const override { return c_; }

    bool submit(const uint8_t* p, size_t len, uint64_t rx_ts_us, ITileSink* sink) override {
        c_.rx_datagrams++;
        c_.rx_bytes += len;

        DatagramHeader h{};
        if (!parse_header(p, len, &h)) { c_.short_datagram++; return false; }

        // ---- TRANSPORT.md 12, in the order the table lists them.
        if (h.version != kVersion) { c_.bad_version++; return false; }
        if (h.caps & ~caps_)       { c_.bad_caps++;    return false; }

        // Per-path loss, measured BEFORE any FEC recovery (8.3).
        const uint32_t pid = h.path_id & 1;
        account_path(pid, h.path_seq);

        // The on-wire datagram is 24 + payload_len + 16 (D12). The self-test
        // trailer, if the blaster appended one, sits after the tag and is not
        // part of the datagram proper.
        const size_t wire_len = size_t(kHeaderBytes) + h.payload_len + kTagBytes;
        if (wire_len > len) { c_.bad_directory++; return false; }

        if (h.is_parity()) {
            // D11: tile_count == 0 marks parity. Without RS decode there is
            // nothing useful to do with it; counted, not an error.
            c_.parity_dropped++;
            return false;
        }

        // ---- AEAD. The real library opens here and a failure is `auth_fail`
        // with no partial placement. The stub's open is the identity function.
        static_assert(kStubAeadIsIdentity, "stub AEAD must be identity");
        const uint8_t* pt = p + kHeaderBytes;
        const uint32_t pt_len = h.payload_len;

        if (touch_payload_) {
            // Not decryption: a read of every payload byte so that the pps
            // self test includes the memory traffic a real AES-256-GCM open
            // would generate. Kept out of the default path so the headline
            // number is the receive path's own ceiling (PAPER 4.11).
            uint32_t acc = 0;
            for (uint32_t i = 0; i < pt_len; ++i) acc += pt[i];
            payload_checksum_sink_ += acc;
        }

        // ---- Payload layout (3): [26 pose hdr] [4*N directory] [bitstreams]
        uint32_t off = 0;
        const uint8_t* pose = nullptr;
        if (h.pose_hdr) {
            if (pt_len < kPoseHdrBytes) { c_.bad_directory++; return false; }
            pose = pt;
            off += kPoseHdrBytes;
        }
        const uint32_t n = h.tile_count;
        if (pt_len < off + n * kDirEntryBytes) { c_.bad_directory++; return false; }

        // ---- Range and homogeneity (3.2, 12).
        if (uint32_t(h.tile_first) + n > cfg_.tiles_per_frame()) { c_.bad_range++; return false; }
        // "a contiguous sequence of tiles from one tile row": the run must not
        // cross a row boundary.
        if ((h.tile_first % cfg_.cols) + n > cfg_.cols) { c_.bad_range++; return false; }

        uint32_t sum = 0;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t w = rd_u32(pt + off + i * kDirEntryBytes);
            dir_[i] = w;
            sum += (w & 0xfff);
        }
        off += n * kDirEntryBytes;

        // "The sum of all len ... plus 4*N plus 26 if pose_hdr MUST equal the
        // plaintext length. A receiver that finds otherwise MUST discard the
        // whole datagram" -- it cannot know where the tile boundaries are.
        if (off + sum != pt_len) { c_.bad_directory++; return false; }

        // ---- Duplicate suppression (7.2), data key.
        const uint64_t key = (uint64_t(h.stream_id) << 56) | (uint64_t(h.frame_id) << 40) |
                             (uint64_t(h.layer_id) << 36) | (uint64_t(h.tile_first) << 20) |
                             (uint64_t(h.frag_idx) << 18);
        if (dup_.seen_or_insert(key)) { c_.duplicate++; return false; }

        // ---- Fragments. 3.4: a tile with any fragment missing is a lost tile;
        // reassembly is the library's job, and the stub does not do it.
        if (h.frag_count != 0) { c_.bad_range++; return false; }

        PlacedRun run{};
        run.frame_id      = h.frame_id;
        run.pose_seq      = h.pose_seq;
        run.tile_first    = h.tile_first;
        run.tile_count    = h.tile_count;
        run.layer_id      = h.layer_id;
        run.tile_class    = h.tile_class;
        run.band          = h.band;
        run.ref_delta     = h.ref_delta;
        run.keyframe      = (h.flags & kFlagKeyframeRun) != 0;
        run.partial_frame = (h.flags & kFlagPartialFrame) != 0;
        run.recovered     = false;
        run.dir           = dir_.data();
        run.bitstream     = pt + off;
        run.bitstream_len = sum;
        run.pose_hdr      = pose;
        run.tx_ts         = h.tx_ts;
        run.enc_us        = h.enc_us;
        run.rx_ts_us      = rx_ts_us;

        c_.placed_runs++;
        c_.placed_tiles += n;
        if (sink) sink->place(run);
        return true;
    }

    // ---------------------------------------------------------------- 8

    size_t build_feedback(const BandFeedbackInput* bands, size_t band_count,
                          const FeedbackFlags& flags,
                          const uint8_t path_loss_q8[2], const uint8_t path_rtt_ms[2],
                          uint8_t* out, size_t cap) override {
        if (band_count == 0 || band_count > 3) return 0;
        size_t o = 0;
        if (cap < 8) return 0;

        // ---- 8.1 header
        uint8_t fl = 0;
        if (flags.deadline_moved) fl |= 1u << 0;
        if (flags.path0_stalled)  fl |= 1u << 1;
        if (flags.path1_stalled)  fl |= 1u << 2;
        if (flags.rekey_req)      fl |= 1u << 3;
        out[0] = uint8_t((kVersion & 0x0f) | (fl << 4));
        out[1] = 0;  // stream_id
        wr_u16(out + 2, bands[0].frame_id);
        out[4] = bands[0].band;
        out[5] = uint8_t(band_count);
        wr_u16(out + 6, bands[0].tiles_in_band);
        o = 8;

        // ---- 8.2 band records, newest first
        for (size_t b = 0; b < band_count; ++b) {
            const BandFeedbackInput& in = bands[b];
            if (o + 20 > cap) return 0;

            // Choose the smallest legal encoding (D9).
            uint8_t mode = 0;  // RAW
            const uint32_t raw_bytes = (in.tiles_in_band + 7u) / 8u;
            std::vector<uint8_t>& rle = rle_scratch_;
            rle.clear();
            const bool have_rle = (caps_ & kCapRleFeedback) != 0;

            uint32_t missing = count_missing(in);
            if (missing == 0) {
                mode = 1;  // ALL
            } else if (have_rle) {
                build_rle(in, &rle);
                // rle is empty when it would have needed more than 255 runs.
                if (!rle.empty() && rle.size() < raw_bytes) mode = 2;
            }

            uint8_t rflags = uint8_t(mode & 0x03);
            if (in.complete)        rflags |= 1u << 2;
            if (in.deadline_missed) rflags |= 1u << 3;

            wr_u16(out + o + 0, in.frame_id);
            out[o + 2] = in.band;
            out[o + 3] = rflags;
            wr_u32(out + o + 4,  in.rx_ts_first);
            wr_u32(out + o + 8,  in.rx_ts_last);
            wr_u16(out + o + 12, in.decode_us);
            wr_u16(out + o + 14, in.conceal_tiles);
            wr_u16(out + o + 16, in.late_tiles);
            out[o + 18] = in.fec_recovered;
            out[o + 19] = in.fec_failed;
            o += 20;

            if (mode == 0) {
                if (o + raw_bytes > cap) return 0;
                uint32_t nb = raw_bytes < in.bitmap_bytes ? raw_bytes : in.bitmap_bytes;
                std::memcpy(out + o, in.received_bitmap, nb);
                if (nb < raw_bytes) std::memset(out + o + nb, 0, raw_bytes - nb);
                o += raw_bytes;
            } else if (mode == 2) {
                if (o + rle.size() > cap) return 0;
                std::memcpy(out + o, rle.data(), rle.size());
                o += rle.size();
            }
            // mode 1 (ALL) writes zero bytes.
        }

        // ---- 8.3 trailer
        if (o + 4 > cap) return 0;
        out[o + 0] = path_loss_q8[0];
        out[o + 1] = path_loss_q8[1];
        out[o + 2] = path_rtt_ms[0];
        out[o + 3] = path_rtt_ms[1];
        o += 4;
        return o;
    }

private:
    static bool bit(const uint8_t* bm, uint32_t i) { return (bm[i >> 3] >> (i & 7)) & 1u; }

    static uint32_t count_missing(const BandFeedbackInput& in) {
        uint32_t m = 0;
        for (uint32_t i = 0; i < in.tiles_in_band; ++i)
            if (!bit(in.received_bitmap, i)) ++m;
        return m;
    }

    // Runs of consecutive MISSING tiles: u8 nruns, then nruns x (u16 start, u8 len).
    static void build_rle(const BandFeedbackInput& in, std::vector<uint8_t>* out) {
        std::vector<uint8_t> body;
        uint32_t runs = 0;
        uint32_t i = 0;
        while (i < in.tiles_in_band) {
            if (bit(in.received_bitmap, i)) { ++i; continue; }
            uint32_t start = i;
            while (i < in.tiles_in_band && !bit(in.received_bitmap, i) && (i - start) < 255) ++i;
            const uint32_t len = i - start;
            if (runs == 255) { out->clear(); return; }  // does not fit, caller uses RAW
            body.push_back(uint8_t(start & 0xff));
            body.push_back(uint8_t(start >> 8));
            body.push_back(uint8_t(len));
            ++runs;
        }
        out->clear();
        out->push_back(uint8_t(runs));
        out->insert(out->end(), body.begin(), body.end());
    }

    void account_path(uint32_t pid, uint16_t seq14) {
        c_.path_rx[pid]++;
        PathState& s = path_[pid];
        if (!s.started) { s.started = true; s.expected = seq14; }
        const uint64_t ext = extend_path_seq(s.expected, seq14);
        if (ext > s.expected) {
            c_.path_lost[pid] += (ext - s.expected);
            s.expected = ext + 1;
        } else if (ext < s.expected) {
            c_.path_reorder[pid]++;
            // Do not rewind `expected`: a reordered datagram is not evidence
            // that everything after it is still in flight.
        } else {
            s.expected = ext + 1;
        }
    }

    struct PathState { bool started = false; uint64_t expected = 0; };

    StreamConfig          cfg_;
    bool                  touch_payload_;
    uint8_t               caps_ = 0;
    TransportCounters     c_;
    DupTable              dup_;
    PathState             path_[2];
    std::vector<uint32_t> dir_;
    std::vector<uint8_t>  rle_scratch_;
    volatile uint64_t     payload_checksum_sink_ = 0;
};

}  // namespace

std::unique_ptr<IDepacketizer> create_stub_depacketizer(const StreamConfig& cfg,
                                                        bool touch_payload) {
    return std::make_unique<StubDepacketizer>(cfg, touch_payload);
}

}  // namespace nxc
