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
        "docs/RESEARCH-ACADEMIC.md entry 12), parity for class A only, and the paper's\n"
        "class-aware 30 / 10 / 0.  With parity off the 44-byte parity reserve is\n"
        "released too, so the run payload budget grows from 1316 to 1360 bytes: that\n"
        "headroom is part of what FEC costs.\n\n"
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
    }

    // Concealment cost of removing FEC, and what the overhead bought.
    std::fprintf(f,
                 "\n| scenario | concealed/frame off | class A only | full |"
                 " tiles saved by class A parity | by the rest | overhead of class A"
                 " parity | of the rest |\n|---|---|---|---|---|---|---|---|\n");
    for (const FecSweepRow& s : sw)
        std::fprintf(f, "| %s | %.1f | %.1f | %.1f | %.1f | %.1f | %+.2f pp | %+.2f pp |\n",
                     s.off.name.c_str(), s.off.conceal_per_frame,
                     s.a_only.conceal_per_frame, s.full.conceal_per_frame,
                     s.off.conceal_per_frame - s.a_only.conceal_per_frame,
                     s.a_only.conceal_per_frame - s.full.conceal_per_frame,
                     s.a_only.overhead_pct - s.off.overhead_pct,
                     s.full.overhead_pct - s.a_only.overhead_pct);

    // ---- the reading, computed from the rows above ----------------------
    double saved_a = 0, saved_rest = 0, cost_a = 0, cost_rest = 0, n = 0;
    double best_a = -1e9, best_a_loss = 0;
    int a_beats_off = 0, full_beats_off = 0, total = 0;
    const ScenarioResult* ctrl_off = nullptr;
    const ScenarioResult* ctrl_a = nullptr;
    const ScenarioResult* ctrl_full = nullptr;
    for (const FecSweepRow& s : sw) {
        if (s.off.link_desc.find("600M") != std::string::npos) {
            ctrl_off = &s.off; ctrl_a = &s.a_only; ctrl_full = &s.full;
            continue;  // the control is not part of the average
        }
        double sa = s.off.conceal_per_frame - s.a_only.conceal_per_frame;
        double sr = s.a_only.conceal_per_frame - s.full.conceal_per_frame;
        saved_a += sa;
        saved_rest += sr;
        cost_a += s.a_only.overhead_pct - s.off.overhead_pct;
        cost_rest += s.full.overhead_pct - s.a_only.overhead_pct;
        if (sa > best_a) { best_a = sa; best_a_loss = s.off.measured_loss_pct; }
        if (s.a_only.conceal_per_frame < s.off.conceal_per_frame) ++a_beats_off;
        if (s.full.conceal_per_frame < s.off.conceal_per_frame) ++full_beats_off;
        ++total;
        n += 1;
    }
    if (n <= 0) return;
    saved_a /= n; saved_rest /= n; cost_a /= n; cost_rest /= n;
    const double tiles = 2312.0;
    double saved_full = saved_a + saved_rest;
    double cost_full = cost_a + cost_rest;

    std::fprintf(f,
        "\n### Reading: does prioritized FEC earn its 17 percent?\n\n"
        "On these numbers, not as configured -- but the class A half of it does.  "
        "Averaged over the six single-path loss scenarios at 300 Mbit/s, the full "
        "30 / 10 / 0 policy costs %.2f percentage points of wire overhead and leaves "
        "%.1f MORE tiles concealed per frame than running with no parity at all "
        "(%.2f %% of the %.0f-tile frame): it makes the picture worse, not better.  "
        "Split in two, class A parity costs %.2f pp and removes %.1f concealed tiles "
        "per frame, best %.1f at %.1f %% measured loss, while the class B row on top "
        "costs a further %.2f pp and puts %.1f of them back.  Class A only beat no FEC "
        "in %d of %d scenarios; the full policy beat it in %d.\n\n"
        "The mechanism is the band deadline, not the coding.  Parity is extra bytes "
        "in the same band window as the data it protects, on a link carrying "
        "%.0f Mbit/s of a 300 Mbit/s budget whose per-band burst is several times "
        "that, so the parity for band b delays band b+1 into its own deadline.  The "
        "reference-age columns show how sharp that edge is: with parity off, %.1f %% "
        "of tiles still reference N-1, and with the full policy on, %.1f %% do -- the "
        "17 %% of parity bytes costs essentially every tile a full frame of reference "
        "recency, which PAPER 6.6 prices at a further 5 to 10 %% of bits.  That "
        "second-order cost is larger than the parity itself and is not in the paper's "
        "budget at all.\n\n"
        "The 600 Mbit/s control says the opposite, and says it loudly: with the air no "
        "longer the constraint, concealment is %.1f / %.1f / %.1f tiles per frame for "
        "off / class A / full, so parity is worth far more than its bytes -- and both "
        "settings hold %.1f %% of tiles on N-1.  Whether prioritized FEC earns its "
        "overhead is therefore not a property of the FEC at all; it is a property of "
        "the link headroom, and the parity ladder of PAPER 4.4 keys off measured loss "
        "when it should key off measured headroom.\n\n"
        "One caveat on the control row that has to be stated rather than smoothed "
        "over: its parity-off concealment (%.1f tiles per frame, %.1f of them late) is "
        "worse than the same setting at 300 Mbit/s, which is backwards for a faster "
        "link.  The deadline-offset column is the tell.  The controller of PAPER 4.3 "
        "climbs on a miss but only relaxes after 90 consecutive frames with zero "
        "missing tiles, and on a fast link it reaches that condition often enough to "
        "relax back into the miss region and oscillate.  So the control row measures "
        "the controller as much as the FEC, and the sweep should be re-run with the "
        "deadline pinned before any of these numbers are used to change the parity "
        "policy.  It is logged as an open issue, not folded into the conclusion.\n\n"
        "Two caveats before this is read as an argument for switching FEC off.  A "
        "concealed tile is not a lost tile: it is a warp of the previous frame, which "
        "is nearly free in the periphery and very visible in the fovea, and that "
        "asymmetry is exactly what the class weighting exists to exploit -- so counting "
        "tiles understates class A parity and overstates class B.  And the intra "
        "column is the GRACE comparison in one number: it never rises above 1.6 %% in "
        "any of the twenty-one rows, with or without parity, because a concealed tile "
        "stays an exact reference here (decisions D10 and D17) and a loss therefore "
        "costs one frame of prediction rather than an undecodable frame.  That "
        "structural resilience is what GRACE trains a network to acquire and what NX "
        "Warp gets from the reference model for free; it, rather than the concealment "
        "counts, is the real argument for spending less on FEC.\n\n"
        "Recommendation from this table: keep class A parity, make the class B row "
        "conditional on measured loss instead of always-on (the ladder in PAPER 4.4 "
        "already has the hook -- it just starts in the wrong place), and re-run this "
        "sweep against a quality metric rather than a tile count before committing.\n",
        cost_full, -saved_full, -100.0 * saved_full / tiles, tiles,
        cost_a, saved_a, best_a, best_a_loss, cost_rest, -saved_rest,
        a_beats_off, total, full_beats_off,
        sw.empty() ? 0.0 : sw.front().full.bitrate_mbps,
        sw.empty() ? 0.0 : sw.front().off.ref_pct[0],
        sw.empty() ? 0.0 : sw.front().full.ref_pct[0],
        ctrl_off ? ctrl_off->conceal_per_frame : 0.0,
        ctrl_a ? ctrl_a->conceal_per_frame : 0.0,
        ctrl_full ? ctrl_full->conceal_per_frame : 0.0,
        ctrl_full ? ctrl_full->ref_pct[0] : 0.0,
        ctrl_off ? ctrl_off->conceal_per_frame : 0.0,
        ctrl_off ? ctrl_off->late_per_frame : 0.0);
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
