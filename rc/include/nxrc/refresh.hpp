// nxrc::RefreshScheduler - the temporal axis of the degradation ladder.
//
// The spatial ladder (RATECONTROL.md 4.4) decides how much detail a tile
// keeps.  This decides how *often* a tile's residual is sent at all.  A tile
// the scheduler skips is emitted as `WARP_SKIP`: the decoder's pose warp
// keeps it moving with the head, and only the residual correction is
// withheld.  It is not a frozen tile.
//
// Specification: docs/RATECONTROL.md section 8.
// Sources: Floeter, Geringer, Reina, Weiskopf, Ropinski, ETRA 2025
// (arXiv 2505.03682) for the operating points; Tursun and Didyk, ACM TOG
// 2022 (arXiv 2205.00108, see nxrc/tvm.hpp) for the cost function.
//
// Data layout discipline, as everywhere in this library: struct of arrays
// indexed by tile id, one pass per tile plus sum reductions.  The gate
// search is a fixed 16-step loop of (one thread per tile + one sum
// reduction), the same shape as the spatial pressure search.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef NXRC_REFRESH_HPP
#define NXRC_REFRESH_HPP

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

#include "nxfov/foveation.hpp"
#include "nxrc/nxrc.hpp"
#include "nxrc/tvm.hpp"

namespace nxrc {

// The temporal ladder.  A tile's temporal step is a refresh divisor k: its
// residual is coded on one frame in k, and it is WARP_SKIP on the other
// k - 1.  These are Floeter et al.'s per-region update rates 1/1 .. 1/5,
// with 5 replaced by 6 so the ladder divides the rolling-refresh period
// evenly and every k divides k_max.
static constexpr int kTemporalSteps = 5;

struct RefreshConfig {
    uint8_t ladder[kTemporalSteps] = { 1, 2, 3, 4, 6 };
    int     ladder_len = kTemporalSteps;

    float fps = 72.0f;

    // ---- hard invariants -------------------------------------------------
    // Every tile is coded at least this often, whatever the budget and
    // whatever the model says.  This is the bound the reference chain and
    // the rolling intra refresh are argued against (RATECONTROL.md 8.5); it
    // must divide evenly by every ladder entry, and it caps the ladder.
    uint8_t k_max_frames = 6;

    // Foveal floor: a tile at or inside this eccentricity is coded every
    // frame at every budget.  Matches RateConfig::mid_ring_deg so the
    // temporal and spatial fovea are the same region.
    float fovea_full_deg = 8.0f;

    // Per-class caps on k.  Text is never skipped unless it is static;
    // edges reach 1/2 and stop, because a stale edge is a double image and
    // not a blur.  Flat and texture use the whole ladder.
    uint8_t max_k[4] = { 6, 6, 2, 1 };   // Flat, Texture, Edge, Text

    // A tile whose warped residual is below this is already WARP_SKIP in the
    // allocator (RateConfig::skip_sad) and costs nothing.  The scheduler
    // does not count it against the budget and lets even text sit there,
    // but its age still ticks and the k_max_frames bound still applies.
    float static_mad = 1.0f;

    // ---- budget knob -----------------------------------------------------
    // Exactly one mode is active.  If target_coded_fraction is in (0, 1] the
    // gate is searched to hit that steady-state duty cycle.  Otherwise if
    // target_bits > 0 the target fraction is derived from it and the
    // measured mean cost of a coded tile.  Otherwise (both <= 0) the gate
    // follows the spatial ladder pressure, which is the default coupling
    // described in RATECONTROL.md 8.4.
    float target_coded_fraction = -1.0f;
    float target_bits           = -1.0f;

    // ---- the gate --------------------------------------------------------
    // The gate is a visibility threshold in the model's JND units (C_M).  A
    // tile takes the largest admissible divisor whose predicted visibility
    // is at or below the gate.
    int   gate_steps = 16;
    float gate_lo    = 0.02f;
    float gate_hi    = 4.0f;
    // Pressure-coupled mode: the temporal ladder does not engage at all
    // until the spatial ladder has run out of free steps.  Below this
    // pressure the gate is pinned at gate_lo.
    float engage_pressure = 2.0f;
    float max_pressure    = 4.0f;
    // The gate may tighten instantly (a budget cut is a hard constraint) but
    // relaxes at most this many multiplicative steps per frame, so the
    // periphery does not flap between two refresh rates.  Same asymmetry,
    // and the same reason, as the spatial pressure and the decode governor.
    float gate_slew_up = 1.35f;

    // EMA constant for the bits-per-coded-tile estimate used by target_bits.
    float cost_ema = 0.15f;

    tvm::ModelParams model;
};

// Per-frame inputs.  Spans, not owned; all optional spans may be empty.
struct RefreshInputs {
    const nxfov::FoveationMap* fov = nullptr;
    std::span<const uint8_t> cls;          // from classify_tiles
    std::span<const float>   complexity;   // warped residual, MAD per pixel
    const TileStats*         stats = nullptr;

    // Tiles the encoder must code this frame whatever the scheduler thinks.
    // The scheduler never overrides these; it only ever adds skips.
    //  intra_due      : this frame's slice of the rolling intra refresh
    //                   (1/180 of tiles, PAPER.md 6.6 / ADR-0006).
    //  ref_ineligible : the 3x3 acknowledged-neighbourhood rule failed, so
    //                   the tile is going intra anyway (TRANSPORT.md 9).
    std::span<const uint8_t> intra_due;
    std::span<const uint8_t> ref_ineligible;

    bool  scene_cut       = false;   // every tile is coded on a cut
    int   frame_index     = 0;       // drives the refresh phase
    float spatial_pressure = 0.0f;   // AllocResult::pressure of this frame
    float ppd_center      = 22.0f;   // nxfov::ppd_center(lens)
};

struct RefreshResult {
    std::vector<uint8_t> divisor;      // chosen k, 1 = every frame
    std::vector<uint8_t> force_skip;   // 1 = WARP_SKIP this frame
    std::vector<uint8_t> age;          // frames since last coded, after this one
    std::vector<uint8_t> mandatory;    // coded regardless of the model
    std::vector<float>   visibility;   // C_M at the chosen k, JND units
    std::vector<float>   p_detect;     // Weibull probability of detection

    float gate            = 0.0f;
    float duty_cycle      = 1.0f;   // steady-state coded fraction of the map
    float coded_fraction  = 1.0f;   // this frame's actual coded fraction
    float mean_visibility = 0.0f;   // over tiles with k > 1
    float max_visibility  = 0.0f;
    int   coded    = 0;
    int   skipped  = 0;             // skipped by the scheduler, not by SAD
    int   forced   = 0;             // mandatory tiles
    int   age_forced = 0;           // skips overridden by the k_max bound

    size_t size() const { return divisor.size(); }
    void   resize(size_t n);
};

// The refresh phase of a tile: which frame of its k-cycle it refreshes on.
// A fixed bit-reversal (van der Corput) permutation of the tile id, the same
// device the rolling intra refresh uses and the same one qp_dither uses, so
// neighbouring tiles land on different frames and there is no refresh wave.
// Deterministic and stateless.
uint32_t refresh_phase(size_t tile_id, uint8_t k);

class RefreshScheduler {
public:
    explicit RefreshScheduler(RefreshConfig cfg = {});

    void reset(size_t tile_count);

    // One frame.  Pure function of (internal age state, inputs, config).
    const RefreshResult& schedule(const RefreshInputs& in);

    // Feed back the frame's real total bits and coded-tile count so
    // target_bits mode can convert bits to a duty cycle.
    void update_cost(float total_bits, int coded_tiles);

    const RefreshResult& last() const { return out_; }
    const RefreshConfig& config() const { return cfg_; }
    RefreshConfig&       config() { return cfg_; }

    float gate() const { return gate_; }
    float bits_per_coded_tile() const { return cost_; }

    // Largest divisor from the ladder that this tile may take at `gate`,
    // after the class, foveal and k_max caps.  Exposed for the tests and
    // the simulator; monotone non-decreasing in `gate`.
    uint8_t admissible_divisor(const RefreshInputs& in, size_t i,
                               float gate) const;

    // The model input assembled for one tile.  Exposed so the simulator can
    // report per-ring visibility without duplicating the reduction.
    tvm::TileTemporal tile_model(const RefreshInputs& in, size_t i) const;

private:
    float duty_cycle_at(const RefreshInputs& in, float gate) const;
    float search_gate(const RefreshInputs& in, float target);

    RefreshConfig        cfg_;
    RefreshResult        out_;
    std::vector<uint8_t> age_;
    float                gate_ = 0.0f;
    float                cost_ = 0.0f;
    bool                 have_cost_ = false;
};

} // namespace nxrc

#endif // NXRC_REFRESH_HPP
