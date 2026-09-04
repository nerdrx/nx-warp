// nxvc-blast -- flood an NX Warp client with paper-realistic datagrams.
//
// This is the host half of the pps/throughput self test. PAPER 4.1 claims "the
// Android UDP receive path on an XR2 tops out between 50k and 100k packets/s
// even with recvmmsg", and PAPER 4.11 puts the saturation point around 80k pps
// on one core. Those numbers decide whether 1 Gbit/s over WiFi is reachable, and
// they have never been measured on a real device.
//
// The tool generates well-formed NX Warp v1 datagrams (docs/TRANSPORT.md
// sections 2 and 3) at a configurable rate, with run lengths derived from a
// target bitrate the way the paper derives them, so the packet rate and the
// packet size move together the way they would in a real session. It appends a
// 16-byte self-test trailer after the datagram so the client has ground-truth
// sequencing that does not depend on the 14-bit path_seq.
//
// It is NOT an encoder: payload bytes are noise. It measures the receive path.
//
// Build:  cmake -S . -B build && cmake --build build -j4
// Run:    ./build/nxvc-blast --host 192.168.1.50 --profile 400mbit --seconds 20
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- wire
// Transcribed from docs/TRANSPORT.md sections 1, 2 and 3. Kept local so this
// tool builds standalone on a host with no Android or Vulkan toolchain.

constexpr uint8_t  kVersion       = 1;
constexpr uint32_t kHeaderBytes   = 24;
constexpr uint32_t kTagBytes      = 16;
constexpr uint32_t kPoseHdrBytes  = 26;
constexpr uint32_t kDirEntryBytes = 4;

constexpr uint32_t kCols          = 68;    // ceil(4320 / 64)
constexpr uint32_t kRows          = 34;    // ceil(2160 / 64)
constexpr uint32_t kTiles         = kCols * kRows;   // 2312
constexpr uint32_t kBandRows      = 6;
constexpr uint32_t kBands         = 6;

constexpr uint32_t kSelfTestMagic = 0x4e584254u;  // "NXBT"
constexpr uint32_t kStatsMagic    = 0x4e585253u;  // "NXRS"
constexpr uint32_t kTrailerBytes  = 16;

inline void wr_u16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
// The client's stats report (nxc_wire.h StatsReport), little endian.
struct StatsReport {
    uint32_t magic, seq;
    uint64_t window_us, rx_datagrams, rx_bytes, lost_datagrams, reordered;
    uint32_t rcvbuf_granted, sched_policy, affinity_mask;
    uint32_t cpu_permille_proc, cpu_permille_rx, drops_ring_full, udp_rcvbuf_errors, pad;
};

// ---------------------------------------------------------------- time

uint64_t now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

void sleep_until_us(uint64_t t) {
    const uint64_t n = now_us();
    if (t <= n) return;
    const uint64_t d = t - n;
    // Below ~80 us a nanosleep costs more than it saves; spin instead so the
    // pacing at 90 kpps is not dominated by wakeup jitter.
    if (d > 120) {
        timespec ts{};
        ts.tv_sec = time_t((d - 80) / 1000000ull);
        ts.tv_nsec = long(((d - 80) % 1000000ull) * 1000ull);
        nanosleep(&ts, nullptr);
    }
    while (now_us() < t) { /* spin */ }
}

// ---------------------------------------------------------------- config

struct Config {
    std::string host = "127.0.0.1";
    uint16_t    port = 9944;
    uint32_t    mtu = 1400;          // PAPER 4.1 budget
    double      mbits = 400.0;       // target downstream bitrate
    double      pps_override = 0;    // if set, ignore the bitrate-derived rate
    double      seconds = 20.0;
    uint32_t    fps = 90;
    uint32_t    paths = 1;           // PAPER 4.8, class A duplication is not simulated
    bool        pose_hdr = true;     // TRANSPORT.md 3.3, first datagram of a band
    uint32_t    batch = 64;
    bool        quiet = false;
};

void usage() {
    std::printf(
        "nxvc-blast -- NX Warp client receive-path self test\n"
        "\n"
        "  --host <ip>        target device (default 127.0.0.1)\n"
        "  --port <n>         target port (default 9944)\n"
        "  --mbps <f>         target downstream bitrate; sets run length and pps\n"
        "  --pps <f>          override the packet rate, keeping the datagram size\n"
        "  --mtu <n>          path MTU payload budget (default 1400; try 8900 on USB)\n"
        "  --seconds <f>      run length (default 20)\n"
        "  --fps <n>          frame rate the pacing is built around (default 90)\n"
        "  --paths <1|2>      spread datagrams over N path_ids (default 1)\n"
        "  --batch <n>        sendmmsg batch (default 64)\n"
        "  --profile <name>   150mbit | 400mbit | 1gbit | jumbo | ceiling\n"
        "  --quiet            only print the final summary\n"
        "\n"
        "Profiles follow PAPER 4.1: 150 Mbit/s is about 13.5 kpps, 1 Gbit/s about\n"
        "90 kpps, and 'jumbo' is the 8900-byte USB path where pps falls by 6x.\n"
        "'ceiling' ramps the rate to find where the device starts dropping.\n");
}

bool apply_profile(Config* c, const std::string& p) {
    if (p == "150mbit")      { c->mbits = 150;  c->mtu = 1400; }
    else if (p == "400mbit") { c->mbits = 400;  c->mtu = 1400; }
    else if (p == "1gbit")   { c->mbits = 1000; c->mtu = 1400; }
    else if (p == "jumbo")   { c->mbits = 1000; c->mtu = 8900; }
    else if (p == "ceiling") { c->mbits = 1000; c->mtu = 1400; }
    else return false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    bool ceiling = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--host")    { const char* v = next(); if (v) cfg.host = v; }
        else if (a == "--port")    { const char* v = next(); if (v) cfg.port = uint16_t(atoi(v)); }
        else if (a == "--mtu")     { const char* v = next(); if (v) cfg.mtu = uint32_t(atoi(v)); }
        else if (a == "--mbps")    { const char* v = next(); if (v) cfg.mbits = atof(v); }
        else if (a == "--pps")     { const char* v = next(); if (v) cfg.pps_override = atof(v); }
        else if (a == "--seconds") { const char* v = next(); if (v) cfg.seconds = atof(v); }
        else if (a == "--fps")     { const char* v = next(); if (v) cfg.fps = uint32_t(atoi(v)); }
        else if (a == "--paths")   { const char* v = next(); if (v) cfg.paths = uint32_t(atoi(v)); }
        else if (a == "--batch")   { const char* v = next(); if (v) cfg.batch = uint32_t(atoi(v)); }
        else if (a == "--quiet")   { cfg.quiet = true; }
        else if (a == "--profile") {
            const char* v = next();
            if (!v || !apply_profile(&cfg, v)) { std::fprintf(stderr, "bad profile\n"); return 2; }
            ceiling = (std::string(v) == "ceiling");
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (cfg.paths < 1) cfg.paths = 1;
    if (cfg.paths > 2) cfg.paths = 2;
    if (cfg.batch < 1) cfg.batch = 1;

    // ---------------------------------------------------------- geometry
    //
    // PAPER 4.1: run_payload_budget = mtu - header - tag (no FEC here, the stub
    // client does not negotiate CAP_FEC so parity would just be dropped).
    // TRANSPORT.md 5 rounds the budget down to a multiple of 4 so the directory
    // stays aligned.
    if (cfg.mtu < kHeaderBytes + kTagBytes + 64) {
        std::fprintf(stderr, "mtu %u is too small\n", cfg.mtu);
        return 2;
    }
    uint32_t budget = cfg.mtu - kHeaderBytes - kTagBytes;
    budget &= ~3u;

    const double bytes_per_frame = cfg.mbits * 1e6 / 8.0 / double(cfg.fps);
    const double avg_tile_bytes = bytes_per_frame / double(kTiles);

    // How many tiles fit in one run: each costs its bitstream plus a 4-byte
    // directory entry, and a run may not cross a tile row (TRANSPORT.md 3.2).
    uint32_t tiles_per_run = 1;
    if (avg_tile_bytes + kDirEntryBytes > 0) {
        const double room = double(budget) - double(kPoseHdrBytes);
        tiles_per_run = uint32_t(room / (avg_tile_bytes + double(kDirEntryBytes)));
    }
    tiles_per_run = std::clamp(tiles_per_run, 1u, std::min(kCols, 255u));

    const uint32_t runs_per_frame = (kTiles + tiles_per_run - 1) / tiles_per_run;
    double target_pps = double(runs_per_frame) * double(cfg.fps);
    if (cfg.pps_override > 0) target_pps = cfg.pps_override;

    const uint32_t tile_bytes = uint32_t(std::max(1.0, avg_tile_bytes));

    std::printf("nxvc-blast -> %s:%u\n", cfg.host.c_str(), cfg.port);
    std::printf("  mtu %u, run budget %u, avg tile %.1f B, %u tiles/run\n",
                cfg.mtu, budget, avg_tile_bytes, tiles_per_run);
    std::printf("  %.0f Mbit/s at %u fps = %u runs/frame = %.0f pps target\n",
                cfg.mbits, cfg.fps, runs_per_frame, target_pps);
    if (ceiling)
        std::printf("  CEILING MODE: ramping the rate until the device reports loss\n");

    // ---------------------------------------------------------- socket
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int sndbuf = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg.port);
    if (inet_pton(AF_INET, cfg.host.c_str(), &dst.sin_addr) != 1) {
        std::fprintf(stderr, "bad host address: %s\n", cfg.host.c_str());
        return 2;
    }

    // ---------------------------------------------------------- buffers
    //
    // One preallocated datagram per batch slot. Only the fields that change per
    // datagram are patched, so the per-packet cost on the sender stays well
    // below the receiver's -- otherwise this tool would be measuring itself.
    const uint32_t dg_max = kHeaderBytes + kPoseHdrBytes +
                            tiles_per_run * (kDirEntryBytes + tile_bytes) +
                            kTagBytes + kTrailerBytes;
    std::vector<uint8_t> slab(size_t(cfg.batch) * dg_max);
    std::vector<mmsghdr> msgs(cfg.batch);
    std::vector<iovec>   iov(cfg.batch);

    // Fill payload areas with noise once. The client does not decode it.
    for (size_t i = 0; i < slab.size(); ++i) slab[i] = uint8_t(i * 31 + 7);

    uint32_t abs_seq = 0;
    uint16_t frame_id = 0;
    uint32_t tile_cursor = 0;
    uint16_t path_seq[2] = {0, 0};
    uint32_t path_rr = 0;

    uint64_t sent = 0, send_errors = 0, bytes_sent = 0;
    const uint64_t t0 = now_us();
    const uint64_t t_end = t0 + uint64_t(cfg.seconds * 1e6);
    uint64_t next_report = t0 + 1000000ull;
    uint64_t last_sent = 0;
    double   cur_pps = target_pps;
    if (ceiling) cur_pps = target_pps * 0.25;

    // What the device last told us, and what came back on the uplink.
    uint64_t last_device_lost = 0, last_device_drops = 0;
    uint64_t feedback_rx = 0, feedback_bytes = 0;
    bool     got_report = false;
    bool     warned_no_report = false;

    // Reports come back on the same socket.
    auto drain_reports = [&]() {
        uint8_t buf[2048];
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        for (;;) {
            ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                                 reinterpret_cast<sockaddr*>(&from), &fl);
            if (n <= 0) break;
            if (n >= ssize_t(sizeof(StatsReport)) && rd_u32(buf) == kStatsMagic) {
                StatsReport r{};
                std::memcpy(&r, buf, sizeof(r));
                const double window_s = double(r.window_us) / 1e6;
                std::printf(
                    "  [device] rx %" PRIu64 " dgrams, %.2f Mbit/s, lost %" PRIu64
                    " reord %" PRIu64 ", ringfull %u, snmp_rcvbuf_err %u,\n"
                    "           rcvbuf %u KB, sched %s, affinity 0x%x, cpu proc %.1f%% rx %.1f%%\n",
                    r.rx_datagrams,
                    window_s > 0 ? double(r.rx_bytes) * 8.0 / 1e6 / window_s : 0.0,
                    r.lost_datagrams, r.reordered, r.drops_ring_full, r.udp_rcvbuf_errors,
                    r.rcvbuf_granted / 1024, r.sched_policy ? "FIFO" : "OTHER",
                    r.affinity_mask, r.cpu_permille_proc / 10.0, r.cpu_permille_rx / 10.0);
                last_device_lost = r.lost_datagrams;
                last_device_drops = r.drops_ring_full;
                got_report = true;
            } else if (n >= 8 && (buf[0] & 0x0f) == kVersion) {
                // A feedback packet (TRANSPORT.md 8). Counted, not parsed: the
                // encoder-side consumer of these lives in transport/.
                ++feedback_rx;
                feedback_bytes += uint64_t(n);
            }
        }
    };

    // ---------------------------------------------------------- send loop
    uint64_t t_next = t0;
    while (now_us() < t_end) {
        const uint32_t n = cfg.batch;
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t* d = slab.data() + size_t(i) * dg_max;

            // How many tiles this run carries: bounded by the run budget, by the
            // end of the tile row (TRANSPORT.md 3.2) and by the end of the frame.
            const uint32_t row = tile_cursor / kCols;
            const uint32_t col = tile_cursor % kCols;
            uint32_t count = std::min(tiles_per_run, kCols - col);
            count = std::min(count, kTiles - tile_cursor);
            if (count == 0) count = 1;

            const uint32_t band = std::min(row / kBandRows, kBands - 1);
            const bool with_pose = cfg.pose_hdr && (col == 0) &&
                                   (row % kBandRows == 0);

            const uint32_t dir_bytes = count * kDirEntryBytes;
            const uint32_t pose_bytes = with_pose ? kPoseHdrBytes : 0;
            const uint32_t payload_len = pose_bytes + dir_bytes + count * tile_bytes;

            // ---- 24-byte header (TRANSPORT.md 2)
            std::memset(d, 0, kHeaderBytes);
            d[0] = kVersion;                      // flags nibble 0
            if (tile_cursor + count >= kTiles) d[0] |= uint8_t(0x08u << 4);  // LAST_RUN_OF_FRAME
            d[1] = 0;                             // stream_id
            wr_u16(d + 2, frame_id);
            wr_u16(d + 4, uint16_t(tile_cursor));
            d[6] = uint8_t(count);
            d[7] = 0;                             // layer 0, ref_delta 0, frag_idx 0
            d[8] = uint8_t((0u) |                 // frag_count 0
                           ((band < 2 ? 0u : (band < 4 ? 1u : 2u)) << 2) |  // tile_class A/B/C
                           ((band & 0x07u) << 4) |
                           ((with_pose ? 1u : 0u) << 7));
            d[9] = 0;                             // caps: negotiate nothing
            wr_u16(d + 10, frame_id);             // pose_seq: one pose per frame
            const uint32_t pid = (cfg.paths > 1) ? (path_rr++ & 1u) : 0u;
            wr_u16(d + 12, uint16_t((path_seq[pid] & 0x3fff) | (pid << 14)));
            path_seq[pid] = uint16_t((path_seq[pid] + 1) & 0x3fff);
            d[14] = 0;                            // fec_group
            d[15] = 0;                            // fec_idx / fec_k = 0, no FEC
            wr_u32(d + 16, uint32_t(now_us()));   // tx_ts
            wr_u16(d + 20, uint16_t(payload_len));
            wr_u16(d + 22, uint16_t(500));        // enc_us, plausible constant

            uint32_t off = kHeaderBytes;
            if (with_pose) {
                // TRANSPORT.md 3.3. Only render_finish_ts is read by the client.
                std::memset(d + off, 0, kPoseHdrBytes);
                wr_u16(d + off, frame_id);
                d[off + 8] = 0x7f;                // w component of the quaternion
                wr_u32(d + off + 22, uint32_t(now_us()));
                off += kPoseHdrBytes;
            }
            // ---- tile directory (TRANSPORT.md 3.1): len in [11:0], qp in [17:12]
            for (uint32_t k = 0; k < count; ++k) {
                const uint32_t w = (tile_bytes & 0xfffu) | (uint32_t(28) << 12) |
                                   (uint32_t(2) << 18);   // mode 2 = WARP_MV
                wr_u32(d + off + k * kDirEntryBytes, w);
            }
            off += dir_bytes;
            off += count * tile_bytes;            // bitstreams: already noise
            off += kTagBytes;                     // AEAD tag: the stub does not check it

            // ---- self-test trailer, after the datagram proper
            wr_u32(d + off + 0, kSelfTestMagic);
            wr_u32(d + off + 4, abs_seq++);
            const uint64_t ts = now_us();
            wr_u32(d + off + 8, uint32_t(ts));
            wr_u32(d + off + 12, uint32_t(ts >> 32));
            off += kTrailerBytes;

            iov[i].iov_base = d;
            iov[i].iov_len = off;
            std::memset(&msgs[i].msg_hdr, 0, sizeof(msghdr));
            msgs[i].msg_hdr.msg_iov = &iov[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            msgs[i].msg_hdr.msg_name = &dst;
            msgs[i].msg_hdr.msg_namelen = sizeof(dst);

            tile_cursor += count;
            if (tile_cursor >= kTiles) { tile_cursor = 0; ++frame_id; }
        }

        int got = sendmmsg(fd, msgs.data(), n, 0);
        if (got < 0) {
            if (errno == ENOBUFS || errno == EAGAIN) {
                // The local send buffer is the bottleneck, not the device.
                ++send_errors;
                usleep(200);
            } else {
                perror("sendmmsg");
                break;
            }
        } else {
            sent += uint32_t(got);
            for (int i = 0; i < got; ++i) bytes_sent += msgs[i].msg_hdr.msg_iov->iov_len;
        }

        // Pace to cur_pps.
        t_next += uint64_t(double(n) * 1e6 / cur_pps);
        const uint64_t nowv = now_us();
        if (t_next < nowv) t_next = nowv;   // we are behind; do not bank credit
        sleep_until_us(t_next);

        drain_reports();

        if (now_us() >= next_report) {
            const double dt = 1.0;
            const double achieved = double(sent - last_sent) / dt;
            if (!cfg.quiet) {
                std::printf("[host] target %.0f pps, achieved %.0f pps, %.2f Mbit/s%s\n",
                            cur_pps, achieved,
                            achieved * double(dg_max) * 8.0 / 1e6,
                            send_errors ? "  (ENOBUFS backpressure)" : "");
            }
            if (ceiling) {
                // Ramp until the device reports loss, or until the host itself
                // cannot keep up (in which case the number measured would be the
                // sender's ceiling, not the receiver's).
                const bool host_ok = achieved > cur_pps * 0.9;

                if (!got_report) {
                    // No stats report is NOT evidence of loss -- it usually means
                    // the app is not running, or the uplink is blocked. Keep
                    // ramping on the host-side signal alone and say so, rather
                    // than reporting a ceiling that was never measured.
                    if (!warned_no_report) {
                        std::printf("       WARNING: no stats reports from the device yet; "
                                    "ramping on host-side pacing alone.\n"
                                    "       The 'ceiling' found this way is the SENDER's, "
                                    "not the receiver's.\n");
                        warned_no_report = true;
                    }
                    if (host_ok && cur_pps < target_pps * 4) {
                        cur_pps *= 1.35;
                        std::printf("       ramping to %.0f pps\n", cur_pps);
                    } else if (!host_ok) {
                        std::printf("       host cannot sustain %.0f pps (achieved %.0f) -- "
                                    "sender bound, not a device result\n", cur_pps, achieved);
                        ceiling = false;
                    }
                } else if (last_device_lost == 0 && last_device_drops == 0) {
                    if (host_ok && cur_pps < target_pps * 4) {
                        cur_pps *= 1.35;
                        std::printf("       ramping to %.0f pps\n", cur_pps);
                    } else if (!host_ok) {
                        std::printf("       host cannot sustain %.0f pps (achieved %.0f); "
                                    "the device is still clean, so this is a SENDER bound\n",
                                    cur_pps, achieved);
                        ceiling = false;
                    }
                } else {
                    std::printf("       device reporting loss (%" PRIu64 " lost, %" PRIu64
                                " ring-full) at %.0f pps -- receive-path ceiling found\n",
                                last_device_lost, last_device_drops, cur_pps);
                    ceiling = false;
                }
            }
            last_sent = sent;
            next_report += 1000000ull;
        }
    }

    // Let the last reports arrive.
    for (int i = 0; i < 20; ++i) { drain_reports(); usleep(50000); }

    const double elapsed = double(now_us() - t0) / 1e6;
    std::printf("\n=== nxvc-blast summary ===\n");
    std::printf("  elapsed        %.2f s\n", elapsed);
    std::printf("  datagrams sent %" PRIu64 " (%.0f pps)\n", sent, double(sent) / elapsed);
    std::printf("  bytes sent     %" PRIu64 " (%.2f Mbit/s)\n", bytes_sent,
                double(bytes_sent) * 8.0 / 1e6 / elapsed);
    std::printf("  sendmmsg backpressure events %" PRIu64 "\n", send_errors);
    std::printf("  feedback packets received    %" PRIu64 " (%" PRIu64 " bytes, %.3f Mbit/s)\n",
                feedback_rx, feedback_bytes,
                double(feedback_bytes) * 8.0 / 1e6 / elapsed);
    if (got_report) {
        std::printf("  device last report: lost %" PRIu64 ", ring-full drops %" PRIu64 "\n",
                    last_device_lost, last_device_drops);
    } else {
        std::printf("  device sent no stats reports (is the app running and reachable?)\n");
    }
    close(fd);
    return 0;
}
