#include "nxvc/transport/sender.h"

#include <algorithm>
#include <cstring>

namespace nxt {

Sender::Sender(const StreamConfig& cfg, const Aead* aead, const Key& session_key,
               const Key& session_salt)
    : cfg_(cfg), aead_(aead), pkt_(cfg), striper_(cfg), sched_(cfg), shadow_(cfg) {
    for (uint8_t p = 0; p < kMaxPaths; ++p) {
        subkey_dn_[p] = derive_subkey(session_key, session_salt, p, Direction::kDownstream);
        subkey_up_[p] = derive_subkey(session_key, session_salt, p, Direction::kUpstream);
    }
}

void Sender::begin_frame(uint16_t frame_id, const PoseHeader& pose,
                         uint32_t render_finish_us, uint32_t frame_bit_budget) {
    frame_ext_ = frame_started_ ? extend_seq16(frame_ext_, frame_id) : frame_id;
    frame_started_ = true;
    frame_id_ = frame_id;
    pose_ = pose;
    render_finish_us_ = render_finish_us;
    shadow_.begin_frame(frame_id);
    sched_.begin_frame(render_finish_us, frame_bit_budget);
}

Datagram Sender::seal_one(PendingDatagram& pd, uint8_t path_id, uint32_t tx_ts) {
    pd.hdr.path_id = path_id;
    uint64_t s = seq_[path_id]++;
    pd.hdr.path_seq = uint16_t(s & 0x3FFF);
    pd.hdr.tx_ts = tx_ts;
    pd.hdr.payload_len = uint16_t(pd.plaintext.size());
    pd.hdr.caps = cfg_.caps;

    Datagram d;
    d.path_id = path_id;
    d.tx_ts = tx_ts;
    d.bytes.resize(kHeaderBytes + pd.plaintext.size() + kTagBytes);
    encode_header(pd.hdr, d.bytes.data());
    Nonce n = derive_nonce(cfg_.stream_id, path_id, epoch_, s);
    aead_->seal(subkey_dn_[path_id], n,
                std::span<const uint8_t>(d.bytes.data(), kHeaderBytes),
                std::span<const uint8_t>(pd.plaintext.data(), pd.plaintext.size()),
                d.bytes.data() + kHeaderBytes);
    return d;
}

std::vector<Datagram> Sender::send_band(uint8_t band, std::span<const TileInput> tiles,
                                        uint32_t encode_finish_us, uint16_t enc_us,
                                        bool last_band) {
    std::vector<Datagram> out;

    FrameContext ctx;
    ctx.frame_id = frame_id_;
    ctx.pose_seq = pose_.pose_seq;
    ctx.tx_ts = encode_finish_us;
    ctx.enc_us = enc_us;
    ctx.last_band = last_band;
    ctx.pose = &pose_;

    std::vector<SendUnit> units;
    Packetizer::Status st = pkt_.packetize_band(band, tiles, ctx, &units);
    stats.oversize_tiles = pkt_.oversize_tiles();
    if (st != Packetizer::Status::kOk && units.empty()) return out;

    size_t band_bytes = 0, class_a_bytes = 0;
    for (const SendUnit& u : units) {
        size_t b = u.bytes();
        band_bytes += b;
        if (u.cls == TileClass::kA) class_a_bytes += b;
    }
    striper_.tick_band(encode_finish_us);
    uint32_t band_period = cfg_.frame_period_us / (cfg_.bands() ? cfg_.bands() : 1);
    striper_.begin_band(band_bytes, class_a_bytes, band_period);

    // Assign paths first so the pacer knows how many datagrams there will be.
    struct Job {
        SendUnit* unit;
        uint8_t path;
    };
    std::vector<Job> jobs;
    size_t ndg = 0;
    for (SendUnit& u : units) {
        std::vector<uint8_t> paths = striper_.assign(u);
        for (uint8_t p : paths) {
            jobs.push_back(Job{&u, p});
            ndg += u.data.size() + size_t(u.m);
        }
        if (paths.size() > 1) stats.duplicated_datagrams += u.data.size() + size_t(u.m);
    }
    BandScheduler::Plan plan = sched_.plan(band, encode_finish_us, ndg);

    size_t idx = 0;
    for (Job& j : jobs) {
        SendUnit& u = *j.unit;
        FecGroupEncoder enc;
        enc.reset(int(u.data.size()), u.m);
        for (PendingDatagram& pd : u.data) {
            PendingDatagram copy = pd;  // path fields differ per copy
            Datagram d = seal_one(copy, j.path, plan.tx_at(idx++));
            if (u.m > 0) enc.add(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()));
            stats.datagrams++;
            stats.data_datagrams++;
            stats.wire_bytes += d.bytes.size();
            stats.header_bytes += kHeaderBytes;
            stats.tag_bytes += kTagBytes;
            stats.dir_bytes += size_t(copy.hdr.tile_count) * kDirEntryBytes;
            if (copy.hdr.pose_hdr) stats.pose_bytes += kPoseHeaderBytes;
            stats.runs++;
            stats.tiles += copy.hdr.tile_count;
            size_t overhead = size_t(copy.hdr.tile_count) * kDirEntryBytes +
                              (copy.hdr.pose_hdr ? kPoseHeaderBytes : 0);
            stats.tile_bytes += copy.plaintext.size() - overhead;
            out.push_back(std::move(d));
        }
        if (u.m > 0) {
            std::vector<ByteVec> parity = enc.finish();
            for (size_t g = 0; g < parity.size(); ++g) {
                PendingDatagram pp;
                pp.parity = true;
                pp.hdr = u.data.front().hdr;
                pp.hdr.tile_count = 0;
                pp.hdr.flags &= uint8_t(~(kFlagLastRunOfFrame | kFlagKeyframeRun));
                pp.hdr.pose_hdr = false;
                pp.hdr.fec_group = u.group;
                pp.hdr.fec_k = uint8_t(u.data.size());
                pp.hdr.fec_idx = uint8_t(u.data.size() + g);
                pp.plaintext = std::move(parity[g]);
                Datagram d = seal_one(pp, j.path, plan.tx_at(idx++));
                stats.datagrams++;
                stats.parity_datagrams++;
                stats.wire_bytes += d.bytes.size();
                stats.parity_bytes += d.bytes.size();
                out.push_back(std::move(d));
            }
        }
    }
    sched_.note_band_bits(uint32_t(band_bytes * 8));
    return out;
}

bool Sender::on_feedback(std::span<const uint8_t> wire, uint8_t path_id,
                         uint64_t now_us) {
    if (path_id >= kMaxPaths || wire.size() < 8 + kTagBytes) return false;
    // AAD is the 8-byte feedback header; the nonce counter is derived from
    // (frame_id, band) extended across wraps (TRANSPORT.md 4.2, 8).
    const uint8_t* p = wire.data();
    uint16_t frame_id = rd16(p + 2);
    uint8_t band = p[4];
    uint8_t nbands = cfg_.bands();
    uint64_t counter = extend_seq16(frame_ext_, frame_id) * nbands + band;
    Nonce n = derive_nonce(cfg_.stream_id, path_id, epoch_, counter);

    ByteVec pt(wire.size() - 8 - kTagBytes + 8);
    size_t got = aead_->open(subkey_up_[path_id], n,
                             std::span<const uint8_t>(p, 8),
                             std::span<const uint8_t>(p + 8, wire.size() - 8),
                             pt.data() + 8);
    if (got == SIZE_MAX) return false;
    std::memcpy(pt.data(), p, 8);
    pt.resize(8 + got);

    FeedbackPacket fb;
    if (!decode_feedback(std::span<const uint8_t>(pt.data(), pt.size()), &fb))
        return false;
    shadow_.apply_feedback(fb);
    shadow_.add_feedback_bytes(wire.size());
    stats.feedback_packets++;
    stats.feedback_bytes += wire.size();
    for (uint8_t i = 0; i < kMaxPaths; ++i) {
        if (fb.path_rtt_ms[i]) striper_.update_rtt(i, uint32_t(fb.path_rtt_ms[i]) * 1000);
        // The feedback reports per-path loss; anything short of total loss is
        // evidence that the path is still delivering (TRANSPORT.md 10).
        if (fb.path_loss[i] < 255) striper_.note_rx(i, now_us);
    }
    striper_.note_rx(path_id, now_us);
    return true;
}

}  // namespace nxt
