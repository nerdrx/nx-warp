// The Phase 0 table (PAPER 3.4) and the JSON the runner scripts consume.
#include "nxb_bench.h"

#include <cstdio>
#include <sstream>

namespace nxb {

namespace {

std::string esc(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

std::string num(double v, int prec = 3)
{
    char b[64];
    snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

// The pass/fail rule for one kernel, exactly as PAPER 3.4 states it.
// Returns "PASS", "FAIL" or "info".
std::string statusOf(const KernelResult& r)
{
    if (!r.ran) return "skip";
    if (r.thresholdGbps > 0) return r.gbPerSec >= r.thresholdGbps ? "PASS" : "FAIL";
    if (r.thresholdP50 <= 0) return "info";
    bool ok = r.p50 < r.thresholdP50;
    if (r.thresholdP99 > 0 && r.p99 >= r.thresholdP99) ok = false;
    return ok ? "PASS" : "FAIL";
}

} // namespace

std::string verdictFor(const std::vector<KernelResult>& results)
{
    for (const auto& r : results)
    {
        if (r.name != kidName(K5_FULL) || !r.ran) continue;
        if (r.p50 < 5.0)
            return "pure compute: K5 p50 " + num(r.p50, 2) +
                   " ms is inside the 5.0 ms budget -- pure compute is the default at 90 Hz";
        if (r.p50 <= 8.0)
            return "hybrid default: K5 p50 " + num(r.p50, 2) +
                   " ms is between 5 and 8 ms -- pure compute at 72 Hz or with 1.5x foveated "
                   "tile reduction, hybrid is the default";
        return "hybrid only: K5 p50 " + num(r.p50, 2) +
               " ms is over 8 ms -- this device is hybrid-only; the pure-compute path "
               "continues on PC and next-generation Adreno";
    }
    return "no verdict: K5 did not run";
}

std::string buildTable(const RunInfo& info, const std::vector<KernelResult>& results)
{
    std::ostringstream o;
    o << "\n";
    o << "NX Warp Phase 0 gate -- PAPER section 3.4\n";
    o << "device      : " << info.device.name;
    if (!info.device.driver.empty()) o << " (" << info.device.driver << ")";
    o << "\n";
    o << "platform    : " << info.platform << "   mode: " << info.mode << "\n";
    o << "subgroup    : size " << info.device.subgroupSize
      << " (min " << info.device.subgroupMin << ", max " << info.device.subgroupMax << ")"
      << ", ballot " << (info.device.subgroupBallot ? "yes" : "NO")
      << ", size control " << (info.device.subgroupSizeControl ? "yes" : "no") << "\n";
    o << "shared mem  : " << info.device.maxSharedMemory << " B"
      << "   timestampPeriod " << num(double(info.device.timestampPeriod), 2) << " ns"
      << "   validBits " << info.device.timestampValidBits << "\n";
    o << "frame       : " << info.cfg.width << "x" << info.cfg.height
      << "  co-tenant reprojection " << (info.cfg.cotenant ? "ON" : "off")
      << " at " << info.cfg.reproW << "x" << info.cfg.reproH << "\n";
    o << "frames      : " << info.cfg.warmup << " warm-up + " << info.cfg.frames
      << " measured, per kernel\n";
    if (!info.cfg.label.empty()) o << "label       : " << info.cfg.label << "\n";
    o << "\n";

    o << "  kernel        p50      p95      p99      min      max   threshold        result\n";
    o << "  ------------------------------------------------------------------------------\n";
    for (const auto& r : results)
    {
        char line[256];
        std::string thr;
        if (r.thresholdGbps > 0)       thr = ">= " + num(r.thresholdGbps, 0) + " GB/s";
        else if (r.thresholdP50 > 0 && r.thresholdP99 > 0)
            thr = "< " + num(r.thresholdP50, 1) + " / " + num(r.thresholdP99, 1);
        else if (r.thresholdP50 > 0)   thr = "< " + num(r.thresholdP50, 1) + " ms";
        else                           thr = "informational";

        if (!r.ran)
        {
            snprintf(line, sizeof line, "  %-12s %48s %-13s %s\n",
                     r.name.c_str(), "", thr.c_str(), ("skip: " + r.skipReason).c_str());
            o << line;
            continue;
        }
        snprintf(line, sizeof line, "  %-12s %7s  %7s  %7s  %7s  %7s   %-13s %s\n",
                 r.name.c_str(), num(r.p50, 3).c_str(), num(r.p95, 3).c_str(),
                 num(r.p99, 3).c_str(), num(r.minMs, 3).c_str(), num(r.maxMs, 3).c_str(),
                 thr.c_str(), statusOf(r).c_str());
        o << line;
        if (r.gbPerSec > 0)
        {
            snprintf(line, sizeof line, "  %-12s %s GB/s achievable\n", "",
                     num(r.gbPerSec, 1).c_str());
            o << line;
        }
    }
    o << "  (all times in milliseconds, VK_QUERY_TYPE_TIMESTAMP pairs, "
         "timestampPeriod applied)\n\n";

    if (info.hybridDecodeLatencyP50 >= 0.0)
        o << "hybrid base decode latency p50: " << num(info.hybridDecodeLatencyP50, 2)
          << " ms (threshold < 15 ms)\n\n";

    // Thermal report
    bool anyThermal = false;
    for (const auto& r : results) if (!r.minuteP50.empty()) anyThermal = true;
    if (anyThermal)
    {
        o << "thermal (per-minute p50, ms)\n";
        for (const auto& r : results)
        {
            if (r.minuteP50.empty()) continue;
            o << "  " << r.name << ":";
            for (double v : r.minuteP50) o << " " << num(v, 2);
            double drift = (r.firstMinuteP50 > 0)
                ? (r.lastMinuteP50 - r.firstMinuteP50) / r.firstMinuteP50 * 100.0 : 0.0;
            o << "   first " << num(r.firstMinuteP50, 2)
              << " -> last " << num(r.lastMinuteP50, 2)
              << "  (" << (drift >= 0 ? "+" : "") << num(drift, 1) << "%)\n";
        }
        o << "\n";
    }

    o << "verdict: " << info.verdict << "\n";
    return o.str();
}

std::string buildJson(const RunInfo& info, const std::vector<KernelResult>& results)
{
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"nxwarp-phase0/1\",\n";
    o << "  \"paper_section\": \"3.4\",\n";
    o << "  \"platform\": \"" << esc(info.platform) << "\",\n";
    o << "  \"mode\": \"" << esc(info.mode) << "\",\n";
    o << "  \"label\": \"" << esc(info.cfg.label) << "\",\n";
    o << "  \"device\": {\n";
    o << "    \"name\": \"" << esc(info.device.name) << "\",\n";
    o << "    \"driver\": \"" << esc(info.device.driver) << "\",\n";
    o << "    \"vendor_id\": " << info.device.vendorID << ",\n";
    o << "    \"device_id\": " << info.device.deviceID << ",\n";
    o << "    \"api_version\": " << info.device.apiVersion << ",\n";
    o << "    \"subgroup_size\": " << info.device.subgroupSize << ",\n";
    o << "    \"subgroup_min\": " << info.device.subgroupMin << ",\n";
    o << "    \"subgroup_max\": " << info.device.subgroupMax << ",\n";
    o << "    \"subgroup_ballot\": " << (info.device.subgroupBallot ? "true" : "false") << ",\n";
    o << "    \"subgroup_size_control\": "
      << (info.device.subgroupSizeControl ? "true" : "false") << ",\n";
    o << "    \"max_shared_memory\": " << info.device.maxSharedMemory << ",\n";
    o << "    \"timestamp_period_ns\": " << num(double(info.device.timestampPeriod), 4) << ",\n";
    o << "    \"timestamp_valid_bits\": " << info.device.timestampValidBits << "\n";
    o << "  },\n";
    o << "  \"config\": {\n";
    o << "    \"width\": " << info.cfg.width << ",\n";
    o << "    \"height\": " << info.cfg.height << ",\n";
    o << "    \"repro_width\": " << info.cfg.reproW << ",\n";
    o << "    \"repro_height\": " << info.cfg.reproH << ",\n";
    o << "    \"warmup\": " << info.cfg.warmup << ",\n";
    o << "    \"frames\": " << info.cfg.frames << ",\n";
    o << "    \"cotenant\": " << (info.cfg.cotenant ? "true" : "false") << ",\n";
    o << "    \"qp\": " << info.cfg.qp << ",\n";
    o << "    \"symbols_per_pixel\": " << num(info.cfg.symbolsPerPixel, 3) << ",\n";
    o << "    \"thermal_seconds\": " << num(info.cfg.thermalSeconds, 1) << "\n";
    o << "  },\n";
    if (info.hybridDecodeLatencyP50 >= 0.0)
        o << "  \"hybrid_decode_latency_p50_ms\": "
          << num(info.hybridDecodeLatencyP50, 3) << ",\n";
    o << "  \"kernels\": [\n";
    for (size_t i = 0; i < results.size(); ++i)
    {
        const KernelResult& r = results[i];
        o << "    {\n";
        o << "      \"name\": \"" << esc(r.name) << "\",\n";
        o << "      \"ran\": " << (r.ran ? "true" : "false") << ",\n";
        if (!r.ran) o << "      \"skip_reason\": \"" << esc(r.skipReason) << "\",\n";
        o << "      \"frames\": " << r.frames << ",\n";
        o << "      \"p50_ms\": " << num(r.p50) << ",\n";
        o << "      \"p95_ms\": " << num(r.p95) << ",\n";
        o << "      \"p99_ms\": " << num(r.p99) << ",\n";
        o << "      \"min_ms\": " << num(r.minMs) << ",\n";
        o << "      \"max_ms\": " << num(r.maxMs) << ",\n";
        o << "      \"mean_ms\": " << num(r.mean) << ",\n";
        o << "      \"gb_per_sec\": " << num(r.gbPerSec, 2) << ",\n";
        o << "      \"threshold_p50_ms\": " << num(r.thresholdP50, 2) << ",\n";
        o << "      \"threshold_p99_ms\": " << num(r.thresholdP99, 2) << ",\n";
        o << "      \"threshold_gbps\": " << num(r.thresholdGbps, 2) << ",\n";
        o << "      \"status\": \"" << statusOf(r) << "\",\n";
        o << "      \"first_minute_p50_ms\": " << num(r.firstMinuteP50) << ",\n";
        o << "      \"last_minute_p50_ms\": " << num(r.lastMinuteP50) << ",\n";
        o << "      \"minute_p50_ms\": [";
        for (size_t j = 0; j < r.minuteP50.size(); ++j)
            o << (j ? ", " : "") << num(r.minuteP50[j]);
        o << "]\n";
        o << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    o << "  ],\n";
    o << "  \"verdict\": \"" << esc(info.verdict) << "\"\n";
    o << "}\n";
    return o.str();
}

} // namespace nxb
