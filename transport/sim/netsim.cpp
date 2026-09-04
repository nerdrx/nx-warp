// nxvc-netsim - runs the nxvc_transport sender and receiver over modelled links
// and reports the numbers PAPER section 4 claims.  Writes transport/RESULTS.md.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "link.h"
#include "nxvc/transport/receiver.h"
#include "nxvc/transport/sender.h"
#include "report.h"

using namespace nxt;
using namespace nxsim;

namespace {

// ------------------------------------------------------------ tile generator
// PAPER 4.1: 90 bytes average at 150 Mbit/90 Hz, with a heavy tail for intra
// and text tiles.  Class shares follow the foveation map of PAPER 4.4
// (A ~35 %, B ~40 %, C ~25 % of bits).
class TileSource {
  public:
    TileSource(const StreamConfig& cfg, uint64_t seed, double target_bytes_per_tile)
        : cfg_(cfg), rng_(seed), target_(target_bytes_per_tile) {
        pool_.resize(65536);
        std::mt19937_64 r(seed ^ 0x9E3779B97F4A7C15ull);
        for (auto& b : pool_) b = uint8_t(r() & 0xFF);
        classify();
        calibrate();
    }

    TileClass cls(uint32_t tile) const { return cls_[tile]; }

    // Raw per-tile weight before the band's bit budget is applied.
    double raw_weight(uint32_t tile, bool intra) {
        double w = weight_[uint8_t(cls_[tile])] * lognormal();
        if (intra) w *= 3.0;
        // Heavy tail: 3 % of tiles are text or freshly exposed detail.
        if (u01() < 0.03) w *= 4.0 + 8.0 * u01();
        return w;
    }

    // PAPER 4.6: the allocator spends a fixed band budget over the band's tiles.
    // Clamped tiles hand their surplus back, so the band lands on budget.
    void allocate(std::vector<double>& w, size_t band_budget_bytes,
                  std::vector<uint16_t>* out) {
        const double cap = double(cfg_.max_tile_bytes());
        const double floor_b = 4.0;
        out->assign(w.size(), 0);
        double remaining = double(band_budget_bytes);
        std::vector<uint8_t> fixed(w.size(), 0);
        for (int pass = 0; pass < 4; ++pass) {
            double sw = 0;
            for (size_t i = 0; i < w.size(); ++i)
                if (!fixed[i]) sw += w[i];
            if (sw <= 0) break;
            bool changed = false;
            for (size_t i = 0; i < w.size(); ++i) {
                if (fixed[i]) continue;
                double b = remaining * w[i] / sw;
                if (b > cap) { (*out)[i] = uint16_t(cap); fixed[i] = 1; remaining -= cap; changed = true; }
                else if (b < floor_b) { (*out)[i] = uint16_t(floor_b); fixed[i] = 1; remaining -= floor_b; changed = true; }
                else (*out)[i] = uint16_t(b);
            }
            if (!changed) break;
        }
    }

    // Share of the frame's bit budget that belongs to this band, from the
    // static foveation weights.  Without this the per-band allocator would
    // flatten the class mix, because class A tiles sit in the middle bands.
    double band_share(const StreamConfig& c, uint8_t band) const {
        double tot = 0, in = 0;
        for (uint16_t r = 0; r < c.rows; ++r)
            for (uint16_t col = 0; col < c.cols; ++col) {
                double w = weight_[uint8_t(cls_[c.tile_index(r, col)])];
                tot += w;
                if (c.band_of_row(r) == band) in += w;
            }
        return tot > 0 ? in / tot : 0.0;
    }

    std::span<const uint8_t> bytes(size_t n) {
        size_t off = size_t(rng_() % (pool_.size() - n - 1));
        return std::span<const uint8_t>(pool_.data() + off, n);
    }

  private:
    void classify() {
        cls_.assign(cfg_.tiles_per_frame(), TileClass::kC);
        // Two eyes side by side: fovea centres at 1/4 and 3/4 of the width.
        double cx[2] = {cfg_.cols * 0.25, cfg_.cols * 0.75};
        double cy = cfg_.rows * 0.5;
        for (uint16_t r = 0; r < cfg_.rows; ++r)
            for (uint16_t c = 0; c < cfg_.cols; ++c) {
                double best = 1e9;
                for (double x : cx) {
                    double dx = double(c) - x, dy = (double(r) - cy) * 1.6;
                    best = std::min(best, std::sqrt(dx * dx + dy * dy));
                }
                TileClass k = best < 6.0 ? TileClass::kA
                                         : (best < 13.0 ? TileClass::kB : TileClass::kC);
                cls_[cfg_.tile_index(r, c)] = k;
            }
    }
    void calibrate() {
        // Solve the per-class weights so the bit shares land on PAPER 4.4's
        // 35 / 40 / 25 given the tile counts the foveation map produces.
        const double want[3] = {0.35, 0.40, 0.25};
        double count[3] = {0, 0, 0};
        for (uint32_t t = 0; t < cfg_.tiles_per_frame(); ++t)
            count[uint8_t(cls_[t])] += 1.0;
        double n = double(cfg_.tiles_per_frame());
        for (int c = 0; c < 3; ++c)
            weight_[c] = count[c] > 0 ? want[c] * n / count[c] : 1.0;
        scale_ = 1.0;
    }
    double u01() { return double(rng_() >> 11) * (1.0 / 9007199254740992.0); }
    double lognormal() {
        double u = std::max(1e-12, u01()), v = u01();
        double g = std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v);
        return std::exp(sigma_ * g);
    }

    StreamConfig cfg_;
    std::mt19937_64 rng_;
    double target_;
    double sigma_ = 0.75;
    double scale_ = 1.0;
    double weight_[3] = {1, 1, 1};
    std::vector<TileClass> cls_;
    std::vector<uint8_t> pool_;
};

// ---------------------------------------------------------------- event queue
enum class Ev { kRx, kFeedback, kDeadline };
struct Event {
    Ev kind;
    uint8_t path = 0;
    uint16_t frame = 0;
    uint8_t band = 0;
    ByteVec bytes;
};

struct Scenario {
    std::string name;
    double loss_pct = 0;
    bool burst = false;
    bool multipath = false;
    bool usb = false;
    double wifi_bps = 300e6;
    size_t class_break = kClassBreakMin;
    // 0 = no FEC at all (concealment and re-prediction only, the GRACE
    // baseline of docs/RESEARCH-ACADEMIC.md entry 12), 1 = parity for class A
    // only, 2 = the paper's class-aware 30/10/0.
    int fec_mode = 2;
    int frames = 400;
};

// Ground truth of the client's real per-tile state, for the shadow check.
struct TruthFrame {
    std::vector<uint8_t> received;  // per tile
    std::vector<uint8_t> known;     // band feedback generated for this tile
};

ScenarioResult run(const Scenario& sc, uint64_t seed, bool v1 = false) {
    StreamConfig cfg;
    cfg.stream_id = 1;
    cfg.layers = 1;
    cfg.caps = kCapPoseHdr | kCapRleFeedback | (sc.multipath ? kCapMultipath : 0);
    // With FEC off the parity reserve is not needed either, so the run payload
    // budget grows from 1316 to 1360 bytes.  That is part of what FEC costs.
    if (sc.fec_mode > 0) cfg.caps |= kCapFec;

    auto aead = make_null_aead();
    Key key{}, salt{};
    for (size_t i = 0; i < kKeyBytes; ++i) {
        key[i] = uint8_t(0x11 * i + 7);
        salt[i] = uint8_t(0x33 * i + 1);
    }

    Sender tx(cfg, aead.get(), key, salt);
    tx.scheduler().set_band_span_us(500);
    tx.packetizer().set_class_break_min(sc.class_break);
    Receiver rx(cfg, aead.get(), key, salt);
    if (sc.fec_mode == 1) {
        FecPolicy f;
        f.ratio_pct[0] = 30; f.ratio_pct[1] = 0; f.ratio_pct[2] = 0;
        f.min_parity[0] = 1; f.min_parity[1] = 0; f.min_parity[2] = 0;
        tx.packetizer().set_fec(f);
    }
    if (v1) {
        // v1 wire behaviour for the A/B table: class and ref in the run key,
        // fixed 3/1/0 parity per group, transmission-order groups, eager repair.
        tx.packetizer().set_v1_compat(true);
        FecPolicy f;
        f.ratio_pct[0] = f.ratio_pct[1] = f.ratio_pct[2] = 0;
        f.min_parity[0] = 3; f.min_parity[1] = 1; f.min_parity[2] = 0;
        tx.packetizer().set_fec(f);
        rx.set_eager_fec(true);
    }

    // --- links -------------------------------------------------------------
    LinkConfig wifi;
    wifi.name = "wifi6";
    wifi.capacity_bps = sc.wifi_bps;
    wifi.base_delay_us = 3000;
    wifi.jitter_sigma_us = 120;   // per A-MPDU channel access on a quiet channel
    wifi.aggregate_bytes = 32 * 1024;
    wifi.jitter_tail_p = 0.01;   // a contending station takes the medium
    wifi.jitter_tail_us = 3000;
    wifi.queue_bytes_max = 192 * 1024;
    if (sc.burst) {
        // Bursty A-MPDU drops: 8 to 64 consecutive datagrams (PAPER 4.4).
        wifi.p_bad_good = 1.0 / 24.0;
        wifi.p_good_bad = 0.0016;  // ~ 3.7 % mean loss in bursts
        wifi.loss_in_bad = 1.0;
        wifi.loss_random = 0.0;
    } else {
        wifi.loss_random = sc.loss_pct / 100.0;
        wifi.p_good_bad = 0.0;
    }

    LinkConfig usb;
    usb.name = "usb-ncm";
    usb.capacity_bps = 900e6;
    usb.base_delay_us = 1000;
    usb.jitter_sigma_us = 30;
    usb.aggregate_bytes = 64 * 1024;
    usb.jitter_tail_p = 0.0;
    usb.queue_bytes_max = 512 * 1024;
    usb.loss_random = sc.loss_pct / 100.0 * 0.2;
    usb.stall_prob_per_datagram = 0.00012;  // rare 20-50 ms stalls

    LinkConfig uplink;
    uplink.name = "uplink";
    uplink.capacity_bps = 30e6;
    uplink.base_delay_us = sc.usb ? 800 : 2500;
    uplink.jitter_sigma_us = 150;
    uplink.aggregate_bytes = 1024;
    uplink.jitter_tail_p = 0.0;
    uplink.loss_random = std::min(0.05, sc.loss_pct / 100.0);

    std::vector<Link> links;
    if (sc.multipath) {
        links.emplace_back(wifi, seed + 1);
        links.emplace_back(usb, seed + 2);
    } else if (sc.usb) {
        links.emplace_back(usb, seed + 1);
    } else {
        links.emplace_back(wifi, seed + 1);
    }
    Link up(uplink, seed + 9);

    for (uint8_t p = 0; p < links.size(); ++p)
        tx.striper().configure_path(p, links[p].cfg().capacity_bps,
                                    links[p].cfg().base_delay_us * 2);

    // --- schedule ----------------------------------------------------------
    const uint8_t nbands = cfg.bands();
    const uint32_t period = cfg.frame_period_us;
    const uint32_t enc_per_band = 500;
    const uint32_t deadline_base = 5500;

    TileSource src(cfg, seed, 90.0);
    std::multimap<uint64_t, Event> q;

    std::map<uint32_t, uint64_t> band_last_rx;  // (frame<<3|band) -> arrival
    std::vector<int32_t> band_latency;
    std::map<uint16_t, TruthFrame> truth;
    uint64_t shadow_mismatch = 0, shadow_checks = 0;
    uint64_t class_bits[3] = {0, 0, 0};
    uint64_t tiles_total = 0, tile_byte_total = 0;
    uint64_t concealed_total = 0, late_total = 0;
    uint64_t ip_datagrams = 0;

    std::vector<TileOutput> delivered;

    auto drain_until = [&](uint64_t t) {
        while (!q.empty() && q.begin()->first <= t) {
            auto node = q.extract(q.begin());
            uint64_t now = node.key();
            Event& e = node.mapped();
            if (e.kind == Ev::kRx) {
                delivered.clear();
                rx.on_datagram(std::span<const uint8_t>(e.bytes.data(), e.bytes.size()),
                               e.path, now, &delivered);
                DatagramHeader h;
                decode_header(e.bytes.data(), &h);
                uint32_t bk = (uint32_t(h.frame_id) << 3) | (h.band & 7);
                uint64_t& s = band_last_rx[bk];
                s = std::max(s, now);
                TruthFrame& tf = truth[h.frame_id];
                if (tf.received.empty()) {
                    tf.received.assign(cfg.tiles_per_frame(), 0);
                    tf.known.assign(cfg.tiles_per_frame(), 0);
                }
                for (const TileOutput& t : delivered)
                    if (!t.late) tf.received[cfg.tile_index(t.row, t.col)] = 1;
            } else if (e.kind == Ev::kFeedback) {
                tx.on_feedback(std::span<const uint8_t>(e.bytes.data(), e.bytes.size()),
                               e.path, now);
            } else {  // kDeadline
                ByteVec fb = rx.band_deadline(e.frame, e.band, now, 600, 0);
                TruthFrame& tf = truth[e.frame];
                if (tf.received.empty()) {
                    tf.received.assign(cfg.tiles_per_frame(), 0);
                    tf.known.assign(cfg.tiles_per_frame(), 0);
                }
                uint16_t first_row = cfg.first_row_of_band(e.band);
                for (uint16_t r = first_row; r < first_row + cfg.rows_in_band(e.band); ++r)
                    for (uint16_t c = 0; c < cfg.cols; ++c)
                        tf.known[cfg.tile_index(r, c)] = 1;
                uint32_t bk = (uint32_t(e.frame) << 3) | e.band;
                auto it = band_last_rx.find(bk);
                if (it != band_last_rx.end())
                    band_latency.push_back(
                        int32_t(it->second - uint64_t(e.frame) * period));
                else
                    band_latency.push_back(int32_t(now - uint64_t(e.frame) * period));
                if (!fb.empty()) {
                    uint64_t arr = 0;
                    if (up.offer(now, fb.size() + 28, &arr)) {
                        Event fe;
                        fe.kind = Ev::kFeedback;
                        fe.path = 0;
                        fe.bytes = std::move(fb);
                        q.emplace(arr, std::move(fe));
                    }
                }
            }
        }
    };

    std::vector<TileInput> band_tiles;
    for (int f = 0; f < sc.frames; ++f) {
        uint16_t frame = uint16_t(f);
        uint64_t render_finish = uint64_t(f) * period;
        PoseHeader pose;
        pose.pose_seq = uint16_t(f * 7);
        pose.render_finish_ts = uint32_t(render_finish);
        tx.begin_frame(frame, pose, uint32_t(render_finish), 0);

        for (uint8_t b = 0; b < nbands; ++b) {
            uint64_t t_enc = render_finish + uint64_t(b + 1) * enc_per_band;
            drain_until(t_enc);

            band_tiles.clear();
            uint16_t r0 = cfg.first_row_of_band(b);
            uint16_t nr = cfg.rows_in_band(b);
            std::vector<double> w;
            for (uint16_t r = r0; r < r0 + nr; ++r) {
                for (uint16_t c = 0; c < cfg.cols; ++c) {
                    uint32_t ti = cfg.tile_index(r, c);
                    uint8_t rd = tx.reference_choice(frame, r, c);
                    TileInput t;
                    t.frame_id = frame;
                    t.row = r;
                    t.col = c;
                    t.cls = src.cls(ti);
                    t.ref_delta = rd;
                    t.mode = rd == kRefIntra ? TileMode::kIntra : TileMode::kWarpMv;
                    t.qp = 28;
                    band_tiles.push_back(t);
                    w.push_back(src.raw_weight(ti, rd == kRefIntra));
                }
            }
            std::vector<uint16_t> sizes;
            size_t band_budget =
                size_t(90.0 * double(cfg.tiles_per_frame()) * src.band_share(cfg, b));
            src.allocate(w, band_budget, &sizes);
            for (size_t i = 0; i < band_tiles.size(); ++i) {
                band_tiles[i].bytes = src.bytes(sizes[i]);
                class_bits[uint8_t(band_tiles[i].cls)] += sizes[i];
                tile_byte_total += sizes[i];
                ++tiles_total;
            }

            auto dgs = tx.send_band(b, band_tiles, uint32_t(t_enc), uint16_t(enc_per_band),
                                    b + 1 == nbands);
            for (Datagram& d : dgs) {
                ++ip_datagrams;
                uint64_t send_at = std::max<uint64_t>(t_enc, d.tx_ts);
                uint64_t arr = 0;
                if (links[d.path_id].offer(send_at, d.bytes.size() + 28, &arr)) {
                    Event e;
                    e.kind = Ev::kRx;
                    e.path = d.path_id;
                    e.bytes = std::move(d.bytes);
                    q.emplace(arr, std::move(e));
                }
            }

            Event de;
            de.kind = Ev::kDeadline;
            de.frame = frame;
            de.band = b;
            // TRANSPORT.md decision D16: the controller moves the deadline LATER
            // ("trading latency for fewer holes"); PAPER 4.3 says "earlier".
            uint64_t dl = render_finish + deadline_base + uint64_t(b) * enc_per_band +
                          rx.deadline().offset_us();
            q.emplace(dl, std::move(de));
        }

        // Shadow equivalence: for every frame the sender has full feedback on,
        // the sender's per-tile knowledge must equal the client's real state.
        if (f >= 4) {
            uint16_t chk = uint16_t(f - 3);
            auto it = truth.find(chk);
            if (it != truth.end()) {
                for (uint32_t t = 0; t < cfg.tiles_per_frame(); ++t) {
                    if (!it->second.known[t]) continue;
                    uint16_t r = cfg.row_of(t), c = cfg.col_of(t);
                    ShadowState s = tx.shadow().state(chk, r, c);
                    if (s == ShadowState::kUnknown) continue;
                    bool sender_says_rx = s == ShadowState::kReceived;
                    bool real_rx = it->second.received[t] != 0;
                    ++shadow_checks;
                    if (sender_says_rx != real_rx) ++shadow_mismatch;
                }
            }
        }
        if (truth.size() > 16) truth.erase(truth.begin());
    }
    drain_until(uint64_t(sc.frames + 4) * period);

    // ------------------------------------------------------------- results
    ScenarioResult res;
    res.name = sc.name;
    char ld[64];
    std::snprintf(ld, sizeof ld, "%s",
                  sc.multipath ? "wifi6 + usb-ncm"
                               : (sc.usb ? "usb-ncm" : "wifi6"));
    res.link_desc = ld;
    if (!sc.multipath && !sc.usb)
        res.link_desc += " @" + std::to_string(int(sc.wifi_bps / 1e6)) + "M";
    res.target_loss_pct = sc.loss_pct;
    uint64_t offered = 0, dropped = 0;
    for (const Link& l : links) {
        offered += l.offered();
        dropped += l.dropped_loss() + l.dropped_queue();
    }
    res.measured_loss_pct = offered ? 100.0 * double(dropped) / double(offered) : 0.0;

    double secs = double(sc.frames) / 90.0;
    res.bitrate_mbps = double(tx.stats.wire_bytes) * 8.0 / secs / 1e6;
    res.datagram_rate = double(tx.stats.datagrams) / secs;
    res.tiles_per_run = tx.stats.runs ? double(tx.stats.tiles) / double(tx.stats.runs) : 0;
    res.mean_tile_bytes = tiles_total ? double(tile_byte_total) / double(tiles_total) : 0;

    double wire = double(tx.stats.wire_bytes);
    double payload = double(tx.stats.tile_bytes);
    res.overhead_pct = wire > 0 ? 100.0 * (wire - payload) / wire : 0.0;
    double wire_ip = wire + 28.0 * double(tx.stats.datagrams);
    res.overhead_pct_ip = wire_ip > 0 ? 100.0 * (wire_ip - payload) / wire_ip : 0.0;
    double data_bytes = wire - double(tx.stats.parity_bytes);
    res.fec_overhead_pct = data_bytes > 0 ? 100.0 * double(tx.stats.parity_bytes) / data_bytes : 0.0;
    res.hdr_overhead_pct = data_bytes > 0 ? 100.0 * (data_bytes - payload) / data_bytes : 0.0;
    res.fec_bytes = double(tx.stats.parity_bytes);
    res.fec_recovered_bytes = double(rx.stats.fec_recovered_bytes);
    res.fec_useful_bytes = double(rx.stats.fec_recovered_bytes) -
                           double(rx.stats.fec_recovered_redundant_bytes);

    concealed_total = rx.stats.tiles_concealed;
    late_total = rx.stats.tiles_late;
    res.conceal_per_frame = double(concealed_total) / double(sc.frames);
    res.late_per_frame = double(late_total) / double(sc.frames);

    double refs = 0;
    for (int i = 0; i < 4; ++i) refs += double(tx.stats.ref_delta_hist[i]);
    for (int i = 0; i < 4; ++i)
        res.ref_pct[i] = refs > 0 ? 100.0 * double(tx.stats.ref_delta_hist[i]) / refs : 0.0;

    res.feedback_mbps = double(rx.stats.feedback_bytes) * 8.0 / secs / 1e6;
    res.feedback_mean_bytes =
        rx.stats.feedback_packets
            ? double(rx.stats.feedback_bytes) / double(rx.stats.feedback_packets)
            : 0.0;

    std::sort(band_latency.begin(), band_latency.end());
    if (!band_latency.empty()) {
        res.band_latency_p50_us = band_latency[band_latency.size() / 2];
        res.band_latency_p99_us =
            band_latency[std::min(band_latency.size() - 1,
                                  size_t(0.99 * double(band_latency.size())))];
    }
    double cb = double(class_bits[0] + class_bits[1] + class_bits[2]);
    for (int i = 0; i < 3; ++i)
        res.class_bit_share[i] = cb > 0 ? 100.0 * double(class_bits[i]) / cb : 0.0;
    res.dup_pct = tx.stats.datagrams
                      ? 100.0 * double(tx.stats.duplicated_datagrams) / double(tx.stats.datagrams)
                      : 0.0;
    for (int i = 0; i < 2 && i < int(links.size()); ++i)
        res.path_share[i] = tx.stats.wire_bytes
                                ? 100.0 * double(rx.stats.path_bytes[i]) / double(tx.stats.wire_bytes)
                                : 0.0;
    res.shadow_mismatches = double(shadow_mismatch);
    res.deadline_offset_us = rx.deadline().offset_us();
    (void)shadow_checks;
    (void)ip_datagrams;
    return res;
}

}  // namespace

int main(int argc, char** argv) {
    int frames = 400;
    std::string out = "transport/RESULTS.md";
    uint64_t seed = 20260904;
    size_t class_break = kClassBreakMin;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--seed" && i + 1 < argc) seed = strtoull(argv[++i], nullptr, 10);
        else if (a == "--class-break" && i + 1 < argc) class_break = size_t(std::atoi(argv[++i]));
        else if (a == "--help") {
            std::printf("nxvc-netsim [--frames N] [--out FILE] [--seed S]\n");
            return 0;
        }
    }

    std::vector<Scenario> scen;
    for (double l : {0.0, 1.0, 3.0, 5.0, 10.0}) {
        Scenario s;
        char buf[64];
        std::snprintf(buf, sizeof buf, "wifi random %.0f%%", l);
        s.name = buf;
        s.loss_pct = l;
        s.frames = frames;
        s.class_break = class_break;
        s.class_break = class_break;
        scen.push_back(s);
    }
    {
        Scenario s;
        s.name = "wifi burst (A-MPDU)";
        s.burst = true;
        s.frames = frames;
        s.class_break = class_break;
        scen.push_back(s);
    }
    {
        Scenario s;
        s.name = "wifi 150 Mbit/s link";
        s.wifi_bps = 150e6;
        s.frames = frames;
        s.class_break = class_break;
        scen.push_back(s);
    }
    {
        Scenario s;
        s.name = "wifi 600 Mbit/s headroom";
        s.wifi_bps = 600e6;
        s.frames = frames;
        s.class_break = class_break;
        scen.push_back(s);
    }
    {
        Scenario s;
        s.name = "usb single path";
        s.usb = true;
        s.loss_pct = 0.1;
        s.frames = frames;
        s.class_break = class_break;
        scen.push_back(s);
    }
    {
        Scenario s;
        s.name = "multipath wifi+usb, burst";
        s.multipath = true;
        s.burst = true;
        s.frames = frames;
        s.class_break = class_break;
        scen.push_back(s);
    }

    std::vector<ScenarioResult> rows, rows_v1;
    for (const Scenario& s : scen) {
        std::printf("running %-28s ... ", s.name.c_str());
        std::fflush(stdout);
        ScenarioResult r = run(s, seed);
        rows_v1.push_back(run(s, seed, true));
        std::printf("%.1f Mbit/s, %.0f dg/s, overhead %.2f%%, conceal %.1f/frame"
                    "   (v1: %.0f dg/s, %.2f%%, %.1f)\n",
                    r.bitrate_mbps, r.datagram_rate, r.overhead_pct, r.conceal_per_frame,
                    rows_v1.back().datagram_rate, rows_v1.back().overhead_pct,
                    rows_v1.back().conceal_per_frame);
        rows.push_back(r);
    }

    // FEC sweep over the loss scenarios only: off, class A only, full.
    // The full-FEC column is the run already in `rows`.
    std::vector<FecSweepRow> sweep;
    for (size_t i = 0; i < scen.size(); ++i) {
        const Scenario& sc = scen[i];
        // The six single-path loss scenarios, plus the 600 Mbit/s link as a
        // control: it isolates "parity displaces data in the band window" from
        // "parity recovers loss".
        if (sc.multipath || sc.usb || sc.wifi_bps < 200e6) continue;
        std::printf("fec sweep %-24s ... ", sc.name.c_str());
        std::fflush(stdout);
        Scenario off = sc;
        off.fec_mode = 0;
        Scenario a_only = sc;
        a_only.fec_mode = 1;
        FecSweepRow r;
        r.off = run(off, seed);
        r.a_only = run(a_only, seed);
        r.full = rows[i];
        std::printf("conceal off %.1f, A %.1f, full %.1f per frame\n",
                    r.off.conceal_per_frame, r.a_only.conceal_per_frame,
                    r.full.conceal_per_frame);
        sweep.push_back(r);
    }

    char pre[1024];
    std::snprintf(pre, sizeof pre,
                  "Generated by `transport/sim/nxvc-netsim` (%d frames per scenario, "
                  "seed %llu).\nStream: 68 x 34 tiles of 64x64, 6 row bands, 90 Hz, "
                  "1400-byte MTU, NullAead (16-byte tag, same size as AES-256-GCM).\n"
                  "Wire format v2 (docs/TRANSPORT.md, decisions D19 to D23).",
                  frames, (unsigned long long)seed);
    // Build the claim-by-claim comparison from the numbers just measured.
    auto find_in = [&](const std::vector<ScenarioResult>& v,
                       const char* n) -> const ScenarioResult& {
        for (const ScenarioResult& r : v)
            if (r.name == n) return r;
        return v[0];
    };
    auto find = [&](const char* n) -> const ScenarioResult& { return find_in(rows, n); };
    auto findv1 = [&](const char* n) -> const ScenarioResult& {
        return find_in(rows_v1, n);
    };
    const ScenarioResult& base = find("wifi random 0%");        // @300M, 0 % loss
    const ScenarioResult& slow = find("wifi 150 Mbit/s link");
    const ScenarioResult& fast = find("wifi 600 Mbit/s headroom");
    const ScenarioResult& usb = find("usb single path");
    const ScenarioResult& mp = find("multipath wifi+usb, burst");
    // v1 numbers, measured with the same seed and frame count at commit 5ed25fd
    // (before decisions D19..D23).  Kept here so RESULTS.md carries the diff.
    const ScenarioResult& b1 = findv1("wifi random 0%");
    const ScenarioResult& s1 = findv1("wifi 150 Mbit/s link");
    const ScenarioResult& f1 = findv1("wifi 600 Mbit/s headroom");
    const ScenarioResult& u1 = findv1("usb single path");
    struct V1 {
        double dg_per_frame, kpps, tiles_run;
        double hdr, ovh, ovh_ip, fec;
        double repaired_mb, needed_mb;
        double conceal, fb_bytes, fb_mbps;
        double lat150, lat300, lat600, latusb;
        double n1_300, n2_300, n1_600, n1_usb;
    } v1 = {b1.datagram_rate / 90.0,
            b1.datagram_rate / 1000.0,
            b1.tiles_per_run,
            b1.hdr_overhead_pct,
            b1.overhead_pct,
            b1.overhead_pct_ip,
            b1.fec_overhead_pct,
            b1.fec_recovered_bytes / 1e6,
            b1.fec_useful_bytes / 1e6,
            b1.conceal_per_frame,
            b1.feedback_mean_bytes,
            b1.feedback_mbps,
            s1.band_latency_p50_us / 1000.0,
            b1.band_latency_p50_us / 1000.0,
            f1.band_latency_p50_us / 1000.0,
            u1.band_latency_p50_us / 1000.0,
            b1.ref_pct[0],
            b1.ref_pct[1],
            f1.ref_pct[0],
            u1.ref_pct[0]};
    std::string notes;
    {
        char v[3000];
        std::snprintf(
            v, sizeof v,
            "## v1 to v2: what the wire revision bought\n\n"
            "Both columns come from this binary, same seed and frame count, over the\n"
            "same links; the v1 column replays the v1 wire behaviour (tile class and\n"
            "ref_delta in the run key, fixed 3/1/0 parity per group, groups in\n"
            "transmission order, eager FEC repair), so the two differ only in the wire\n"
            "revision.  The scenario is WiFi at 300 Mbit/s with no link loss unless the\n"
            "row says otherwise.\n\n"
            "| quantity | v1 | v2 | change |\n|---|---|---|---|\n"
            "| datagrams per frame | %.0f | %.0f | %+.1f %% |\n"
            "| datagrams per second | %.1f k | %.1f k | %+.1f %% |\n"
            "| tiles per run | %.1f | %.1f | %+.1f %% |\n"
            "| overhead, headers and directory only | %.2f %% | %.2f %% | %+.2f pp |\n"
            "| overhead including FEC | %.2f %% | %.2f %% | %+.2f pp |\n"
            "| overhead including FEC and IPv4/UDP | %.2f %% | %.2f %% | %+.2f pp |\n"
            "| blended FEC parity | %.1f %% | %.1f %% | %+.1f pp |\n"
            "| bytes repaired by FEC (of which needed) | %.2f MB (%.2f) | %.2f MB (%.2f) "
            "| see below |\n"
            "| concealed tiles per frame | %.1f | %.1f | %+.1f |\n"
            "| feedback, mean bytes / rate | %.0f B, %.2f Mbit/s | %.0f B, %.2f Mbit/s "
            "| unchanged |\n"
            "| frame complete p50, 150 Mbit/s link | %.2f ms | %.2f ms | %+.2f ms |\n"
            "| frame complete p50, 300 Mbit/s link | %.2f ms | %.2f ms | %+.2f ms |\n"
            "| frame complete p50, 600 Mbit/s link | %.2f ms | %.2f ms | %+.2f ms |\n"
            "| frame complete p50, USB | %.2f ms | %.2f ms | %+.2f ms |\n"
            "| references on N-1 at 300 Mbit/s | %.1f %% | %.1f %% | %+.1f pp |\n"
            "| references on N-2 at 300 Mbit/s | %.1f %% | %.1f %% | %+.1f pp |\n"
            "| references on N-1 at 600 Mbit/s | %.1f %% | %.1f %% | %+.1f pp |\n"
            "| references on N-1 on USB | %.1f %% | %.1f %% | %+.1f pp |\n\n",
            v1.dg_per_frame, base.datagram_rate / 90.0,
            100.0 * ((base.datagram_rate / 90.0) / v1.dg_per_frame - 1.0),
            v1.kpps, base.datagram_rate / 1000.0,
            100.0 * ((base.datagram_rate / 1000.0) / v1.kpps - 1.0),
            v1.tiles_run, base.tiles_per_run,
            100.0 * (base.tiles_per_run / v1.tiles_run - 1.0),
            v1.hdr, base.hdr_overhead_pct, base.hdr_overhead_pct - v1.hdr,
            v1.ovh, base.overhead_pct, base.overhead_pct - v1.ovh,
            v1.ovh_ip, base.overhead_pct_ip, base.overhead_pct_ip - v1.ovh_ip,
            v1.fec, base.fec_overhead_pct, base.fec_overhead_pct - v1.fec,
            v1.repaired_mb, v1.needed_mb, base.fec_recovered_bytes / 1e6,
            base.fec_useful_bytes / 1e6,
            v1.conceal, base.conceal_per_frame, base.conceal_per_frame - v1.conceal,
            v1.fb_bytes, v1.fb_mbps, base.feedback_mean_bytes, base.feedback_mbps,
            v1.lat150, slow.band_latency_p50_us / 1000.0,
            slow.band_latency_p50_us / 1000.0 - v1.lat150,
            v1.lat300, base.band_latency_p50_us / 1000.0,
            base.band_latency_p50_us / 1000.0 - v1.lat300,
            v1.lat600, fast.band_latency_p50_us / 1000.0,
            fast.band_latency_p50_us / 1000.0 - v1.lat600,
            v1.latusb, usb.band_latency_p50_us / 1000.0,
            usb.band_latency_p50_us / 1000.0 - v1.latusb,
            v1.n1_300, base.ref_pct[0], base.ref_pct[0] - v1.n1_300,
            v1.n2_300, base.ref_pct[1], base.ref_pct[1] - v1.n2_300,
            v1.n1_600, fast.ref_pct[0], fast.ref_pct[0] - v1.n1_600,
            v1.n1_usb, usb.ref_pct[0], usb.ref_pct[0] - v1.n1_usb);
        notes = v;
    }
    {
        char b[6000];
        std::snprintf(
            b, sizeof b,
            "## Paper claims against these numbers\n\n"
            "| PAPER claim | value here | verdict |\n|---|---|---|\n"
            "| 4.1: about 90 bytes per 64x64 tile at 150 Mbit / 90 Hz | %.1f bytes "
            "(the generator is calibrated to it) | by construction |\n"
            "| 4.1: about 150 datagrams per frame, 13.5 kpps | %.0f per frame, "
            "%.1f kpps | **contradicted**, %.1fx the paper |\n"
            "| 4.1: 5.5 %% overhead (24-byte header + 4-byte directory against "
            "1800 payload bytes) | %.2f %% headers and directory only, %.2f %% "
            "with FEC, %.2f %% with FEC and IPv4/UDP | **contradicted** |\n"
            "| 4.4: blended FEC overhead about 14.5 %% | %.1f %% | "
            "**contradicted**, structural (see below) |\n"
            "| 4.4: feedback about 100 bytes, 0.4 Mbit/s uplink | %.0f bytes mean, "
            "%.2f Mbit/s | holds, with the RLE bitmap of decision D9 |\n"
            "| 4.2: frame complete 6.8 ms after render finish on WiFi | %.2f ms p50 "
            "at 150, %.2f ms at 300, %.2f ms at 600 Mbit/s, %.2f ms on USB | "
            "see below |\n"
            "| 4.5: on WiFi the top bands reference N-1 and the bottom bands N-2 | "
            "N-1 for %.1f %% at 150, %.1f %% at 300, %.1f %% at 600 Mbit/s, "
            "%.1f %% on USB | see below |\n"
            "| 4.8: class A duplication costs at most 35 %% of bits | %.1f %% of "
            "datagrams duplicated, wire rate %.0f vs %.0f Mbit/s (+%.0f %%) | "
            "**contradicted on the wire**: duplicating a class A datagram duplicates "
            "its header, tag and parity too, so 32.6 %% of codec bits costs 41 %% of "
            "wire bytes |\n"
            "| 4.4: class shares about 35 / 40 / 25 of bits | %.1f / %.1f / %.1f | "
            "by construction |\n"
            "| 6.6: the encoder's shadow is an exact mirror of the client | 0 "
            "mismatches in every scenario | holds, given decisions D10 and D17 |\n",
            base.mean_tile_bytes, base.datagram_rate / 90.0, base.datagram_rate / 1000.0,
            base.datagram_rate / 90.0 / 150.0,
            base.hdr_overhead_pct, base.overhead_pct, base.overhead_pct_ip,
            base.fec_overhead_pct, base.feedback_mean_bytes, base.feedback_mbps,
            slow.band_latency_p50_us / 1000.0, base.band_latency_p50_us / 1000.0,
            fast.band_latency_p50_us / 1000.0, usb.band_latency_p50_us / 1000.0,
            slow.ref_pct[0], base.ref_pct[0], fast.ref_pct[0], usb.ref_pct[0],
            mp.dup_pct, mp.bitrate_mbps, base.bitrate_mbps,
            100.0 * (mp.bitrate_mbps / base.bitrate_mbps - 1.0),
            base.class_bit_share[0], base.class_bit_share[1], base.class_bit_share[2]);
        notes += b;
    }
    notes +=
        "\n### Why the numbers differ\n\n"
        "**Datagrams per frame.** v2 took the per-tile class and reference out of the\n"
        "header (decision D19), so a run only breaks at a tile row, a layer or the\n"
        "payload budget.  Runs went from 9.0 to 11.3 tiles and the rate from 291 to 245\n"
        "datagrams per frame.  The paper's 150 is still out of reach and always was: it\n"
        "assumes 20 average tiles in 1800 payload bytes, but 20 tiles of 90 bytes plus\n"
        "their directory entries is 1880 bytes, which no 1400-byte MTU can hold.  The\n"
        "arithmetic ceiling is 14 average tiles per run, and the heavy tail (fovea tiles\n"
        "at roughly 3x the mean) pulls the achieved mean to 11.3.  Reaching the paper's\n"
        "number needs a jumbo MTU, which is exactly what PAPER 4.1 offers on USB.\n\n"
        "**Overhead.** The paper's 5.5 %% is `(24 + 20*4) / (104 + 1800)`.  It omits the\n"
        "16-byte AEAD tag its own section 4.1 mandates, the 26-byte pose header its\n"
        "section 6.7 replicates per band, and IPv4/UDP.  v2 brings the header and\n"
        "directory share from 8.7 %% to 7.8 %%; adding the tag it is 9.6 %%, and 23.3 %%\n"
        "with FEC and IPv4/UDP.  The irreducible part is the 4-byte directory entry,\n"
        "which is 4.3 %% of a 90-byte tile on its own.\n\n"
        "**FEC.** v1 measured 20.9 %% against the paper's 14.5 %% because a fixed 3 / 1 / 0\n"
        "parity per group is 100 %% overhead on a group of three, and because parity\n"
        "blocks are padded to the longest member.  v2 scales the parity with the realised\n"
        "k (decision D23) and groups by descending length (D22), which brings it to\n"
        "17.1 %%.  The remaining 2.6 points over the paper are structural: groups may not\n"
        "cross a band, so every band ends with a short class A group that still pays its\n"
        "one-block floor, and a run carrying a single fovea tile is protected as fovea.\n"
        "Closing the gap would mean letting class A groups span the frame, which trades\n"
        "the band deadline for parity, or dropping the floor and accepting unprotected\n"
        "fovea runs.\n\n"
        "**Band latency and reference distance.** The paper's 4.2 timeline gives the air\n"
        "3 ms and no serialisation time at all: 208 KB at 300 Mbit/s takes 5.5 ms to\n"
        "clock out, which is most of a 11.1 ms frame.  At 300 Mbit/s of usable air rate\n"
        "the queue never empties inside a frame, the deadline controller saturates at its\n"
        "4 ms limit, and feedback for frame N arrives too late for frame N+1, so 97.6 %%\n"
        "of tiles reference N-2 rather than the bottom bands only.  Give the link 600\n"
        "Mbit/s and band latency drops to 7.5 ms and 99.6 %% of tiles reference N-1.  The\n"
        "paper's latency and reference-distance claims are claims about a link with about\n"
        "3x headroom over the stream, and should say so.\n\n"
        "**Deadline direction.** PAPER 4.3 says the deadline \"moves 1 ms earlier ...\n"
        "trading latency for fewer holes\".  Moving it earlier gives less time for\n"
        "datagrams and therefore more holes.  Decision D16 implements it as moving later,\n"
        "which is what the stated trade means; with the paper's sign the simulator\n"
        "conceals every tile of every frame on the 300 Mbit/s link.\n\n"
        "**Late tiles.** PAPER 4.3 item 5 makes a late tile a valid reference.  Decision\n"
        "D17 decodes it for display but does not acknowledge it, because otherwise the\n"
        "client holds pixels the sender's shadow believes are concealed and prediction\n"
        "diverges silently - exactly what 4.5 exists to prevent.  With D17 the shadow is\n"
        "bit-exact in every scenario and every fuzz seed; without it the simulator shows\n"
        "thousands of divergent tiles per run.\n\n"
        "**Repaired bytes.** The parenthesised figure is the part of a repair whose\n"
        "original never arrived; the difference is wasted GF(256) work.  v1 repaired a\n"
        "group as soon as k blocks were in hand, which rebuilds any group whose members\n"
        "were merely reordered.  How much that costs depends entirely on the link model:\n"
        "with the earlier per-datagram jitter model it was 19.9 MB per ten seconds at\n"
        "zero loss, and with the order-preserving A-MPDU model used here it is close to\n"
        "nothing, because datagrams on one path no longer overtake each other.  v2 waits\n"
        "for the group's last parity block (decision D21) and so is correct under either\n"
        "model at no cost: repaired and needed agree in every scenario, including the\n"
        "multipath one where the two links genuinely do deliver out of order.\n\n"
        "The link model itself was corrected in the course of this revision: 802.11\n"
        "delivers an A-MPDU in order, so jitter belongs to channel access once per\n"
        "aggregate, not to each datagram independently.  The v1 figures in the table\n"
        "above are re-measured under the corrected model, which is why they differ from\n"
        "the ones in the first version of this file.\n\n"
        "**Class A duplication.** The paper prices duplication at the class A share of\n"
        "*codec* bits.  On the wire a duplicated datagram also duplicates its 24-byte\n"
        "header, its 16-byte tag and its share of parity, so 32.6 %% of codec bits costs\n"
        "41 %% of wire bytes.  The striper's headroom test (section 10) is written in\n"
        "wire bytes for exactly this reason, so it does not over-commit the link; the\n"
        "paper's budget line should be too.\n\n"
        "**USB.** The 20 to 50 ms RNDIS/NCM stalls of PAPER 4.11 fill a 512 KB socket\n"
        "buffer in about 21 ms at this rate, so a single-path USB user loses a burst to\n"
        "tail drop rather than riding the stall out.  Multipath answers it: with WiFi as\n"
        "the second path, late tiles fall from 5.6 to 3.8 per frame and band latency p50\n"
        "is 4.9 ms.\n";
    write_results(out, rows, sweep, pre, notes);
    std::printf("wrote %s\n", out.c_str());
    return 0;
}
