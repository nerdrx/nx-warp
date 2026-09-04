#ifndef NXVC_SIM_REPORT_H
#define NXVC_SIM_REPORT_H

#include <string>
#include <vector>

namespace nxsim {

struct ScenarioResult {
    std::string name;
    std::string link_desc;
    double target_loss_pct = 0;
    double measured_loss_pct = 0;
    double bitrate_mbps = 0;
    double datagram_rate = 0;      // datagrams per second, both paths
    double tiles_per_run = 0;
    double mean_tile_bytes = 0;
    double overhead_pct = 0;       // (wire - tile payload) / wire, no IP/UDP
    double overhead_pct_ip = 0;    // including 28 bytes of IPv4+UDP per datagram
    double hdr_overhead_pct = 0;   // data datagrams only, no FEC, no IP/UDP
    double fec_overhead_pct = 0;   // parity bytes / data datagram bytes
    double fec_bytes = 0;
    double fec_recovered_bytes = 0;
    double fec_useful_bytes = 0;
    double conceal_per_frame = 0;
    double late_per_frame = 0;
    double ref_pct[4] = {0, 0, 0, 0};  // ref_delta 0,1,2 and intra
    double feedback_mbps = 0;
    double feedback_mean_bytes = 0;
    double band_latency_p50_us = 0;
    double band_latency_p99_us = 0;
    double class_bit_share[3] = {0, 0, 0};
    double dup_pct = 0;
    double path_share[2] = {0, 0};
    double shadow_mismatches = 0;
    double deadline_offset_us = 0;
};

void write_results(const std::string& path, const std::vector<ScenarioResult>& rows,
                   const std::string& preamble, const std::string& notes);

}  // namespace nxsim

#endif
