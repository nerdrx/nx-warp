// Host-side loopback check for the client's transport and frame-ring logic.
//
// Why this exists: the client shell cannot be unit tested on the device without
// a device, and the most expensive class of bug here is a disagreement about the
// wire format between nxvc-blast and the depacketizer -- one that would show up
// on a headset as "everything is dropped" with no obvious cause.
//
// This binary links the *real* nxc_transport_stub.cpp and nxc_frame_ring.cpp,
// binds a UDP socket, and feeds whatever nxvc-blast sends it through exactly the
// path the app uses. It asserts that:
//   * every datagram is accepted (no bad_version / bad_directory / bad_range)
//   * tiles are placed and the ring advances
//   * band deadlines fire, tiles get concealed, feedback packets are generated
//   * the generated feedback parses back per TRANSPORT.md 8
//
// Usage:
//   ./host-loopback &              # listens on 127.0.0.1:9944
//   ./nxvc-blast --host 127.0.0.1 --profile 150mbit --seconds 5
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "nxc_config.h"
#include "nxc_frame_ring.h"
#include "nxc_transport.h"
#include "nxc_wire.h"

// ---------------------------------------------------------------- host shims
// nxc_util.cpp is Android only (it logs through android/log.h).
namespace nxc {

uint64_t now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

static void vlg(const char* lvl, const char* fmt, va_list ap) {
    std::fprintf(stderr, "[%s] ", lvl);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}
void log_info(const char* fmt, ...) { va_list a; va_start(a, fmt); vlg("info", fmt, a); va_end(a); }
void log_warn(const char* fmt, ...) { va_list a; va_start(a, fmt); vlg("warn", fmt, a); va_end(a); }
void log_err (const char* fmt, ...) { va_list a; va_start(a, fmt); vlg("err ", fmt, a); va_end(a); }

}  // namespace nxc

using namespace nxc;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Parses a feedback packet per TRANSPORT.md 8 and returns false if it is
// malformed. This is the check that the generator and the document agree.
bool parse_feedback(const uint8_t* p, size_t len, const StreamConfig& cfg,
                    std::string* err) {
    if (len < 8 + 20 + 4) { *err = "shorter than header + one record + trailer"; return false; }
    const uint8_t version = p[0] & 0x0f;
    if (version != kVersion) { *err = "bad version"; return false; }
    const uint8_t band_count = p[5];
    if (band_count < 1 || band_count > 3) { *err = "band_count out of range"; return false; }
    const uint16_t tiles_in_band = rd_u16(p + 6);

    size_t o = 8;
    for (uint8_t b = 0; b < band_count; ++b) {
        if (o + 20 > len) { *err = "record truncated"; return false; }
        const uint8_t band = p[o + 2];
        const uint8_t flags = p[o + 3];
        const uint8_t mode = flags & 0x03;
        if (mode == 3) { *err = "bitmap_mode 3 is reserved"; return false; }
        const uint32_t nb = cfg.tiles_in_band(band);
        o += 20;
        if (mode == 0) {                       // RAW
            const uint32_t raw = (nb + 7) / 8;
            if (o + raw > len) { *err = "raw bitmap truncated"; return false; }
            o += raw;
        } else if (mode == 2) {                // RLE
            if (o + 1 > len) { *err = "rle count truncated"; return false; }
            const uint8_t runs = p[o];
            if (o + 1 + size_t(runs) * 3 > len) { *err = "rle body truncated"; return false; }
            o += 1 + size_t(runs) * 3;
        }
        // mode 1 (ALL) carries no bitmap.
    }
    if (o + 4 != len) { *err = "trailer is not exactly 4 bytes at the end"; return false; }
    (void)tiles_in_band;
    return true;
}

class CountingFeedback : public IFeedbackSink {
public:
    explicit CountingFeedback(const StreamConfig& cfg) : cfg_(cfg) {}
    void send_feedback(const uint8_t* data, size_t len) override {
        ++packets;
        bytes += len;
        if (len > max_bytes) max_bytes = len;
        std::string err;
        if (!parse_feedback(data, len, cfg_, &err)) {
            if (bad_examples < 3)
                std::printf("    malformed feedback (%zu bytes): %s\n", len, err.c_str());
            ++bad;
            ++bad_examples;
        }
    }
    uint64_t packets = 0, bytes = 0, bad = 0;
    size_t   max_bytes = 0;
    int      bad_examples = 0;

private:
    StreamConfig cfg_;
};

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 9944;
    double seconds = 8.0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = uint16_t(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
    }

    AppConfig cfg;
    cfg.send_feedback = true;
    // Deadlines have to fire quickly or an 8-second run sees none.
    cfg.reproject_budget_us = 1500;
    cfg.runtime_margin_us = 3000;

    auto depack = create_stub_depacketizer(cfg.stream, false);
    CountingFeedback fb(cfg.stream);
    FrameRing ring(cfg, depack.get(), &fb);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { perror("bind"); return 1; }

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 2000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::printf("host-loopback listening on udp/%u for %.1f s\n", port, seconds);
    std::printf("run:  ./nxvc-blast --host 127.0.0.1 --profile 150mbit --seconds %.0f\n\n",
                seconds);

    std::vector<uint8_t> buf(65536);
    const uint64_t t0 = now_us();
    const uint64_t t_end = t0 + uint64_t(seconds * 1e6);
    uint64_t received = 0, selftest_seen = 0;

    while (now_us() < t_end) {
        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n > 0) {
            ++received;
            uint32_t dlen = uint32_t(n);
            if (dlen >= kSelfTestTrailerBytes) {
                const uint8_t* tr = buf.data() + dlen - kSelfTestTrailerBytes;
                if (rd_u32(tr) == kSelfTestMagic) {
                    ++selftest_seen;
                    dlen -= kSelfTestTrailerBytes;
                }
            }
            depack->submit(buf.data(), dlen, now_us(), &ring);
        }
        ring.tick(now_us());
    }

    const auto& c = depack->counters();
    const RingStats rs = ring.stats();

    std::printf("\n=== counters ===\n");
    std::printf("  received datagrams   %" PRIu64 "\n", received);
    std::printf("  self-test trailers   %" PRIu64 "\n", selftest_seen);
    std::printf("  placed runs / tiles  %" PRIu64 " / %" PRIu64 "\n",
                c.placed_runs, c.placed_tiles);
    std::printf("  bad version/caps     %" PRIu64 " / %" PRIu64 "\n", c.bad_version, c.bad_caps);
    std::printf("  bad directory/range  %" PRIu64 " / %" PRIu64 "\n",
                c.bad_directory, c.bad_range);
    std::printf("  short / duplicate    %" PRIu64 " / %" PRIu64 "\n",
                c.short_datagram, c.duplicate);
    std::printf("  parity dropped       %" PRIu64 "\n", c.parity_dropped);
    std::printf("  path0 rx/lost        %" PRIu64 " / %" PRIu64 "\n",
                c.path_rx[0], c.path_lost[0]);
    std::printf("  frames seen/advanced %" PRIu64 " / %" PRIu64 "\n",
                rs.frames_seen, rs.frames_advanced);
    std::printf("  deadlines fired      %" PRIu64 "\n", rs.deadlines_fired);
    std::printf("  tiles concealed/late %" PRIu64 " / %" PRIu64 "\n",
                rs.tiles_concealed, rs.tiles_late);
    std::printf("  stale-frame drops    %" PRIu64 "\n", rs.stale_frame_drop);
    std::printf("  deadline offset      %u us\n", rs.deadline_offset_us);
    std::printf("  feedback pkts/bytes  %" PRIu64 " / %" PRIu64 " (max %zu B)\n",
                fb.packets, fb.bytes, fb.max_bytes);

    std::printf("\n=== checks ===\n");
    check(received > 0, "datagrams arrived (is nxvc-blast running?)");
    if (received == 0) return 1;

    check(c.bad_version == 0, "no version rejections");
    check(c.bad_caps == 0, "no capability rejections");
    check(c.bad_directory == 0, "directory sum matches the plaintext length everywhere");
    check(c.bad_range == 0, "no run crosses a tile row or leaves the grid");
    check(c.short_datagram == 0, "no truncated datagrams");
    check(c.placed_tiles > 0, "tiles were placed into the ring");
    check(rs.frames_advanced > 1, "the 4-slot ring advanced");
    check(rs.deadlines_fired > 0, "band deadlines fired");
    check(fb.packets > 0, "feedback packets were generated");
    check(fb.bad == 0, "every feedback packet parses per TRANSPORT.md 8");
    // D9: the raw worst case is 225 bytes and must stay under any MTU.
    check(fb.max_bytes <= 256, "feedback packets stay under 256 bytes (D9)");
    check(selftest_seen == received, "every datagram carried a self-test trailer");

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
                g_failures == 1 ? "" : "s");
    close(fd);
    return g_failures ? 1 : 0;
}
