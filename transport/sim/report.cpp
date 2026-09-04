#include "report.h"

#include <cstdio>
#include <string>

namespace nxsim {

void write_results(const std::string& path, const std::vector<ScenarioResult>& rows,
                   const std::string& preamble, const std::string& notes) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "netsim: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "# NX Warp transport: simulator results\n\n%s\n\n", preamble.c_str());

    std::fprintf(f,
                 "## Rate, overhead and FEC\n\n"
                 "| scenario | link | loss offered / measured | Mbit/s on wire | dg/s |"
                 " tiles/run | header+dir overhead | overhead incl FEC | overhead incl FEC+IP/UDP | FEC parity |"
                 " FEC parity bytes | repaired bytes (of which needed) |\n"
                 "|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (const auto& r : rows)
        std::fprintf(f,
                     "| %s | %s | %.1f%% / %.2f%% | %.1f | %.0f | %.1f | %.2f%% | %.2f%% | %.2f%% |"
                     " %.1f%% | %.2f MB | %.2f MB (%.2f MB) |\n",
                     r.name.c_str(), r.link_desc.c_str(), r.target_loss_pct,
                     r.measured_loss_pct, r.bitrate_mbps, r.datagram_rate, r.tiles_per_run,
                     r.hdr_overhead_pct, r.overhead_pct, r.overhead_pct_ip, r.fec_overhead_pct,
                     r.fec_bytes / 1e6, r.fec_recovered_bytes / 1e6, r.fec_useful_bytes / 1e6);

    std::fprintf(f,
                 "\n## Concealment, references, feedback and latency\n\n"
                 "| scenario | concealed tiles/frame | late tiles/frame |"
                 " ref N-1 | ref N-2 | ref N-3 | intra | feedback | mean fb bytes |"
                 " band latency p50 | p99 | deadline offset | shadow mismatches |\n"
                 "|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (const auto& r : rows)
        std::fprintf(f,
                     "| %s | %.1f | %.1f | %.1f%% | %.1f%% | %.1f%% | %.1f%% |"
                     " %.2f Mbit/s | %.0f | %.2f ms | %.2f ms | %.1f ms | %.0f |\n",
                     r.name.c_str(), r.conceal_per_frame, r.late_per_frame, r.ref_pct[0],
                     r.ref_pct[1], r.ref_pct[2], r.ref_pct[3], r.feedback_mbps,
                     r.feedback_mean_bytes, r.band_latency_p50_us / 1000.0,
                     r.band_latency_p99_us / 1000.0, r.deadline_offset_us / 1000.0,
                     r.shadow_mismatches);

    std::fprintf(f,
                 "\n## Class mix and multipath\n\n"
                 "| scenario | class A bits | class B bits | class C bits |"
                 " duplicated datagrams | bytes on path 0 | path 1 | mean tile bytes |\n"
                 "|---|---|---|---|---|---|---|---|\n");
    for (const auto& r : rows)
        std::fprintf(f,
                     "| %s | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %.1f%% |"
                     " %.1f |\n",
                     r.name.c_str(), r.class_bit_share[0], r.class_bit_share[1],
                     r.class_bit_share[2], r.dup_pct, r.path_share[0], r.path_share[1],
                     r.mean_tile_bytes);

    if (!notes.empty()) std::fprintf(f, "\n%s\n", notes.c_str());
    std::fclose(f);
}

}  // namespace nxsim
