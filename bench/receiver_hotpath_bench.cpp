#include "nxvc/transport/receiver.h"
#include "nxvc/transport/sender.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace nxt;

namespace {

constexpr int kFrames = 16;
constexpr int kRepeats = 20;
constexpr size_t kTileBytes = 51;

struct Workload {
    StreamConfig cfg;
    std::vector<std::vector<Datagram>> frames;
    size_t wire_bytes = 0;
    size_t datagrams = 0;
    size_t tile_bytes = 0;
};

StreamConfig config(bool fec) {
    StreamConfig c;
    c.cols = 136;
    c.rows = 68;
    c.band_rows = 68;
    c.layers = 1;
    c.mtu = 1024;
    c.caps = kCapPoseHdr | (fec ? kCapFec : 0);
    return c;
}

Workload make_workload(bool fec, bool trusted) {
    Workload w{.cfg = config(fec)};
    auto aead = trusted ? make_trusted_lan_aead() : make_null_aead();
    Key key{}, salt{};
    Sender tx(w.cfg, aead.get(), key, salt);
    tx.set_auto_fec(false);

    std::vector<uint8_t> tile(kTileBytes);
    for (size_t i = 0; i < tile.size(); ++i) tile[i] = uint8_t(i * 17 + 3);
    std::vector<TileInput> tiles;
    tiles.reserve(w.cfg.tiles_per_frame());
    for (uint16_t row = 0; row < w.cfg.rows; ++row)
        for (uint16_t col = 0; col < w.cfg.cols; ++col) {
            TileInput t;
            t.row = row;
            t.col = col;
            t.cls = TileClass::kA;
            t.bytes = tile;
            tiles.push_back(t);
        }

    w.tile_bytes = tiles.size() * kTileBytes;
    for (uint16_t frame = 0; frame < kFrames; ++frame) {
        PoseHeader pose;
        tx.begin_frame(frame, pose, 0, 0);
        for (TileInput& t : tiles) t.frame_id = frame;
        w.frames.push_back(tx.send_band(0, tiles, 100, 10, true));
        for (const Datagram& d : w.frames.back()) {
            w.wire_bytes += d.bytes.size();
            ++w.datagrams;
        }
    }
    return w;
}

uint64_t mix(uint64_t h, uint8_t v) {
    h ^= uint64_t(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t run(const Workload& w, bool trusted, double* ms, bool* valid) {
    auto aead = trusted ? make_trusted_lan_aead() : make_null_aead();
    Key key{}, salt{};
    std::vector<TileOutput> out;
    uint64_t checksum = 0xcbf29ce484222325ull;
    *valid = true;
    double total_ms = 0;
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        Receiver rx(w.cfg, aead.get(), key, salt);
        const auto start = std::chrono::steady_clock::now();
        for (size_t frame_id = 0; frame_id < w.frames.size(); ++frame_id) {
            const auto& frame = w.frames[frame_id];
            size_t output_count = 0;
            out.clear();
            for (const Datagram& d : frame) {
                if (!rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                                    d.path_id, uint64_t(repeat) * 1000000 + d.tx_ts, &out))
                    *valid = false;
                for (const TileOutput& t : out) {
                    ++output_count;
                    checksum = mix(checksum, uint8_t(t.row));
                    checksum = mix(checksum, uint8_t(t.col));
                    checksum = mix(checksum, uint8_t(t.bytes.size()));
                    if (!t.bytes.empty()) {
                        checksum = mix(checksum, t.bytes.front());
                        checksum = mix(checksum, t.bytes.back());
                    }
                }
                out.clear();
            }
            if (output_count != w.cfg.tiles_per_frame()) *valid = false;
            ByteVec report = rx.band_deadline(uint16_t(frame_id), 0,
                                              uint64_t(repeat) * 1000000 + 1000, 40, 0);
            for (uint8_t b : report) checksum = mix(checksum, b);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        total_ms += std::chrono::duration<double, std::milli>(elapsed).count();
    }
    *ms = total_ms;
    return checksum;
}

void bench(bool fec, bool trusted) {
    const Workload w = make_workload(fec, trusted);
    double ms = 0;
    bool valid = false;
    const uint64_t checksum = run(w, trusted, &ms, &valid);
    const char* name = trusted ? "trusted-fec" : (fec ? "null-fec" : "null-nofec");
    std::printf("%s packets=%zu frame_tile_bytes=%zu wire_bytes=%zu repeats=%d elapsed_ms=%.3f checksum=%016llx valid=%s\n",
                name, w.datagrams / kFrames, w.tile_bytes, w.wire_bytes / kFrames,
                kRepeats, ms, (unsigned long long)checksum, valid ? "yes" : "no");
}

} // namespace

int main() {
    bench(false, false);
    bench(true, false);
    bench(true, true);
}
