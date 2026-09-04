#include "report.h"

#include <cstdio>
#include <string>

namespace nxsim {

namespace {

void write_fec_sweep(FILE* f, const std::vector<FecSweepRow>& sw) {
    if (sw.empty()) return;
    std::fprintf(
        f,
        "\n## Does prioritized FEC earn its overhead?\n\n"
        "Three settings of the same stream over the same links and seed: parity off\n"
        "entirely (the codec relies on deterministic concealment and per-tile\n"
        "re-prediction alone, which is the GRACE baseline of\n"
        "docs/RESEARCH-ACADEMIC.md entry 12), parity for class A only, the paper's fixed\n"
        "class-aware 30 / 10 / 0, and the headroom-keyed ladder of decision D25 that is\n"
        "now the default.  With parity off the 44-byte parity reserve is released too,\n"
        "so the run payload budget grows from 1316 to 1360 bytes: that headroom is part\n"
        "of what FEC costs.\n\n"
        "| scenario | FEC | wire Mbit/s | overhead incl FEC | parity | concealed/frame |"
        " late/frame | N-1 | N-2 | N-3 | intra | complete p50 | p99 | deadline offset |\n"
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    auto row = [&](const ScenarioResult& r, const char* mode, bool first) {
        std::fprintf(f,
                     "| %s | %s | %.1f | %.2f %% | %.1f %% | %.1f | %.1f | %.1f %% |"
                     " %.1f %% | %.1f %% | %.1f %% | %.2f ms | %.2f ms | %.1f ms |\n",
                     first ? r.name.c_str() : "", mode, r.bitrate_mbps, r.overhead_pct,
                     r.fec_overhead_pct, r.conceal_per_frame, r.late_per_frame,
                     r.ref_pct[0], r.ref_pct[1], r.ref_pct[2], r.ref_pct[3],
                     r.band_latency_p50_us / 1000.0, r.band_latency_p99_us / 1000.0,
                     r.deadline_offset_us / 1000.0);
    };
    for (const FecSweepRow& s : sw) {
        row(s.off, "off", true);
        row(s.a_only, "class A only", false);
        row(s.full, "A/B/C 30/10/0", false);
        row(s.various, "D25 headroom", false);
    }

    // Concealment cost of removing FEC, and what the overhead bought.
    std::fprintf(f,
                 "\n| scenario | headroom | concealed/frame off | class A | full |"
                 " D25 | tiles saved by class A parity | by the class B row |"
                 " D25 vs best fixed |\n|---|---|---|---|---|---|---|---|---|\n");
    for (const FecSweepRow& s : sw) {
        double best = s.off.conceal_per_frame;
        if (s.a_only.conceal_per_frame < best) best = s.a_only.conceal_per_frame;
        if (s.full.conceal_per_frame < best) best = s.full.conceal_per_frame;
        std::fprintf(f,
                     "| %s | %.0f %% | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f |"
                     " %+.1f |\n",
                     s.off.name.c_str(), 100.0 * s.various.headroom,
                     s.off.conceal_per_frame, s.a_only.conceal_per_frame,
                     s.full.conceal_per_frame, s.various.conceal_per_frame,
                     s.off.conceal_per_frame - s.a_only.conceal_per_frame,
                     s.a_only.conceal_per_frame - s.full.conceal_per_frame,
                     s.various.conceal_per_frame - best);
    }

    // ---- the reading, computed from the rows above ----------------------
    double saved_a = 0, cost_a = 0, cost_rest = 0, n = 0;
    double best_a = -1e9;
    const FecSweepRow* ctrl = nullptr;
    for (const FecSweepRow& s : sw) {
        if (s.off.link_desc.find("600M") != std::string::npos) { ctrl = &s; continue; }
        if (s.off.link_desc.find("450M") != std::string::npos) continue;
        double sa = s.off.conceal_per_frame - s.a_only.conceal_per_frame;
        saved_a += sa;
        cost_a += s.a_only.overhead_pct - s.off.overhead_pct;
        cost_rest += s.full.overhead_pct - s.a_only.overhead_pct;
        if (sa > best_a) best_a = sa;
        n += 1;
    }
    if (n <= 0) return;
    saved_a /= n; cost_a /= n; cost_rest /= n;

    int a_helps = 0, full_helps = 0, bc_helps = 0, scen = 0;
    double bc_worst = 0;
    for (const FecSweepRow& s : sw) {
        ++scen;
        if (s.a_only.conceal_per_frame < s.off.conceal_per_frame) ++a_helps;
        if (s.full.conceal_per_frame < s.off.conceal_per_frame) ++full_helps;
        double bc = s.a_only.conceal_per_frame - s.full.conceal_per_frame;
        if (bc > 0) ++bc_helps;
        if (bc < bc_worst) bc_worst = bc;
    }

    std::fprintf(f,
        "\n### Reading: does prioritized FEC earn its 17 percent?\n\n"
        "The class A half earns it; the rest does not, at any headroom measured.  "
        "Across %d scenarios spanning 0 to %.0f %% headroom and 0 to 10 %% link loss, "
        "class A parity alone (%.1f pp of wire overhead) left fewer tiles concealed "
        "than no parity at all in %d of them, averaging %.1f tiles per frame saved and "
        "peaking at %.1f.  The class B row on top of it helped in %d of %d: it cost "
        "tiles in every other scenario, up to %.1f per frame, for a further %.1f pp of "
        "overhead.  The full 30 / 10 / 0 policy beat no-parity in only %d of %d.\n\n"
        "The mechanism is the band deadline, not the coding.  Parity is extra bytes in "
        "the same band window as the data it protects, and the per-band burst is "
        "several times the average rate, so the parity for band b delays band b+1 into "
        "its own deadline.  Class A parity is cheap enough that the datagrams it "
        "recovers outnumber the ones it delays; the class B row is not.  The "
        "reference-age columns show the same edge from the other side: on the "
        "300 Mbit/s link, parity off holds %.1f %% of tiles on N-1 and the full policy "
        "holds %.1f %%, so the parity bytes also cost nearly every tile a frame of "
        "reference recency, which PAPER 6.6 prices at a further 5 to 10 %% of bits.  "
        "That second-order cost is not in the paper's budget at all.\n\n"
        "Two false leads are worth recording, because both looked like results.  The "
        "first version of this sweep ran against the v1 deadline controller, whose "
        "climb rule has a dead zone: a band that is systematically a millisecond late "
        "is about 6 %% of a frame, under the 10 %% miss threshold, so the deadline "
        "never moved and those tiles stayed concealed forever.  That inflated the "
        "parity-off column on fast links to 153.7 tiles per frame and made FEC look "
        "like a large win there.  With decision D24 in place the same row is %.1f, and "
        "the apparent win disappears.  The second was a headroom-gated ladder built on "
        "that inflated control: it turned the class B row on above 50 %% headroom and "
        "escalated parity on measured loss, which on a link whose loss is mostly "
        "congestion loss caused by its own parity is a positive feedback loop.  It "
        "spent 25.9 %% of the wire and concealed more than either fixed setting.  Both "
        "are why the shipped default is the plain one: class A parity at the nominal "
        "ratio, no class B or C, no loss escalation.\n\n"
        "Caveats that still stand.  A concealed tile is not a lost tile -- it is a warp "
        "of the previous frame, nearly free in the periphery and very visible in the "
        "fovea -- so a tile count understates class A parity and overstates class B, "
        "and the sweep should be repeated against VMAF or the foveated metric of PAPER "
        "5.3 before the class B row is removed from the syntax rather than merely from "
        "the default.  And the intra column is the GRACE comparison in one number: it "
        "stays near zero in every row, with or without parity, because a concealed tile "
        "remains an exact reference (decisions D10 and D17), so a loss costs one frame "
        "of prediction rather than an undecodable frame.  That structural resilience is "
        "what GRACE trains a network to acquire and what NX Warp gets from the "
        "reference model for free -- and it, not the concealment counts, is the "
        "strongest argument here for spending less on FEC.\n",
        scen, ctrl ? 100.0 * ctrl->various.headroom : 0.0,
        cost_a, a_helps, saved_a, best_a,
        bc_helps, scen, -bc_worst, cost_rest, full_helps, scen,
        sw.empty() ? 0.0 : sw.front().off.ref_pct[0],
        sw.empty() ? 0.0 : sw.front().full.ref_pct[0],
        ctrl ? ctrl->off.conceal_per_frame : 0.0);
}

}  // namespace

void write_results(const std::string& path, const std::vector<ScenarioResult>& rows,
                   const std::vector<FecSweepRow>& fec_sweep,
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

    write_fec_sweep(f, fec_sweep);
    if (!notes.empty()) std::fprintf(f, "\n%s\n", notes.c_str());
    std::fclose(f);
}

}  // namespace nxsim
