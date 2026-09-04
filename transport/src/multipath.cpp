#include "nxvc/transport/multipath.h"

#include <algorithm>
#include <limits>

namespace nxt {

void Striper::configure_path(uint8_t id, double rate_bps, uint32_t rtt_us) {
    if (id >= kMaxPaths) return;
    PathInfo& p = paths_[id];
    p.configured = true;
    p.up = true;
    p.rate_bps = rate_bps;
    p.rtt_us = rtt_us;
    p.rtt_base_us = rtt_us;
    p.stalled = false;
}

void Striper::update_rtt(uint8_t id, uint32_t rtt_us) {
    if (id >= kMaxPaths) return;
    PathInfo& p = paths_[id];
    p.rtt_us = rtt_us;
    if (p.rtt_base_us == 0 || rtt_us < p.rtt_base_us) p.rtt_base_us = rtt_us;
    if (p.rtt_base_us && rtt_us > 3 * p.rtt_base_us) {
        p.stalled = true;
        p.probe_bands = 0;
    } else if (p.stalled && p.rtt_base_us && rtt_us < (p.rtt_base_us * 3) / 2) {
        if (++p.probe_bands >= 2) { p.stalled = false; p.probe_bands = 0; }
    }
}

void Striper::note_rx(uint8_t id, uint64_t now_us) {
    if (id >= kMaxPaths) return;
    paths_[id].last_rx_us = now_us;
}

void Striper::tick_band(uint64_t now_us) {
    uint64_t newest = 0;
    for (const PathInfo& p : paths_)
        if (p.configured) newest = std::max(newest, p.last_rx_us);
    for (PathInfo& p : paths_) {
        if (!p.configured || !p.up) continue;
        bool other_flowing = newest > p.last_rx_us && now_us - newest < 20000;
        if (other_flowing && now_us > p.last_rx_us && now_us - p.last_rx_us > 20000) {
            p.stalled = true;
            p.probe_bands = 0;
        }
    }
}

int Striper::up_paths() const {
    int n = 0;
    for (const PathInfo& p : paths_)
        if (p.configured && p.up && !p.stalled) ++n;
    return n;
}

void Striper::begin_band(size_t band_bytes, size_t class_a_bytes,
                         uint32_t band_period_us) {
    dup_a_ = false;
    if (up_paths() < 2) return;
    double cap = 0;
    for (const PathInfo& p : paths_)
        if (p.configured && p.up && !p.stalled)
            cap += p.rate_bps * (double(band_period_us) * 1e-6) / 8.0;
    double need = double(band_bytes) + double(class_a_bytes);
    dup_a_ = need <= cap;
}

std::vector<uint8_t> Striper::assign(const SendUnit& u) {
    std::vector<uint8_t> out;
    // A single-path integration need not call configure_path at all: path 0 is
    // implicitly configured so the sender never silently drops a band.
    if (!paths_[0].configured) {
        bool any = false;
        for (const PathInfo& p : paths_) any = any || p.configured;
        if (!any) {
            out.push_back(0);
            sent_[0] += double(u.bytes());
            return out;
        }
    }
    std::vector<uint8_t> live;
    for (uint8_t i = 0; i < kMaxPaths; ++i)
        if (paths_[i].configured && paths_[i].up && !paths_[i].stalled) live.push_back(i);
    if (live.empty()) {
        // Everything stalled: probe with class C only, on the first configured path.
        for (uint8_t i = 0; i < kMaxPaths; ++i)
            if (paths_[i].configured && paths_[i].up) { live.push_back(i); break; }
        if (live.empty()) return out;
        if (u.cls != TileClass::kC) { out.push_back(live[0]); return out; }
        out.push_back(live[0]);
        return out;
    }

    size_t bytes = u.bytes();
    if (u.cls == TileClass::kA && dup_a_ && live.size() >= 2) {
        for (uint8_t p : live) {
            out.push_back(p);
            sent_[p] += double(bytes);
        }
        return out;
    }

    // Weighted least loaded: minimise sent_bytes / rate.
    uint8_t best = live[0];
    double bestv = std::numeric_limits<double>::max();
    for (uint8_t p : live) {
        double w = paths_[p].rate_bps > 1.0 ? paths_[p].rate_bps : 1.0;
        double v = sent_[p] / w;
        if (v < bestv) { bestv = v; best = p; }
    }
    sent_[best] += double(bytes);
    out.push_back(best);
    return out;
}

}  // namespace nxt
