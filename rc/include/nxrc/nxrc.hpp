// nxvc_rc - NX Warp rate control, tile classification, the degradation
// ladder and the decode-time governor.
//
// Specification: docs/PAPER.md sections 1.5, 4.6, 4.6.1, 4.7, 5.1, 5.2.
// Implementation write-up: docs/RATECONTROL.md.
//
// Data layout discipline (this library is a CPU model of a future set of
// Vulkan compute dispatches):
//
//   * Everything is per tile.  All inputs and outputs are struct-of-arrays
//     indexed by tile id = ty * tiles_x + tx.
//   * No per-pixel work outside compute_tile_stats(), which is the CPU model
//     of the encoder's existing analysis dispatch (one workgroup per tile,
//     tile-summed statistics only).
//   * Every reduction in allocate() is a sum or a max over the tile array,
//     i.e. a subgroup reduction plus a workgroup reduction; the one prefix
//     sum is over the tile weights.  The pressure search is a fixed 17-step
//     loop of such reductions, not a data-dependent one.
//   * No allocation happens inside allocate() after the first frame; all
//     buffers live in RateController and are resized once.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef NXRC_NXRC_HPP
#define NXRC_NXRC_HPP

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

#include "nxfov/foveation.hpp"

namespace nxrc {

// ============================================================ classes =====

// Ordered by how strongly the ladder protects them (Flat is given up first,
// Text last).  Stored in the per-tile arrays as uint8_t.
enum class TileClass : uint8_t {
    Flat    = 0,
    Texture = 1,
    Edge    = 2,
    Text    = 3,
    Count   = 4
};

const char* class_name(uint8_t c);

// Per-tile statistics produced by the encoder's analysis dispatch.
// Struct of arrays; every vector has the same length.
//
//   mean_luma : mean of the luma plane over the tile, 0..255
//   log_var   : log2(variance + 1) of the luma plane over the tile
//   jxx/jxy/jyy: structure tensor sums over the tile,
//                Jxx = sum gx*gx, Jxy = sum gx*gy, Jyy = sum gy*gy,
//                with gx, gy the central differences (I[x+1]-I[x-1])/2.
//   ui_stencil: optional, 1 = the compositor says this tile is UI / quad
//               layer text.  May be empty.
struct TileStats {
    std::vector<float>   mean_luma;
    std::vector<float>   log_var;
    std::vector<float>   jxx, jxy, jyy;
    std::vector<uint8_t> ui_stencil;

    size_t size() const { return mean_luma.size(); }
    void   resize(size_t n);
};

// CPU model of the analysis dispatch: tile-summed statistics from one luma
// plane.  Tiles that hang off the right/bottom edge use the pixels present.
void compute_tile_stats(const uint8_t* luma, int stride,
                        int width, int height, int tile_size,
                        TileStats& out);

// Statistics of a single tile from a tile_size x tile_size luma block.
void compute_one_tile_stats(const uint8_t* luma, int stride, int tile_size,
                            float& mean, float& log_var,
                            float& jxx, float& jxy, float& jyy);

// Thresholds are calibrated against the synthetic material in nxrc::synth;
// the measured statistics and the reasoning are in RATECONTROL.md 3.2.
struct ClassifyConfig {
    // Flat: no gradient energy, or no variance at all.  The gradient test is
    // the important one - a smooth ramp has a large variance and no
    // structure, and must not be called Texture.
    float flat_gradient     = 12.0f;  // mean squared central difference
    float flat_activity     = 3.0f;   // log2(var+1); var < 7, sigma < 2.7

    // Text: hard, full-contrast micro-structure.  `text_r_max` is the guard
    // against white noise, which sits at R = grad/var = 1.0 by construction
    // while glyph fields measure 0.50 to 0.89.
    float text_gradient     = 2500.0f;
    float text_activity     = 11.0f;  // var >= 2047
    float text_r_max        = 0.93f;

    // Edge: one dominant gradient orientation over the tile.
    float edge_coherence    = 0.45f;

    // Hysteresis band applied when a previous class is supplied: a tile has
    // to cross a threshold by this much to leave its class.  Stops the
    // periphery from shimmering between texture and edge frame to frame.
    bool  hysteresis        = true;
    float coherence_margin  = 0.06f;   // absolute, on coherence
    float gradient_margin   = 0.15f;   // relative, on the gradient thresholds
    float activity_margin   = 0.5f;    // absolute, on log-variance
};

// classify_tiles(stats) -> class[].  `prev` may be empty; when it is not, it
// must have the same length and the hysteresis band applies.
void classify_tiles(const TileStats& stats, const ClassifyConfig& cfg,
                    std::span<const uint8_t> prev,
                    std::vector<uint8_t>& out_class);

// Derived quantities, exposed because the tests and the simulator want them.
// coherence = (l1 - l2) / (l1 + l2) of the structure tensor, in [0, 1].
float tile_coherence(float jxx, float jxy, float jyy);
// mean squared gradient magnitude over the tile.
float tile_gradient_energy(float jxx, float jyy, int tile_pixels);
// R = gradient energy / variance, a normalised mean squared spatial
// frequency.  Exactly 1.0 for white noise of any amplitude, 0.25 to 0.67 for
// band-limited texture, 0.003 to 0.06 for edges and ramps.
float tile_frequency_ratio(float grad_energy, float log_var);

// ============================================================ ladder ======

// One row of a class ladder.  `res_level_abs` is an absolute floor on the
// coded resolution level (0 = 64x64, 1 = 32x32, 2 = 16x16); the tile's final
// res_level is max(foveation level, res_level_abs).
struct LadderStep {
    uint8_t res_level_abs = 0;
    uint8_t wm_id         = 1;   // weighting matrix, see WeightingMatrix
    uint8_t dc_plane      = 0;   // 1 = DC plane only, planar interpolation
    int8_t  qp_add        = 0;
};

enum WeightingMatrix : uint8_t {
    WM_FLAT     = 0,  // no perceptual weighting; text and near-lossless
    WM_LUMA     = 1,  // JPEG-luma-like, the default
    WM_PERIPH   = 2,  // strong high-frequency roll-off: this is the blur
    WM_CHROMA   = 3
};

static constexpr int kLadderSteps = 5;   // step 0 (untouched) .. step 4

enum ChromaMode : uint8_t {
    CHROMA_444 = 0,
    CHROMA_420 = 1,
    CHROMA_410 = 2   // chroma at 1/4 in both axes
};

// ============================================================ config ======

struct RateConfig {
    // ---- perceptual QP offsets, PAPER.md 5.2 -----------------------------
    // dQ_ecc by foveation level; the fovea/mid split inside level 0 uses
    // FoveationConfig::mid_ring_deg.
    int8_t dq_ecc_fovea   = 0;
    int8_t dq_ecc_mid     = 2;
    int8_t dq_ecc_half    = 4;
    int8_t dq_ecc_quarter = 6;
    float  mid_ring_deg   = 8.0f;   // level-0 tiles beyond this are mid ring

    // dQ_motion from retinal slip in deg/s: 0 below 10, +2 at 30, +4 at 60,
    // +6 above 100, linear in between.
    float  slip_knots[4]  = {10.0f, 30.0f, 60.0f, 100.0f};
    int8_t dq_head_fast   = 2;      // extra, global, above head_fast_deg_s
    float  head_fast_deg_s = 120.0f;

    // dQ_lum
    float  lum_dark       = 16.0f;
    float  lum_bright     = 220.0f;
    int8_t dq_lum_dark    = -2;
    int8_t dq_lum_bright  = 2;

    // dQ_act: +strength * (log_var - mean log_var), clamped.  NOTE the sign:
    // PAPER.md 5.2 writes a minus but then says "flat tiles get finer steps,
    // busy tiles coarser", which is the x264 rule and needs a plus.  We
    // implement the prose.  See RATECONTROL.md appendix A.2.
    float  act_strength   = 1.0f;
    float  act_clamp      = 4.0f;

    // dQ_class: structure is worth bits.
    int8_t dq_class[4]    = { 0, 0, -2, -4 }; // Flat, Texture, Edge, Text

    // Per-class coded-QP ceiling ("never block") and floor ("do not
    // overspend").  PAPER.md 4.6.1 calls these the per-class QP floors; they
    // are quality floors, i.e. ceilings on the coded QP.
    uint8_t qp_ceiling[4] = { 38, 38, 32, 26 };
    uint8_t qp_floor[4]   = {  8,  8,  4,  0 };
    uint8_t qp_max        = 51;
    uint8_t qp_min        = 0;

    // Cg plane offset relative to Co, PAPER.md 5.2.
    int8_t  cg_qp_offset  = 2;

    // ---- bit model, PAPER.md 4.6 ----------------------------------------
    // predicted_bits = a_t * s_fov^2 * gain(step) * 2^(-qp/6)
    // a_t is normalised to a full-resolution tile, so a resolution change
    // does not invalidate the model.
    float a_init          = 32768.0f;  // ~0.25 bpp at QP 30 on 4096 samples
    float a_exponent      = 0.6f;      // a *= (actual/predicted)^0.6
    float a_ratio_clamp   = 4.0f;      // per-frame ratio clamp
    float a_min           = 512.0f;
    float a_max           = 2097152.0f;

    // Bit-domain gains of the ladder mechanisms.  These are the numbers the
    // encoder measurement has to replace; see RATECONTROL.md appendix A.4.
    float gain_wm_periph  = 0.72f;   // low-pass weighting matrix
    float gain_dc_plane   = 0.12f;   // DC plane only

    // ---- complexity ------------------------------------------------------
    float cplx_clamp_lo   = 0.25f;
    float cplx_clamp_hi   = 4.0f;
    float cplx_text_floor = 1.0f;   // text tiles never get below an even share

    // A tile whose warped SAD is under this (in the same units as the
    // complexity input, mean absolute difference per pixel) is SKIP_WARP.
    float skip_sad        = 1.0f;
    // Tiles allocated fewer than this many bits are not worth a header.
    float min_tile_bits   = 96.0f;
    // Rounds of "skip the tiles that cannot afford a header, then reallocate
    // their bits to the survivors".
    // The last round always commits without dropping anything further, so
    // the bits of every dropped tile are handed back to the survivors.
    int   skip_rounds     = 3;

    // ---- head speed ------------------------------------------------------
    float head_speed_ref  = 120.0f;  // deg/s that normalises to 1.0
    float head_speed_k    = 0.5f;    // percep_t = 1/(1 + k*head_speed_norm)

    // ---- scene cuts, PAPER.md 4.6 ---------------------------------------
    float scene_cut_intra_ratio = 0.5f;
    float scene_cut_cap         = 1.5f;
    int   scene_cut_recovery    = 30;   // frames over which the debt is repaid
    float overrun_trigger       = 1.15f;

    // ---- pressure search -------------------------------------------------
    int   pressure_steps  = 16;   // P is searched on a grid of 16 quarter-steps
    // Pressure may rise immediately (the budget is a hard constraint) but is
    // released at most this much per frame, so the periphery does not flap
    // between two ladder steps.  Same asymmetry as the decode governor.
    float pressure_slew_down = 0.25f;
    // How much of the ladder's total achievable relief the allocator is
    // content to leave on the table.  See allocate.cpp; 0 would chase
    // clamps the ladder cannot fix.
    float pressure_slack  = 0.15f;
};

// ============================================================ governor ====

struct GovernorConfig {
    float target_fraction   = 0.40f;  // of the frame period
    float step_down_ratio   = 1.10f;
    int   step_down_frames  = 3;
    float panic_ratio       = 1.50f;  // one frame is enough
    float step_up_ratio     = 0.70f;
    int   step_up_frames    = 180;    // 2 s at 90 Hz
    int   max_knob          = 5;
    float fovea_shrink      = 0.90f;  // knob 3
    float low_refresh_hz    = 72.0f;  // knob 5
    float high_refresh_hz   = 90.0f;
};

// The five ordered knobs of PAPER.md 4.7 as a state.  `knob_level` is how
// many of them are engaged.
struct KnobState {
    int   knob_level          = 0;
    bool  drop_enh_class_c    = false;  // knob 1
    bool  class_c_base_only   = false;  // knob 2
    float fovea_region_scale  = 1.0f;   // knob 3
    bool  drop_enh_class_b    = false;  // knob 4
    float refresh_hz          = 90.0f;  // knob 5

    // Single-layer mapping (see RATECONTROL.md 5.3).  A v1 stream with one
    // native layer has no enhancement layer to drop, so knobs 1, 2 and 4 are
    // mapped onto the tools that do reduce coded samples: a res_level floor
    // and the DC-plane mode.  Class B is foveation level 1, class C level 2.
    uint8_t res_floor_class_b = 0;
    uint8_t res_floor_class_c = 0;
    bool    class_c_dc_plane  = false;
};

class Governor {
public:
    explicit Governor(GovernorConfig cfg = {}) : cfg_(cfg) {}

    // decode_us: measured decode time of the last frame.
    // frame_period_us: the current display period.
    // deadline_miss: the frame missed its band deadline.
    const KnobState& update(float decode_us, float frame_period_us,
                            bool deadline_miss = false);

    const KnobState& state() const { return state_; }
    const GovernorConfig& config() const { return cfg_; }
    void reset();

    int over_run()  const { return over_; }
    int under_run() const { return under_; }

private:
    void apply_level();

    GovernorConfig cfg_;
    KnobState      state_;
    int            over_  = 0;
    int            under_ = 0;
};

// ============================================================ allocate ====

// Everything the allocator needs for one frame.  Spans, not owned.
struct FrameInputs {
    const nxfov::FoveationMap* fov = nullptr;
    std::span<const uint8_t> cls;         // from classify_tiles
    std::span<const float>   complexity;  // warped SAD per tile, MAD per pixel
    std::span<const float>   slip_deg_s;  // residual MV magnitude, deg/s
    const TileStats*         stats = nullptr;

    // The temporal ladder's decision for this frame, from
    // RefreshScheduler::schedule() (nxrc/refresh.hpp): 1 = code this tile as
    // WARP_SKIP whatever its residual, the decoder's warp keeps it moving
    // and only the residual correction is withheld.  May be empty.  The
    // allocator treats it exactly like a static tile: weight zero, one bit
    // in the row bitmap, and its share redistributed to the survivors by the
    // existing skip_rounds loop.  RATECONTROL.md 8.
    std::span<const uint8_t> force_warp_skip;

    float head_speed_deg_s = 0.0f;
    float intra_ratio      = 0.0f;   // fraction of tiles above the intra threshold
    bool  force_scene_cut  = false;
};

// Struct of arrays, one entry per tile.
struct AllocResult {
    std::vector<uint8_t> qp;
    std::vector<uint8_t> res_level;
    std::vector<uint8_t> chroma_mode;
    std::vector<uint8_t> wm_id;
    std::vector<uint8_t> ladder_step;
    std::vector<uint8_t> dc_plane;
    std::vector<uint8_t> skip;
    std::vector<float>   predicted_bits;
    std::vector<float>   weight;

    // Frame level
    float   pressure        = 0.0f;  // P in [0, 4]
    float   budget_bits     = 0.0f;  // budget actually used after debt / cut
    float   requested_bits  = 0.0f;  // what the caller passed in
    float   predicted_total = 0.0f;
    bool    scene_cut       = false;
    int8_t  cg_qp_offset    = 2;
    int     clamped_ceiling = 0;     // tiles pinned at their class ceiling
    int     clamped_floor   = 0;     // tiles pinned at their class floor
    int     skipped         = 0;
    int     skipped_temporal = 0;    // of those, skipped by the temporal ladder

    size_t size() const { return qp.size(); }
    void   resize(size_t n);
};

// Quality of a tile in QP-equivalent units, larger is worse.  This is the
// model's un-degraded QP: qp minus the QP the ladder and the foveation
// resolution bought back.  It is the scalar the monotonicity and ordering
// invariants are stated in.
float effective_qp(const AllocResult& a, const nxfov::FoveationMap& fov,
                   size_t i, const RateConfig& cfg);

// The ladder table, exposed for the tests and the documentation generator.
const LadderStep& ladder_step(uint8_t cls, int step);
int  ladder_max_step(uint8_t cls);

// How much detail one ladder step removes, in QP-equivalent units (larger is
// worse, 0 = untouched).  This is the ONLY scalar in which ladder steps are
// comparable across classes: step index 2 means "res 1/2" for a texture tile
// and "untouched" for a text tile, so comparing step indices is meaningless.
// severity = -6*log2(bit-domain gain) + the step's QP adder.
float ladder_severity(uint8_t cls, int step, uint8_t fov_level,
                      const RateConfig& cfg);
// The same quantity read back off a committed allocation.
float ladder_severity(const AllocResult& a, const nxfov::FoveationMap& fov,
                      size_t i, const RateConfig& cfg);

class RateController {
public:
    explicit RateController(RateConfig cfg = {});

    void reset(size_t tile_count);

    // frame_budget_bits: B from the transport controller (target bit/s / fps
    // minus FEC and header overhead).
    const AllocResult& allocate(float frame_budget_bits,
                                const FrameInputs& in);

    // Feed back the real per-tile bit counts of the frame just encoded.
    void update_model(std::span<const uint32_t> actual_bits);
    void update_model(std::span<const float> actual_bits);

    const AllocResult& last() const { return out_; }
    const RateConfig&  config() const { return cfg_; }
    RateConfig&        config() { return cfg_; }
    std::span<const float> model() const { return a_; }

    // Debt carried into the next frame (scene-cut burst and overrun).
    float debt_bits() const { return debt_; }
    int   debt_frames() const { return debt_frames_; }
    void  set_knobs(const KnobState& k) { knobs_ = k; }
    const KnobState& knobs() const { return knobs_; }

    // Exposed for the simulator: the last frame's total actual bits.
    float last_actual_bits() const { return last_actual_; }

private:
    float effective_budget(float b, bool scene_cut);
    void  compute_weights(const FrameInputs& in);
    float run_pressure(float p, const FrameInputs& in, float budget,
                       bool commit);

    RateConfig  cfg_;
    KnobState   knobs_;
    AllocResult out_;

    std::vector<float> a_;         // per-tile bit model state
    std::vector<float> w_;         // per-tile allocation weight
    std::vector<float> dq_;        // per-tile perceptual QP offset
    std::vector<float> bits_;      // per-tile allocated bits (scratch)
    std::vector<uint8_t> pinned_;  // scratch for the redistribution loop
    std::vector<uint8_t> res_floor_;  // governor knob res_level floor per tile
    std::vector<uint8_t> force_dc_;   // governor knob DC-plane force per tile

    float pressure_    = 0.0f;
    float debt_        = 0.0f;
    int   debt_frames_ = 0;
    float last_actual_ = 0.0f;
    bool  have_last_   = false;
};

} // namespace nxrc

#endif // NXRC_NXRC_HPP
