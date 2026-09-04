// NX Warp Android client shell -- application entry point.
//
// Three threads:
//   receive  (nxc_net.cpp)  recvmmsg into the lock-free ring, nothing else
//   decode   (here)         drain the ring -> depacketizer -> frame ring,
//                           run the deadline state machine, send feedback
//   main     (here)         android_native_app_glue events, HUD, present
//
// The split is the one PAPER 4.1 prescribes: "the CPU moves bytes and checks
// tags, it never parses the bitstream", and the receive thread does not even do
// that much -- it only gets datagrams off the socket, so that the pps ceiling
// being measured is the receive path's and not the parser's.
#include <android_native_app_glue.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <thread>
#include <vector>

#include "nxc_config.h"
#include "nxc_font.h"
#include "nxc_frame_ring.h"
#include "nxc_net.h"
#include "nxc_transport.h"
#include "nxc_vk.h"
#include "nxc_wire.h"

namespace nxc {
namespace {

// ---------------------------------------------------------------- telemetry

// PAPER 4.9 asks for p50/p99 over the last second. A 1024-sample reservoir at
// 90 Hz x 6 bands is more than a second of band records, and sorting 1024 u32
// once a second is free.
class Percentiles {
public:
    void add(uint32_t v) {
        if (s_.size() < 1024) s_.push_back(v);
        else s_[next_++ % 1024] = v;
    }
    void snapshot(uint32_t* p50, uint32_t* p99) {
        if (s_.empty()) { *p50 = *p99 = 0; return; }
        tmp_ = s_;
        std::sort(tmp_.begin(), tmp_.end());
        *p50 = tmp_[tmp_.size() / 2];
        *p99 = tmp_[std::min(tmp_.size() - 1, tmp_.size() * 99 / 100)];
    }
    void clear() { s_.clear(); next_ = 0; }

private:
    std::vector<uint32_t> s_, tmp_;
    size_t next_ = 0;
};

struct Telemetry {
    // Carried on the wire (TRANSPORT.md 11).
    Percentiles enc_us;      // encode finish minus render finish, server clock delta
    Percentiles spread_us;   // last_rx - first_rx of a frame, client clock
    Percentiles decode_us;   // GPU time of the two decode dispatches
    // Snapshot values for the HUD.
    uint32_t enc_p50 = 0, enc_p99 = 0;
    uint32_t spread_p50 = 0, spread_p99 = 0;
    uint32_t decode_p50 = 0, decode_p99 = 0;
};

// ---------------------------------------------------------------- self test

// Ground-truth loss accounting from the nxvc-blast trailer. Independent of the
// 14-bit path_seq, which wraps every 16384 datagrams -- 0.18 s at 90 kpps -- and
// therefore cannot by itself distinguish a 16384-datagram burst loss from none.
struct SelfTest {
    bool     active = false;
    uint64_t seen = 0;
    uint64_t lost = 0;
    uint64_t reordered = 0;
    uint64_t dup = 0;
    uint32_t next_expected = 0;
    uint32_t highest = 0;
    bool     started = false;

    void observe(uint32_t abs_seq) {
        active = true;
        ++seen;
        if (!started) { started = true; next_expected = abs_seq + 1; highest = abs_seq; return; }
        if (abs_seq == next_expected) {
            next_expected = abs_seq + 1;
        } else if (int32_t(abs_seq - next_expected) > 0) {
            lost += uint32_t(abs_seq - next_expected);
            next_expected = abs_seq + 1;
        } else {
            // Behind the cursor: either a genuine reorder or a duplicate. Both
            // are counted, and both mean the earlier "lost" attribution was
            // pessimistic; the report says so rather than silently correcting.
            ++reordered;
            if (lost) --lost;
        }
        if (int32_t(abs_seq - highest) > 0) highest = abs_seq;
    }
};

// ---------------------------------------------------------------- app

class App : public ITileSink, public IFeedbackSink {
public:
    explicit App(android_app* app) : app_(app) {
        cfg_.mode = AppMode::kSelfTest;
        depack_ = create_stub_depacketizer(cfg_.stream, cfg_.touch_payload);
        ring_ = std::make_unique<FrameRing>(cfg_, depack_.get(), this);
        rx_ = std::make_unique<Receiver>(cfg_);
        hud_ = std::make_unique<TextCanvas>(84, 26);
    }

    ~App() override { stop(); }

    bool start_net() {
        if (!rx_->open()) return false;
        rx_->start();
        snmp0_ = Receiver::read_snmp_udp();
        decode_run_.store(true);
        decode_thread_ = std::thread([this] { decode_main(); });
        return true;
    }

    void stop() {
        if (decode_run_.exchange(false) && decode_thread_.joinable()) decode_thread_.join();
        if (rx_) rx_->stop();
        renderer_.shutdown();
    }

    // ---- ITileSink: decorate the frame ring so telemetry sees every run.
    void place(const PlacedRun& run) override {
        tel_.enc_us.add(run.enc_us);
        if (run.frame_id != tel_frame_ || !tel_frame_valid_) {
            if (tel_frame_valid_ && tel_last_rx_ > tel_first_rx_)
                tel_.spread_us.add(uint32_t(tel_last_rx_ - tel_first_rx_));
            tel_frame_ = run.frame_id;
            tel_frame_valid_ = true;
            tel_first_rx_ = run.rx_ts_us;
        }
        tel_last_rx_ = run.rx_ts_us;
        ring_->place(run);
    }

    // ---- IFeedbackSink
    void send_feedback(const uint8_t* data, size_t len) override {
        if (rx_->send_to_peer(data, len)) fb_bytes_ += len;
    }

    void on_window_created() {
        if (!app_->window) return;
        if (!renderer_.ready()) {
            if (have_device_) {
                if (!renderer_.surface_regained(app_->window))
                    log_err("failed to rebuild the swapchain after resume");
            } else if (renderer_.init(app_->window, cfg_)) {
                have_device_ = true;
            } else {
                log_err("Vulkan init failed");
            }
        }
    }

    void on_window_destroyed() {
        if (have_device_) renderer_.surface_lost();
    }

    void frame() {
        if (!renderer_.ready()) return;

        std::vector<uint32_t> meta;
        uint16_t fid = 0;
        FrameClassification cls{};
        const bool have = ring_->snapshot(&meta, &fid, &cls);

        const uint32_t dus = renderer_.last_decode_us();
        if (dus) { tel_.decode_us.add(dus); ring_->set_decode_us(dus); }

        sample_once_per_second();
        build_hud(cls, have);

        renderer_.render(meta, *hud_, fid);
        ++frames_presented_;
    }

private:
    // ---------------------------------------------------------- decode thread

    void decode_main() {
        pthread_setname_np(pthread_self(), "nxc-decode");
        SpscRing& r = rx_->ring();
        while (decode_run_.load(std::memory_order_relaxed)) {
            uint32_t n = r.available();
            if (n == 0) {
                // Nothing queued. The deadline machine still has to run, so this
                // is a short sleep rather than a block on the ring.
                ring_->tick(now_us());
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            if (n > 256) n = 256;   // bound the time between deadline checks
            const uint64_t t = now_us();
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t len = 0;
                const uint8_t* p = r.peek(i, &len);
                if (len == 0) continue;
                // The nxvc-blast self-test trailer sits after the datagram
                // proper, so it is stripped before the depacketizer sees it.
                uint32_t dlen = len;
                if (len >= kSelfTestTrailerBytes) {
                    const uint8_t* tr = p + len - kSelfTestTrailerBytes;
                    if (rd_u32(tr) == kSelfTestMagic) {
                        st_.observe(rd_u32(tr + 4));
                        dlen = len - kSelfTestTrailerBytes;
                    }
                }
                depack_->submit(p, dlen, t, this);
            }
            r.consume(n);
            ring_->tick(now_us());
        }
    }

    // ---------------------------------------------------------- once a second

    void sample_once_per_second() {
        const uint64_t t = now_us();
        if (last_sample_us_ == 0) {
            last_sample_us_ = t;
            last_dgrams_ = rx_->stats().datagrams.load();
            last_bytes_ = rx_->stats().bytes.load();
            last_proc_ticks_ = read_cpu_ticks(0);
            last_rx_ticks_ = read_cpu_ticks(rx_->rx_tid());
            last_frames_ = frames_presented_;
            return;
        }
        const uint64_t dt = t - last_sample_us_;
        if (dt < 1000000ull) return;

        const uint64_t dg = rx_->stats().datagrams.load();
        const uint64_t by = rx_->stats().bytes.load();
        pps_ = double(dg - last_dgrams_) * 1e6 / double(dt);
        bps_ = double(by - last_bytes_) * 1e6 / double(dt);
        fps_ = double(frames_presented_ - last_frames_) * 1e6 / double(dt);

        // CPU as permille of one core.
        const long hz = clock_ticks_per_sec();
        const uint64_t pt = read_cpu_ticks(0);
        const uint64_t rt = read_cpu_ticks(rx_->rx_tid());
        if (pt != UINT64_MAX && last_proc_ticks_ != UINT64_MAX)
            cpu_proc_permille_ = uint32_t(double(pt - last_proc_ticks_) / double(hz) * 1e6 / double(dt) * 1000.0);
        if (rt != UINT64_MAX && last_rx_ticks_ != UINT64_MAX)
            cpu_rx_permille_ = uint32_t(double(rt - last_rx_ticks_) / double(hz) * 1e6 / double(dt) * 1000.0);

        tel_.enc_us.snapshot(&tel_.enc_p50, &tel_.enc_p99);
        tel_.spread_us.snapshot(&tel_.spread_p50, &tel_.spread_p99);
        tel_.decode_us.snapshot(&tel_.decode_p50, &tel_.decode_p99);

        const auto snmp = Receiver::read_snmp_udp();
        const uint64_t rcvbuf_err = (snmp.ok && snmp0_.ok)
                                        ? (snmp.rcvbuf_errors - snmp0_.rcvbuf_errors) : 0;

        const auto& c = depack_->counters();
        const RingStats rs = ring_->stats();

        // One logcat line per second, greppable. A device run with adb attached
        // gets the numbers here; a run without adb gets them from the stats
        // report below, printed by nxvc-blast.
        log_info("NXC-SELFTEST pps=%.0f mbps=%.2f rx=%" PRIu64 " ringfull=%" PRIu64
                 " snmp_rcvbuf_err=%" PRIu64 " st_seen=%" PRIu64 " st_lost=%" PRIu64
                 " st_reord=%" PRIu64 " placed_tiles=%" PRIu64 " conceal=%" PRIu64
                 " late=%" PRIu64 " badver=%" PRIu64 " baddir=%" PRIu64 " badrange=%" PRIu64
                 " dup=%" PRIu64 " parity=%" PRIu64 " fb=%" PRIu64
                 " cpu_proc=%.1f%% cpu_rx=%.1f%% batch=%.1f fps=%.1f decode_us_p50=%u",
                 pps_, bps_ * 8.0 / 1e6, rx_->stats().datagrams.load(),
                 rx_->stats().drops_ring_full.load(), rcvbuf_err,
                 st_.seen, st_.lost, st_.reordered,
                 c.placed_tiles, rs.tiles_concealed, rs.tiles_late,
                 c.bad_version, c.bad_directory, c.bad_range, c.duplicate,
                 c.parity_dropped, rs.feedback_sent,
                 cpu_proc_permille_ / 10.0, cpu_rx_permille_ / 10.0,
                 mean_batch(), fps_, tel_.decode_p50);

        send_stats_report(dt, rcvbuf_err);

        last_sample_us_ = t;
        last_dgrams_ = dg;
        last_bytes_ = by;
        last_proc_ticks_ = pt;
        last_rx_ticks_ = rt;
        last_frames_ = frames_presented_;
    }

    double mean_batch() const {
        const uint64_t b = rx_->stats().batches.load();
        return b ? double(rx_->stats().batch_msgs.load()) / double(b) : 0.0;
    }

    void send_stats_report(uint64_t dt, uint64_t rcvbuf_err) {
        StatsReport r{};
        r.magic = kStatsMagic;
        r.seq = ++report_seq_;
        r.window_us = dt;
        r.rx_datagrams = rx_->stats().datagrams.load();
        r.rx_bytes = rx_->stats().bytes.load();
        r.lost_datagrams = st_.lost;
        r.reordered = st_.reordered;
        r.rcvbuf_granted = uint32_t(std::max(0, rx_->caps().rcvbuf_granted));
        r.sched_policy = rx_->caps().sched_fifo ? 1u : 0u;
        r.affinity_mask = rx_->caps().affinity_mask;
        r.cpu_permille_proc = cpu_proc_permille_;
        r.cpu_permille_rx = cpu_rx_permille_;
        r.drops_ring_full = uint32_t(rx_->stats().drops_ring_full.load());
        r.udp_rcvbuf_errors = uint32_t(rcvbuf_err);
        rx_->send_to_peer(&r, sizeof(r));
    }

    // ---------------------------------------------------------- HUD

    void build_hud(const FrameClassification& cls, bool have_frame) {
        hud_->clear();
        const auto& c = depack_->counters();
        const RingStats rs = ring_->stats();
        const ReceiveCaps& rc = rx_->caps();
        uint32_t y = 0;

        hud_->printf_at(0, y++, kColHeading, "NX WARP CLIENT SHELL  -  %s",
                        renderer_.decoder() && renderer_.decoder()->is_placeholder()
                            ? "PLACEHOLDER DECODER" : "DECODER");
        hud_->printf_at(0, y++, kColDim, "%s  %ux%u  SUBGROUP %u",
                        renderer_.gpu().device_name, renderer_.extent().width,
                        renderer_.extent().height, renderer_.gpu().subgroup_size);
        ++y;

        // ---- receive path (PAPER 4.1 / 4.11)
        hud_->text(0, y++, "RECEIVE PATH", kColHeading);
        hud_->printf_at(0, y++, pps_ > 1.0 ? kColBody : kColWarn,
                        "PPS %-9.0f  MBIT/S %-8.2f  BATCH %.1f/%d",
                        pps_, bps_ * 8.0 / 1e6, mean_batch(), cfg_.recv_batch);
        // PAPER 4.11: request 8 MB and verify. Linux reports twice what it keeps.
        {
            const int usable = rc.rcvbuf_granted > 0 ? rc.rcvbuf_granted / 2 : 0;
            const bool capped = usable < cfg_.want_rcvbuf;
            hud_->printf_at(0, y++, capped ? kColWarn : kColGood,
                            "RCVBUF WANT %d KB  GOT %d KB%s",
                            cfg_.want_rcvbuf / 1024, usable / 1024,
                            capped ? "  (ROM CAPPED)" : "");
        }
        hud_->printf_at(0, y++, rc.sched_fifo ? kColGood : kColWarn,
                        "SCHED %s  CPU %d %s",
                        rc.sched_fifo ? "FIFO" : "OTHER (REFUSED)",
                        rc.chosen_cpu,
                        rc.chosen_cpu >= 0 ? "PINNED" : "UNPINNED");
        hud_->printf_at(0, y++, kColBody, "CPU PROC %.1f%%  RX THREAD %.1f%%",
                        cpu_proc_permille_ / 10.0, cpu_rx_permille_ / 10.0);
        hud_->printf_at(0, y++,
                        rx_->stats().drops_ring_full.load() ? kColBad : kColDim,
                        "RING %u/%u  DROPS(FULL) %" PRIu64,
                        rx_->ring().depth(), rx_->ring().slots(),
                        rx_->stats().drops_ring_full.load());
        if (st_.active) {
            const double pct = st_.seen ? 100.0 * double(st_.lost) / double(st_.seen + st_.lost) : 0.0;
            hud_->printf_at(0, y++, pct > 0.1 ? kColBad : kColGood,
                            "SELFTEST SEEN %" PRIu64 "  LOST %" PRIu64 " (%.3f%%)  REORD %" PRIu64,
                            st_.seen, st_.lost, pct, st_.reordered);
        }
        ++y;

        // ---- transport (TRANSPORT.md 12)
        hud_->text(0, y++, "TRANSPORT", kColHeading);
        hud_->printf_at(0, y++, kColBody, "RUNS %" PRIu64 "  TILES %" PRIu64 "  DUP %" PRIu64
                        "  PARITY %" PRIu64, c.placed_runs, c.placed_tiles, c.duplicate,
                        c.parity_dropped);
        const uint64_t bad = c.bad_version + c.bad_caps + c.bad_directory + c.bad_range +
                             c.short_datagram;
        hud_->printf_at(0, y++, bad ? kColWarn : kColDim,
                        "DROP VER %" PRIu64 " CAPS %" PRIu64 " DIR %" PRIu64 " RANGE %" PRIu64
                        " SHORT %" PRIu64, c.bad_version, c.bad_caps, c.bad_directory,
                        c.bad_range, c.short_datagram);
        hud_->printf_at(0, y++, kColBody, "PATH0 RX %" PRIu64 " LOST %" PRIu64
                        "   PATH1 RX %" PRIu64 " LOST %" PRIu64,
                        c.path_rx[0], c.path_lost[0], c.path_rx[1], c.path_lost[1]);
        hud_->printf_at(0, y++, kColBody, "FEEDBACK SENT %" PRIu64 "  %" PRIu64 " BYTES",
                        rs.feedback_sent, fb_bytes_);
        ++y;

        // ---- frame ring (PAPER 4.3 / TRANSPORT.md 7)
        hud_->text(0, y++, "FRAME RING (4 SLOT)", kColHeading);
        hud_->printf_at(0, y++, kColBody, "FRAME %u  SLOT %u  ADVANCED %" PRIu64
                        "  STALE DROP %" PRIu64,
                        rs.newest_frame, rs.newest_frame % cfg_.stream.ring_slots,
                        rs.frames_advanced, rs.stale_frame_drop);
        if (have_frame) {
            hud_->printf_at(0, y++, cls.concealed ? kColWarn : kColGood,
                            "FRESH %u  STALE %u  CONCEALED %u  UNDEC %u  EMPTY %u",
                            cls.fresh, cls.stale, cls.concealed, cls.undecodable, cls.empty);
            hud_->printf_at(0, y++, kColBody, "TILES/FRAME %u  PARTIAL %s",
                            cls.total, cls.partial() ? "YES" : "NO");
        } else {
            hud_->text(0, y++, "NO FRAME YET  -  WAITING FOR DATAGRAMS", kColWarn);
        }
        hud_->printf_at(0, y++, rs.deadline_offset_us ? kColWarn : kColDim,
                        "DEADLINE OFFSET %u US  MISS RUN %u  CLEAN %u  FIRED %" PRIu64,
                        rs.deadline_offset_us, rs.consecutive_miss, rs.clean_frames,
                        rs.deadlines_fired);
        hud_->printf_at(0, y++, kColBody, "CONCEALED TOTAL %" PRIu64 "  LATE %" PRIu64,
                        rs.tiles_concealed, rs.tiles_late);
        ++y;

        // ---- telemetry (PAPER 4.9)
        hud_->text(0, y++, "TELEMETRY  P50/P99 OVER 1 S", kColHeading);
        hud_->printf_at(0, y++, kColBody, "DECODE %u/%u US   ENC %u/%u US   SPREAD %u/%u US",
                        tel_.decode_p50, tel_.decode_p99, tel_.enc_p50, tel_.enc_p99,
                        tel_.spread_p50, tel_.spread_p99);
        hud_->printf_at(0, y++, kColDim,
                        "NO CLOCK OFFSET ESTIMATOR: QUEUE/AIR NEED PING-PONG (4.9)");
        hud_->printf_at(0, y++, kColDim, "PRESENT %.1f FPS", fps_);
    }

    // ---------------------------------------------------------- state

    android_app* app_;
    AppConfig    cfg_;
    std::unique_ptr<IDepacketizer> depack_;
    std::unique_ptr<FrameRing>     ring_;
    std::unique_ptr<Receiver>      rx_;
    std::unique_ptr<TextCanvas>    hud_;
    Renderer     renderer_;
    bool         have_device_ = false;

    std::thread       decode_thread_;
    std::atomic<bool> decode_run_{false};

    Telemetry tel_;
    uint16_t  tel_frame_ = 0;
    bool      tel_frame_valid_ = false;
    uint64_t  tel_first_rx_ = 0, tel_last_rx_ = 0;

    SelfTest  st_;
    Receiver::SnmpUdp snmp0_;

    uint64_t last_sample_us_ = 0, last_dgrams_ = 0, last_bytes_ = 0;
    uint64_t last_proc_ticks_ = UINT64_MAX, last_rx_ticks_ = UINT64_MAX;
    uint64_t frames_presented_ = 0, last_frames_ = 0, fb_bytes_ = 0;
    uint32_t cpu_proc_permille_ = 0, cpu_rx_permille_ = 0, report_seq_ = 0;
    double   pps_ = 0, bps_ = 0, fps_ = 0;
};

App* g_app = nullptr;

void handle_cmd(android_app* a, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:  if (g_app) g_app->on_window_created(); break;
        case APP_CMD_TERM_WINDOW:  if (g_app) g_app->on_window_destroyed(); break;
        default: break;
    }
    (void)a;
}

}  // namespace
}  // namespace nxc

extern "C" void android_main(android_app* app) {
    using namespace nxc;

    log_info("NX Warp client shell starting");
    App a(app);
    g_app = &a;
    app->onAppCmd = handle_cmd;

    if (!a.start_net()) {
        log_err("network start failed; the app will still run so the HUD can say so");
    }

    while (true) {
        int events = 0;
        android_poll_source* source = nullptr;
        // Non-blocking poll: the render loop is paced by the FIFO swapchain, and
        // when there is no window it is paced by a short sleep below.
        while (ALooper_pollOnce(0, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) {
                log_info("destroy requested, shutting down");
                a.stop();
                g_app = nullptr;
                return;
            }
        }
        if (app->window) {
            a.frame();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
}
