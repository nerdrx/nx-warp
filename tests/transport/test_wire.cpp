// Wire format round-trips: datagram header, tile directory, pose header,
// feedback packet in all three bitmap modes.  docs/TRANSPORT.md 2, 3, 8.
#include "nxvc/transport/aead.h"
#include "nxvc/transport/wire.h"
#include "test_util.h"

using namespace nxt;

static void header_roundtrip() {
    tt::begin("datagram header round-trip (fuzzed)");
    tt::Rng r(0xC0FFEE);
    for (int i = 0; i < 20000; ++i) {
        DatagramHeader h;
        h.flags = uint8_t(r.u32(16));
        h.stream_id = uint8_t(r.u32(256));
        h.frame_id = uint16_t(r.u32(65536));
        h.tile_first = uint16_t(r.u32(65536));
        h.tile_count = uint8_t(r.u32(256));
        h.layer_id = uint8_t(r.u32(4));
        h.frag_idx = uint8_t(r.u32(4));
        h.frag_count = uint8_t(r.u32(4));
        h.fec_class = uint8_t(r.u32(4));
        h.band = uint8_t(r.u32(8));
        h.pose_hdr = r.u32(2) != 0;
        h.fec_m = uint8_t(r.u32(8));
        h.caps = uint8_t(r.u32(256));
        h.pose_seq = uint16_t(r.u32(65536));
        h.path_seq = uint16_t(r.u32(16384));
        h.path_id = uint8_t(r.u32(4));
        h.fec_group = uint8_t(r.u32(256));
        h.fec_idx = uint8_t(r.u32(16));
        h.fec_k = uint8_t(r.u32(16));
        h.tx_ts = uint32_t(r.next());
        h.payload_len = uint16_t(r.u32(65536));
        h.enc_us = uint16_t(r.u32(65536));

        uint8_t buf[kHeaderBytes];
        encode_header(h, buf);
        DatagramHeader o;
        TT_CHECK(decode_header(buf, &o));
        TT_EQ(o.flags, h.flags);
        TT_EQ(o.stream_id, h.stream_id);
        TT_EQ(o.frame_id, h.frame_id);
        TT_EQ(o.tile_first, h.tile_first);
        TT_EQ(o.tile_count, h.tile_count);
        TT_EQ(o.layer_id, h.layer_id);
        TT_EQ(o.frag_idx, h.frag_idx);
        TT_EQ(o.frag_count, h.frag_count);
        TT_EQ(o.fec_class, h.fec_class);
        TT_EQ(o.band, h.band);
        TT_EQ(int(o.pose_hdr), int(h.pose_hdr));
        TT_EQ(o.fec_m, h.fec_m);
        TT_EQ(o.caps, h.caps);
        TT_EQ(o.pose_seq, h.pose_seq);
        TT_EQ(o.path_seq, h.path_seq);
        TT_EQ(o.path_id, h.path_id);
        TT_EQ(o.fec_group, h.fec_group);
        TT_EQ(o.fec_idx, h.fec_idx);
        TT_EQ(o.fec_k, h.fec_k);
        TT_EQ(o.tx_ts, h.tx_ts);
        TT_EQ(o.payload_len, h.payload_len);
        TT_EQ(o.enc_us, h.enc_us);
    }
    tt::end();
}

static void header_is_24_bytes_and_version_gated() {
    tt::begin("header size and version gate");
    DatagramHeader h;
    uint8_t buf[64] = {0};
    encode_header(h, buf);
    // Nothing beyond byte 23 is touched.
    for (int i = kHeaderBytes; i < 64; ++i) TT_EQ(int(buf[i]), 0);
    buf[0] = uint8_t((buf[0] & 0xF0) | 0x3);  // version 3
    DatagramHeader o;
    TT_CHECK(!decode_header(buf, &o));
    tt::end();
}

static void dir_entry_roundtrip() {
    tt::begin("tile directory entry round-trip");
    tt::Rng r(99);
    for (int i = 0; i < 20000; ++i) {
        TileDirEntry e;
        e.len = uint16_t(r.u32(4096));
        e.qp = uint8_t(r.u32(64));
        e.mode = uint8_t(r.u32(8));
        e.res_level = uint8_t(r.u32(4));
        e.lossless = r.u32(2) != 0;
        e.chroma444 = r.u32(2) != 0;
        e.alpha = r.u32(2) != 0;
        e.tile_class = uint8_t(r.u32(4));
        e.ref_delta = uint8_t(r.u32(4));
        uint32_t v = pack_dir_entry(e);
        TT_EQ(v >> 30, 0u);  // reserved bits stay zero
        TileDirEntry o = unpack_dir_entry(v);
        TT_EQ(o.len, e.len);
        TT_EQ(o.qp, e.qp);
        TT_EQ(o.mode, e.mode);
        TT_EQ(o.res_level, e.res_level);
        TT_EQ(int(o.lossless), int(e.lossless));
        TT_EQ(int(o.chroma444), int(e.chroma444));
        TT_EQ(int(o.alpha), int(e.alpha));
        TT_EQ(o.tile_class, e.tile_class);
        TT_EQ(o.ref_delta, e.ref_delta);
    }
    tt::end();
}

static void pose_roundtrip() {
    tt::begin("pose header round-trip");
    PoseHeader p;
    p.pose_seq = 4242;
    p.quat[0] = -30000; p.quat[1] = 12; p.quat[2] = 32767; p.quat[3] = -1;
    p.pos_mm_q8[0] = -123456; p.pos_mm_q8[1] = 0; p.pos_mm_q8[2] = 2000000000;
    p.render_finish_ts = 0xDEADBEEF;
    uint8_t buf[kPoseHeaderBytes];
    encode_pose_header(p, buf);
    PoseHeader o;
    TT_CHECK(decode_pose_header(buf, &o));
    TT_EQ(o.pose_seq, p.pose_seq);
    for (int i = 0; i < 4; ++i) TT_EQ(o.quat[i], p.quat[i]);
    for (int i = 0; i < 3; ++i) TT_EQ(o.pos_mm_q8[i], p.pos_mm_q8[i]);
    TT_EQ(o.render_finish_ts, p.render_finish_ts);
    tt::end();
}

static void feedback_roundtrip() {
    tt::begin("feedback round-trip, all bitmap modes");
    tt::Rng r(7);
    const uint16_t tib = 408;
    for (int iter = 0; iter < 400; ++iter) {
        FeedbackPacket fb;
        fb.stream_id = uint8_t(r.u32(256));
        fb.flags = uint8_t(r.u32(16));
        fb.tiles_in_band = tib;
        int nb = 1 + int(r.u32(3));
        for (int b = 0; b < nb; ++b) {
            BandReport br;
            br.frame_id = uint16_t(r.u32(65536));
            br.band = uint8_t(r.u32(6));
            br.rx_ts_first = uint32_t(r.next());
            br.rx_ts_last = uint32_t(r.next());
            br.decode_us = uint16_t(r.u32(65536));
            br.conceal_tiles = uint16_t(r.u32(400));
            br.late_tiles = uint16_t(r.u32(400));
            br.fec_recovered = uint8_t(r.u32(256));
            br.fec_failed = uint8_t(r.u32(256));
            br.received.assign(tib, 1);
            int mode = iter % 3;
            if (mode == 1) {  // sparse: a couple of loss bursts -> RLE
                uint32_t start = r.u32(tib - 40);
                for (uint32_t i = start; i < start + 12; ++i) br.received[i] = 0;
            } else if (mode == 2) {  // dense random loss -> RAW
                for (uint16_t i = 0; i < tib; ++i) br.received[i] = uint8_t(r.u32(2));
            }
            fb.bands.push_back(std::move(br));
        }
        fb.path_loss[0] = uint8_t(r.u32(256));
        fb.path_loss[1] = uint8_t(r.u32(256));
        fb.path_rtt_ms[0] = uint8_t(r.u32(256));
        fb.path_rtt_ms[1] = uint8_t(r.u32(256));

        for (bool rle : {false, true}) {
            ByteVec w = encode_feedback(fb, rle);
            FeedbackPacket o;
            TT_CHECK(decode_feedback(std::span<const uint8_t>(w.data(), w.size()), &o));
            TT_EQ(o.stream_id, fb.stream_id);
            TT_EQ(o.bands.size(), fb.bands.size());
            for (size_t i = 0; i < fb.bands.size(); ++i) {
                TT_EQ(o.bands[i].frame_id, fb.bands[i].frame_id);
                TT_EQ(o.bands[i].band, fb.bands[i].band);
                TT_EQ(o.bands[i].rx_ts_first, fb.bands[i].rx_ts_first);
                TT_EQ(o.bands[i].decode_us, fb.bands[i].decode_us);
                TT_EQ(o.bands[i].conceal_tiles, fb.bands[i].conceal_tiles);
                bool same = o.bands[i].received == fb.bands[i].received;
                TT_CHECK(same);
            }
            TT_EQ(o.path_loss[0], fb.path_loss[0]);
            TT_EQ(o.path_rtt_ms[1], fb.path_rtt_ms[1]);
            // TRANSPORT.md 8: a cumulative packet never exceeds an MTU.
            TT_CHECK(w.size() + kTagBytes <= 1400);
        }
    }
    tt::end();
}

static void feedback_sizes() {
    tt::begin("feedback size: clean band 20 B, worst case < 240 B");
    FeedbackPacket fb;
    fb.tiles_in_band = 408;
    for (int i = 0; i < 3; ++i) {
        BandReport br;
        br.received.assign(408, 1);
        fb.bands.push_back(br);
    }
    ByteVec clean = encode_feedback(fb, true);
    TT_EQ(clean.size(), size_t(8 + 3 * 20 + 4));
    for (auto& b : fb.bands)
        for (size_t i = 0; i < b.received.size(); i += 2) b.received[i] = 0;
    ByteVec worst = encode_feedback(fb, true);
    TT_CHECK(worst.size() == 8 + 3 * (20 + 51) + 4);
    TT_CHECK(worst.size() < 240);
    tt::end();
}

static void nonce_and_seq_extension() {
    tt::begin("nonce derivation and 14-bit sequence extension");
    Nonce a = derive_nonce(3, 1, 7, 0x0123456789ABCDEFull);
    TT_EQ(int(a[0]), 3);
    TT_EQ(int(a[1]), 1);
    TT_EQ(int(rd16(a.data() + 2)), 7);
    TT_EQ(rd64(a.data() + 4), 0x0123456789ABCDEFull);
    // Distinct paths and epochs give distinct nonces.
    TT_CHECK(derive_nonce(3, 0, 7, 5) != derive_nonce(3, 1, 7, 5));
    TT_CHECK(derive_nonce(3, 1, 7, 5) != derive_nonce(3, 1, 8, 5));

    // Extension across the 14-bit wrap.
    uint64_t expect = 16380;
    for (uint64_t n = 16380; n < 16390; ++n) {
        uint64_t got = extend_seq14(expect, uint16_t(n & 0x3FFF));
        TT_EQ(got, n);
        expect = got + 1;
    }
    // Reordering behind the expectation still resolves correctly.
    TT_EQ(extend_seq14(20000, uint16_t(19990 & 0x3FFF)), 19990ull);
    tt::end();
}

static void aead_seal_open() {
    tt::begin("AEAD seal/open, AAD and tag are enforced");
    auto null_aead = make_null_aead();
    std::vector<const Aead*> backends{null_aead.get()};
    auto real = make_default_aead();
    if (real) backends.push_back(real.get());

    Key k{}, salt{};
    for (size_t i = 0; i < kKeyBytes; ++i) { k[i] = uint8_t(i); salt[i] = uint8_t(255 - i); }
    Key sub0 = derive_subkey(k, salt, 0, Direction::kDownstream);
    Key sub1 = derive_subkey(k, salt, 1, Direction::kDownstream);
    Key subu = derive_subkey(k, salt, 0, Direction::kUpstream);
    TT_CHECK(sub0 != sub1);
    TT_CHECK(sub0 != subu);

    for (const Aead* a : backends) {
        ByteVec aad(kHeaderBytes);
        for (size_t i = 0; i < aad.size(); ++i) aad[i] = uint8_t(i * 7);
        ByteVec pt(300);
        for (size_t i = 0; i < pt.size(); ++i) pt[i] = uint8_t(i * 3 + 1);
        ByteVec ct(pt.size() + kTagBytes);
        Nonce n = derive_nonce(1, 0, 0, 42);
        size_t w = a->seal(sub0, n, aad, pt, ct.data());
        TT_EQ(w, pt.size() + kTagBytes);
        ByteVec out(pt.size());
        TT_EQ(a->open(sub0, n, aad, ct, out.data()), pt.size());
        TT_CHECK(out == pt);
        // Wrong AAD, wrong nonce, wrong key and a flipped bit all fail.
        aad[3] ^= 1;
        TT_CHECK(a->open(sub0, n, aad, ct, out.data()) == SIZE_MAX);
        aad[3] ^= 1;
        TT_CHECK(a->open(sub0, derive_nonce(1, 0, 0, 43), aad, ct, out.data()) == SIZE_MAX);
        TT_CHECK(a->open(sub1, n, aad, ct, out.data()) == SIZE_MAX);
        ct[10] ^= 0x80;
        TT_CHECK(a->open(sub0, n, aad, ct, out.data()) == SIZE_MAX);
    }
    tt::end();
}

int main() {
    header_roundtrip();
    header_is_24_bytes_and_version_gated();
    dir_entry_roundtrip();
    pose_roundtrip();
    feedback_roundtrip();
    feedback_sizes();
    nonce_and_seq_extension();
    aead_seal_open();
    return tt::report("transport.wire");
}
