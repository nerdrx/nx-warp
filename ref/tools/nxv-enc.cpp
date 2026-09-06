// nxv-enc: encode raw planar 8-bit YUV frames to an .nxv stream.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"
#include "base_refresh.h"
#include "nxrc/encdrive.hpp"

// ------------------------------------------------------- tiny JSON scraping
//
// The `.poses.json` reader below is a scraper, not a parser, exactly like the
// `orientation_xyzw` loop it sits next to: this tool has no JSON dependency and
// the sidecar is machine-written with a known shape (see docs/WARP.md 2.1).
// Both helpers take the FIRST occurrence of the key, which is the top-level one
// because `frames` is last in the document and never contains these keys.

// Value of a string-valued key, or "" if absent.
static std::string json_string(const std::string &txt, const std::string &key) {
    size_t k = txt.find(key);
    if (k == std::string::npos) return {};
    size_t c = txt.find(':', k + key.size());
    if (c == std::string::npos) return {};
    size_t q = txt.find('"', c);
    if (q == std::string::npos) return {};
    size_t e = txt.find('"', q + 1);
    if (e == std::string::npos) return {};
    return txt.substr(q + 1, e - q - 1);
}

// `"fov_deg": {"h": <num>, "v": <num>}`.  Returns false if it is not there or
// is not a pair of finite positive angles, in which case the caller keeps its
// own default and says so.
static bool json_fov_deg(const std::string &txt, double *h, double *v) {
    size_t k = txt.find("\"fov_deg\"");
    if (k == std::string::npos) return false;
    size_t brace = txt.find('}', k);
    if (brace == std::string::npos) return false;
    const std::string obj = txt.substr(k, brace - k);
    auto num = [&](const char *key, double *out) -> bool {
        size_t p = obj.find(key);
        if (p == std::string::npos) return false;
        size_t c = obj.find(':', p + std::strlen(key));
        if (c == std::string::npos) return false;
        char *end = nullptr;
        const double d = std::strtod(obj.c_str() + c + 1, &end);
        if (end == obj.c_str() + c + 1) return false;
        // A degenerate or reflex FOV would make tan() blow up or change sign
        // inside make_K(); refuse it here rather than emit a wild matrix.
        if (!(d > 0.0 && d < 180.0)) return false;
        *out = d;
        return true;
    };
    return num("\"h\"", h) && num("\"v\"", v);
}

static void usage() {
    std::fprintf(stderr,
        "usage: nxv-enc --in file.yuv --w W --h H --pix yuv444p|yuv420p\n"
        "               --qp N [--res-map file] --out out.nxv\n"
        "optional:\n"
        "  --qp-map FILE        per-tile QP bytes, tile_count per frame\n"
        "  --frames N           encode at most N frames\n"
        "  --lossless           QP 0 + transform skip (bit exact)\n"
        "  --tskip off|on|auto  transform-skip decision (default off)\n"
        "  --nsub 0..5|auto     rANS lane count log2 (default 3 = 8 lanes)\n"
        "  --matrix 0..3        frame weighting matrix (default 1)\n"
        "  --wm 0..3|auto       per-tile weighting matrix id (default 0)\n"
        "  --no-rdo             plain dead-zone quantizer (default: RD trellis)\n"
        "  --rdo-lambda F       RD lambda scale (default 0.22, fitted)\n"
        "  --qp-search N        try per-tile qp_delta in [-N, +N] (default 0)\n"
        "  --qp-search-step N   spacing of those candidates (default 2)\n"
        "  --rdoq-effort N      1 fast, 2 medium, 3 full trellis candidates\n"
        "  --dc-lambda F        DC-plane lambda relative to the AC planes\n"
        "  --no-dc-rdoq         leave the DC plane on the dead-zone quantizer\n"
        "  --me-effort N        1 fast, 2 hierarchical+SATD, 3 +true-RD qpel\n"
        "  --no-lambda-class    one lambda for every tile, whatever its class\n"
        "  --lambda-class A,B,C,D  per-class lambda gain: flat, texture,\n"
        "                       edge, text (default 1,1,1,1)\n"
        "  --intra-dir on|off|layer  directional intra (tool 17); `layer`\n"
        "                       predicts the DC-plane residual instead\n"
        "  --intra-dir-cand N   modes RD-checked per block (default 2)\n"
        "  --ctx v1|v2|v3       12, 16 or 27 entropy contexts (tools 21, 25);\n"
        "                       v3 is off by default, see docs/TOOLBITS.md 7\n"
        "  --tab v1|v2          transmitted-table coding: flat 5-bit or compact\n"
        "                       (tool 26); v2 is off by default\n"
        "  --table-iters N      Lloyd iterations refining the per-frame table sets\n"
        "                       (0 = off, default 3)\n"
        "  --xform 8|16|32|auto transform size per tile (tool 27, default 8);\n"
        "                       --split4x4 applies only where this is 8\n"
        "  --no-sign-hide       code every sign (default: hide one per unit)\n"
        "  --split4x4 on|off    per-block 4x4 transform split (default on)\n"
        "  --cfl on|off         chroma-from-luma intra mode (default on)\n"
        "  --entropy rans|lite-fixed|lite-rice\n"
        "                       entropy tool (default rans; the lite variants\n"
        "                       set tool bit 30 and are OFF by default -- the\n"
        "                       decoder negotiates them from its own measured\n"
        "                       Pass A time)\n"
        "  --chroma-qp-off N    chroma QP offset\n"
        "  --custom-tables      derive and transmit probability tables\n"
        "  --tile-420           code 4:2:0 tiles inside a 4:4:4 stream\n"
        "  --rgb                input planes are R,G,B; apply YCoCg-R\n"
        "  --color-space S      unspecified|yuv709l|yuv709f (YCbCr passthrough)\n"
        "  --stats              print where the bits went\n"
        "  --quiet\n"
        "Phase 2 (inter prediction):\n"
        "  --inter on|off       inter prediction (default off = Phase 1)\n"
        "  --eyes 1|2           2 = the input frame is side-by-side stereo and\n"
        "                       --w is its FULL width; each eye is a picture\n"
        "  --poses FILE         .poses.json sidecar; the per-frame head\n"
        "                       orientation the warp matrix is derived from\n"
        "  --fov H,V            field of view in degrees; overrides the\n"
        "                       sidecar's `fov_deg`. Default 95,95, used only\n"
        "                       when neither is given -- a wrong FOV is a\n"
        "                       silently wrong warp (docs/WARP.md 2.1)\n"
        "  --intra-period N     rolling intra refresh period in frames\n"
        "                       (default 180; 1 = every tile every frame).\n"
        "                       Under --drift-refresh this is the hard age cap\n"
        "  --drift-refresh on|off  drive the refresh from the measured drift of\n"
        "                       the encoder's client shadow instead of the\n"
        "                       fixed 1-in-T permutation (default on)\n"
        "  --drift-gate F       --drift-refresh gate, multiples of the\n"
        "                       quantiser noise floor qstep^2/12 (default 4)\n"
        "  --atlas on|off       the per-tile ATLAS reference, tool bit 31.\n"
        "               A skipped tile writes no reference pixels; the\n"
        "               normative output is the atlas, not the picture, and\n"
        "               the displayed picture is the non-normative one-step\n"
        "               warp of 13.12.5.  Requires --inter on, excludes\n"
        "               --stereo on.\n"
        "  --row-present on|off elide the 12-byte header of a tile row with\n"
        "               no coded tile, tool bit 32\n"
        "  --atlas-nbr on|off   NEIGHBOUR-AWARE GATHER, tool bit 33: a\n"
        "               prediction sample landing outside the tile's own\n"
        "               atlas position is fetched through the matrix of the\n"
        "               entry it lands in, resolved with the co-located\n"
        "               matrix.  NORMATIVE; requires --atlas on\n"
        "  --atlas-coarse R     TWO-LEVEL REFRESH: a tile being refreshed\n"
        "               lands at res_level R (1 or 2) and a later frame\n"
        "               refines it to res_level 0.  Encoder policy only, no\n"
        "               syntax: the refinement is an ordinary coded tile\n"
        "               predicting from the upsampled coarse pixels\n"
        "  --atlas-coarse-budget N  at most N refinements per frame\n"
        "  --atlas-sched-bytes N  RANKED REFRESH SCHEDULER: an ATLAS frame\n"
        "               spends at most N bytes on coded tiles, choosing them\n"
        "               by predicted display error rather than letting every\n"
        "               tile decide alone.  Encoder-side rate control\n"
        "  --atlas-coarse-disp T  SINGLE-LEVEL coarse refresh: a tile whose\n"
        "               corner displacement since its last landing exceeds T\n"
        "               luma samples lands at res_level 1 and is not refined.\n"
        "               Same metric as the 13.12.11 mode trigger\n"
        "  --atlas-coarse-stats P  write the two-level accounting to P\n"
        "  --atlas-picture-disp N  13.12.11: code a PICTURE frame -- the\n"
        "               ordinary model, every tile reconstructed, the atlas\n"
        "               rebuilt from it -- once the worst corner displacement\n"
        "               in the atlas passes N luma samples.  Requires\n"
        "               --atlas on; sets tool bit 34\n"
        "  --atlas-picture-spacing N  never two PICTURE frames closer than N\n"
        "               frames apart\n"
        "  --atlas-picture-period N  a PICTURE frame every N frames whatever\n"
        "               the motion\n"
        "  --atlas-skip-margin N  ENCODER ONLY, no syntax: a tile may be\n"
        "               skipped only while the composed displacement at all\n"
        "               four of its corners is under N luma samples.  0 =\n"
        "               unbounded (the default, and the rule as measured)\n"
        "  --atlas-gen-max N    invalidate an atlas entry after N composition\n"
        "               steps; 0 = the warp envelope alone bounds staleness\n"
        "  --atlas-dump P       write the encoder's shadow atlas table after\n"
        "               each frame to P (64 bytes per tile per frame), which\n"
        "               is what a conformance comparison reads\n"        "  --near-skip on|off   DC-correction tile form, tool bit 28\n"
        "                       (default on)\n"
        "  --quad-mv on|off     four vectors per tile, one per 32x32\n"
        "                       quadrant, tool bit 29 (default on)\n"
        "  --ref-sel 0..2       reference distance inter tiles ask for\n"
        "  --stereo on|off      STEREO inter-view mode on the right eye\n"
        "  --mv-range N         coarse search radius in samples (default 16)\n"
        "  --int-decision on|off  the GPU encoder's integer mode decision\n"
        "                       (ADR-0028).  All-i64: no trial encode, no\n"
        "                       log2, no double.  Reproducible on a GPU and\n"
        "                       worse than the default; it exists so the two\n"
        "                       sides stay byte-comparable.  Default off.\n"
        "  --int-lambda N       its SAD-domain lambda, Q8 per quantiser step\n"
        "                       (default 45)\n"
        "  --int-rdoq N         integer requantiser: 0 off, 1 drop a +-1\n"
        "                       coefficient that does not pay for itself.\n"
        "                       The GPU encoder's effort 1; unlike\n"
        "                       --rdoq-effort it is exact in integers\n"
        "  --int-intra-mad F    its INTRA fallback: mean |residual| per luma\n"
        "                       sample above which the tile codes intra\n"
        "                       (default 9)\n"
        "  --skip-thresh F      WARP_SKIP early-out gate, multiples of the\n"
        "                       quantiser noise floor qstep^2/12 (default 1)\n"
        "  --skip-map FILE      per-tile force_warp_skip flags, tile_count\n"
        "                       bytes per frame (docs/RATECONTROL.md 8.7).\n"
        "                       Applied after the mode search; the encoder\n"
        "                       overrides it where a coded tile is required\n"
        "  --mode-lambda F      lambda scale of the per-tile mode decision,\n"
        "                       relative to the trellis (default 1.0)\n"
        "Presets (set the effort knobs above; anything given after --preset\n"
        "on the command line still wins):\n"
        "  --chroma-weight Q8   weight of chroma squared error in the\n"
        "                       encoder's distortion, Q8; 256 = 1.0 = as the\n"
        "                       samples fall (the default).  Below 256 buys\n"
        "                       PSNR-Y and 6:1:1 at the cost of absolute\n"
        "                       chroma fidelity: a perceptual tuning knob,\n"
        "                       not a coding gain.  Quote both metrics.\n"
        "  --preset fast        rdoq 1, me 1, 1 intra mode, no QP search\n"
        "  --preset medium      rdoq 2, me 2, 2 intra modes, no QP search\n"
        "                       (the default)\n"
        "  --preset slow        rdoq 3, me 3, 4 intra modes, per-tile QP\n"
        "                       search +-2\n"
        "  --threads N          code the frame's tiles on N threads.  0 =\n"
        "                       auto (hardware concurrency, capped at 16),\n"
        "                       1 = single-threaded.  The bitstream is\n"
        "                       byte-identical at every N\n"
        "Perceptual rate control (docs/RATECONTROL.md; no syntax change):\n"
        "  --rc                 run the rate-control library per frame and\n"
        "                       feed its per-tile qp / res_level / wm_id /\n"
        "                       force_warp_skip into the encoder.  Overrides\n"
        "                       --qp, --qp-map, --res-map, --skip-map, --wm\n"
        "                       and --matrix\n"
        "  --rc-bitrate M       target bit rate in Mbit/s (default 40)\n"
        "  --rc-fov on|off      the foveation map (default on)\n"
        "  --rc-temporal on|off the per-tile refresh scheduler (default on)\n"
        "  --gaze x,y           fixation inside one eye, normalised to\n"
        "                       [-1,1], +x right +y up.  Default: no tracker,\n"
        "                       i.e. the fixed-foveation eye box on the axis\n"
        "  --rc-panel N         pixels per eye of the PANEL the foveation\n"
        "                       decision is made for (default 2160, Pico 4)\n"
        "  --rc-act F           strength of the activity term dQ_act in QP\n"
        "                       per octave of log-variance (default 1; 0\n"
        "                       switches adaptive quantisation off, which is\n"
        "                       what leaves the eccentricity term in charge)\n"
        "  --rc-fps F           display rate the budget is per-frame of\n"
        "                       (default 90)\n"
        "  --rc-fov-deg H,V     render FOV of one eye the foveation map is\n"
        "                       built for (default 81.2,81.2, the Pico 4).\n"
        "                       This is the headset, not the clip: --fov is\n"
        "                       the FOV the clip was RENDERED with and drives\n"
        "                       the warp\n"
        "  --rc-map FILE        write the per-tile decision of every frame as\n"
        "                       CSV, for the results harness\n");
}

// The frame weighting matrix rc mode declares the WM_ID tool with.  Any
// non-zero id would do; 1 is nxrc::WM_LUMA, the ladder's own default, so a
// tile the map does not touch keeps the matrix the ladder would have chosen.
static constexpr uint32_t WM_LUMA_ID = 1;

// Head angular rate between two OpenXR orientation quaternions, deg/s.
static double head_speed_deg_s(const std::array<double, 4> &a,
                               const std::array<double, 4> &b, double fps) {
    double dot = 0.0;
    for (int i = 0; i < 4; ++i) dot += a[i] * b[i];
    dot = std::fabs(dot) > 1.0 ? 1.0 : std::fabs(dot);
    return 2.0 * std::acos(dot) * 180.0 / 3.14159265358979323846 * fps;}

static bool read_exact(std::FILE *f, void *p, size_t n) {
    return std::fread(p, 1, n, f) == n;
}


// FNV-1a over the atlas pixels of every plane, appended to the table dump so
// that a comparison covers the whole normative output of 13.12 and not only
// its metadata.  Pixels are compared as a digest because the atlas is 3.4 MB
// an eye and a conformance vector should not be.
static void atlas_digest(const uint16_t *(*plane_fn)(const void *, int,
                                                     uint32_t *, uint32_t *,
                                                     uint32_t *),
                         const void *ctx, uint8_t out[32]) {
    for (int p = 0; p < 4; ++p) {
        uint64_t h = 1469598103934665603ull;
        uint32_t w = 0, ht = 0, stride = 0;
        const uint16_t *d = plane_fn(ctx, p, &w, &ht, &stride);
        if (d)
            for (uint32_t y = 0; y < ht; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    const uint16_t v = d[(size_t)y * stride + x];
                    h = (h ^ (v & 0xff)) * 1099511628211ull;
                    h = (h ^ (v >> 8)) * 1099511628211ull;
                }
        for (int k = 0; k < 8; ++k) out[p * 8 + k] = (uint8_t)(h >> (8 * k));
    }
}

static const uint16_t *atlas_plane_shim(const void *c, int pl, uint32_t *w,
                                        uint32_t *h, uint32_t *st) {
    return nxvc_encoder_atlas_plane((const nxvc_encoder *)c, pl, w, h, st);
}
int main(int argc, char **argv) {
    std::string in, out, pix = "yuv420p", resmap_path, qpmap_path;
    int W = 0, H = 0, qp = 24, frames = -1, matrix = 1, chroma_qp_off = 0;
    int lossless = 0, tile420 = 0, custom_tables = 1, rgb = 0, quiet = 0;
    int tskip = 0, nsub = 255, stats = 0;  // nsub 255 = auto lane count
    int color_space = 0;
    int rdo = 1, rdo_lambda_q8 = 0, qp_search = 0, wm = 0;
    // These mirror nxvc_config_default(): the v2 intra tools are on, and the
    // entropy and context package is OFF (docs/TOOLBITS.md 7).
    int intra_dir = 1, intra_dir_layer = 0, ctx_v2 = 1, ctx_v3 = 0;
    int tab_v2 = 0, dir_cand = 0, table_iters = -1;
    int xform = 0;   // tool 27: 0 = 8x8, 1 = 16x16, 2 = 32x32, 255 = auto
    int entropy_lite = 0;   // tool 30: 0 = rANS, 1 = FIXED, 2 = RICE
    int sign_hide = 1;
    int split4x4 = 1, cfl = 1;
    int inter = 0, eyes = 1, intra_period = 180, ref_sel = 0, stereo = 0;
    // --- the atlas reference (SYNTAX.md 13.12, ADR-0029)
    int atlas = 0, row_present = 0, atlas_gen_max = 0;
    int atlas_nbr = 0, atlas_skip_margin = 0;
    int atlas_picture_disp = 0, atlas_picture_spacing = 0;
    int atlas_picture_period = 0;
    std::string atlas_base_path;
    int atlas_base_margin = 8;
    // TWO-LEVEL REFRESH (ADR-0029).  A tile that is being refreshed lands at
    // res_level R -- cheap, few bytes -- and a later frame refines it to full
    // resolution.  The refinement needs NO syntax: under the atlas a coded
    // tile predicts from its own entry, which holds the upsampled coarse
    // pixels, so coding it again at res_level 0 IS a residual on the coarse
    // tile rather than a re-code of it.  The whole policy is therefore a
    // per-frame res_map, which the library already takes.
    int coarse_level = 0;      // 0 = off, 1 = 32x32, 2 = 16x16
    int coarse_budget = 0;     // max refinements a frame may spend; 0 = all
    std::string coarse_stats_path;
    // SINGLE-LEVEL COARSE REFRESH as a rate-control policy: a tile whose
    // pixels have drifted more than T luma samples since they were last
    // coded lands at res_level 1 and is never refined.  T is read against
    // exactly the metric 13.12.11.1's mode trigger uses, through the codec's
    // own nxvc_encoder_atlas_stale_tiles(), so the two cannot disagree.
    int coarse_disp = 0;       // T in luma samples; 0 = off
    int sched_bytes = 0;       // ranked-scheduler budget, bytes/frame; 0 = off
    std::string atlas_dump;
    int mv_range = 16, skip_thresh = 0, mode_lambda = 0;
    int int_decision = 0, int_lambda = 0, int_intra_mad = 0, int_rdoq = 0;
    int int_coded_vectors = 2;
    int threads = 0;   // 0 = auto
    // These mirror nxvc_config_default(): the inter-efficiency tools that the
    // measurement supports are on, sub-tile intra is not.
    int drift_refresh = 1, drift_gate = 0, near_skip = 1, quad_mv = 1;
    // The rate-distortion package's effort knobs.  0 is "take the preset's
    // value" for every one of them.
    int preset = 0;
    int rdoq_effort = 0, me_effort = 0, lambda_class_off = 0, qp_step = 0;
    int lambda_class[4] = {0, 0, 0, 0};
    int dc_lambda = 0, dc_off = 0, chroma_weight = 0;
    double fov_h = 95.0, fov_v = 95.0;
    bool fov_from_cli = false;
    std::string poses_path, skipmap_path;
    // `--rc` is OFF by default and stays off.  The spatial ladder's measured
    // result is NEGATIVE -- the periphery is over-degraded and every foveated
    // metric loses -- so the package merges for its wiring, its two ABI items
    // and its two rc/ fixes, not for the ladder.  Same discipline as the tool
    // bits: a package whose measured result is negative does not become the
    // default path.  See the open issue in docs/RATECONTROL.md.
    int rc_on = 0, rc_fov = 1, rc_temporal = 1, rc_panel = 2160;
    double rc_bitrate = 40.0, rc_fps = 90.0, rc_act = 1.0;
    double rc_fov_h = 81.2, rc_fov_v = 81.2;
    double gaze_x = 0.0, gaze_y = 0.0;
    bool gaze_valid = false;
    std::string rc_map_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--in") in = val();
        else if (a == "--out") out = val();
        else if (a == "--w") W = std::atoi(val());
        else if (a == "--h") H = std::atoi(val());
        else if (a == "--pix") pix = val();
        else if (a == "--qp") qp = std::atoi(val());
        else if (a == "--res-map") resmap_path = val();
        else if (a == "--qp-map") qpmap_path = val();
        else if (a == "--frames") frames = std::atoi(val());
        else if (a == "--lossless") lossless = 1;
        else if (a == "--tile-420") tile420 = 1;
        else if (a == "--custom-tables") custom_tables = 1;
        else if (a == "--no-custom-tables") custom_tables = 0;
        else if (a == "--rgb") rgb = 1;
        else if (a == "--color-space") {
            std::string v = val();
            if (v == "unspecified") color_space = 0;
            else if (v == "yuv709l") color_space = 1;
            else if (v == "yuv709f") color_space = 2;
            else { std::fprintf(stderr, "--color-space: unspecified|yuv709l|yuv709f\n"); return 2; }
        }
        else if (a == "--inter") {
            std::string v = val();
            if (v == "on") inter = 1;
            else if (v == "off") inter = 0;
            else { std::fprintf(stderr, "--inter: on|off\n"); return 2; }
        }
        else if (a == "--stereo") {
            std::string v = val();
            if (v == "on") stereo = 1;
            else if (v == "off") stereo = 0;
            else { std::fprintf(stderr, "--stereo: on|off\n"); return 2; }
        }
        else if (a == "--atlas") {
            std::string v = val();
            if (v == "on") atlas = 1;
            else if (v == "off") atlas = 0;
            else { std::fprintf(stderr, "--atlas: on|off\n"); return 2; }
        }
        else if (a == "--row-present") {
            std::string v = val();
            if (v == "on") row_present = 1;
            else if (v == "off") row_present = 0;
            else { std::fprintf(stderr, "--row-present: on|off\n"); return 2; }
        }
        else if (a == "--atlas-nbr") {
            std::string v = val();
            if (v == "on") atlas_nbr = 1;
            else if (v == "off") atlas_nbr = 0;
            else { std::fprintf(stderr, "--atlas-nbr: on|off\n"); return 2; }
        }
        else if (a == "--atlas-skip-margin") atlas_skip_margin = std::atoi(val());
        else if (a == "--atlas-picture-disp")
            atlas_picture_disp = std::atoi(val());
        else if (a == "--atlas-picture-spacing")
            atlas_picture_spacing = std::atoi(val());
        else if (a == "--atlas-picture-period")
            atlas_picture_period = std::atoi(val());
        else if (a == "--atlas-base") atlas_base_path = val();
        else if (a == "--atlas-base-margin")
            atlas_base_margin = std::atoi(val());
        else if (a == "--atlas-coarse") coarse_level = std::atoi(val());
        else if (a == "--atlas-coarse-budget")
            coarse_budget = std::atoi(val());
        else if (a == "--atlas-coarse-stats") coarse_stats_path = val();
        else if (a == "--atlas-coarse-disp") coarse_disp = std::atoi(val());
        else if (a == "--atlas-sched-bytes") sched_bytes = std::atoi(val());
        else if (a == "--atlas-gen-max") atlas_gen_max = std::atoi(val());
        else if (a == "--atlas-dump") atlas_dump = val();
        else if (a == "--eyes") eyes = std::atoi(val());
        else if (a == "--poses") poses_path = val();
        else if (a == "--skip-map") skipmap_path = val();
        else if (a == "--mode-lambda")
            mode_lambda = (int)(std::atof(val()) * 256.0 + 0.5);
        else if (a == "--fov") {
            std::string v = val();
            size_t c = v.find(',');
            fov_h = std::atof(v.c_str());
            fov_v = c == std::string::npos ? fov_h : std::atof(v.c_str() + c + 1);
            fov_from_cli = true;
        }
        else if (a == "--intra-period") intra_period = std::atoi(val());
        else if (a == "--drift-refresh") {
            std::string v = val();
            if (v == "on") drift_refresh = 1;
            else if (v == "off") drift_refresh = 0;
            else { std::fprintf(stderr, "--drift-refresh: on|off\n"); return 2; }
        }
        else if (a == "--drift-gate")
            drift_gate = (int)(std::atof(val()) * 256.0 + 0.5);
        else if (a == "--near-skip") {
            std::string v = val();
            if (v == "on") near_skip = 1;
            else if (v == "off") near_skip = 0;
            else { std::fprintf(stderr, "--near-skip: on|off\n"); return 2; }
        }
        else if (a == "--quad-mv") {
            std::string v = val();
            if (v == "on") quad_mv = 1;
            else if (v == "off") quad_mv = 0;
            else { std::fprintf(stderr, "--quad-mv: on|off\n"); return 2; }
        }
        else if (a == "--ref-sel") ref_sel = std::atoi(val());
        else if (a == "--mv-range") mv_range = std::atoi(val());
        else if (a == "--threads") threads = std::atoi(val());
        else if (a == "--skip-thresh")
            skip_thresh = (int)(std::atof(val()) * 256.0 + 0.5);
        else if (a == "--int-decision") {
            std::string v = val();
            if (v == "on") int_decision = 1;
            else if (v == "off") int_decision = 0;
            else { std::fprintf(stderr, "--int-decision: on|off\n"); return 2; }
        }
        else if (a == "--int-rdoq") int_rdoq = std::atoi(val());
        else if (a == "--int-lambda") int_lambda = std::atoi(val());
        else if (a == "--int-coded-vectors") {
            std::string v = val();
            if (v == "on") int_coded_vectors = 2;
            else if (v == "static") int_coded_vectors = 1;
            else if (v == "off") int_coded_vectors = 0;
            else {
                std::fprintf(stderr,
                             "--int-coded-vectors: off|static|on\n");
                return 2;
            }
        }
        else if (a == "--int-intra-mad")
            int_intra_mad = (int)(std::atof(val()) * 256.0 + 0.5);
        else if (a == "--rc") rc_on = 1;
        else if (a == "--rc-bitrate") rc_bitrate = std::atof(val());
        else if (a == "--rc-panel") rc_panel = std::atoi(val());
        else if (a == "--rc-map") rc_map_path = val();
        else if (a == "--rc-fps") rc_fps = std::atof(val());
        else if (a == "--rc-act") rc_act = std::atof(val());
        else if (a == "--rc-fov-deg") {
            std::string v = val();
            size_t c = v.find(',');
            rc_fov_h = std::atof(v.c_str());
            rc_fov_v = c == std::string::npos ? rc_fov_h
                                              : std::atof(v.c_str() + c + 1);
        }
        else if (a == "--rc-fov") {
            std::string v = val();
            if (v == "on") rc_fov = 1;
            else if (v == "off") rc_fov = 0;
            else { std::fprintf(stderr, "--rc-fov: on|off\n"); return 2; }
        }
        else if (a == "--rc-temporal") {
            std::string v = val();
            if (v == "on") rc_temporal = 1;
            else if (v == "off") rc_temporal = 0;
            else { std::fprintf(stderr, "--rc-temporal: on|off\n"); return 2; }
        }
        else if (a == "--gaze") {
            std::string v = val();
            size_t c = v.find(',');
            if (c == std::string::npos) {
                std::fprintf(stderr, "--gaze: x,y\n");
                return 2;
            }
            gaze_x = std::atof(v.c_str());
            gaze_y = std::atof(v.c_str() + c + 1);
            gaze_valid = true;
        }
        else if (a == "--quiet") quiet = 1;
        else if (a == "--stats") stats = 1;
        else if (a == "--matrix") matrix = std::atoi(val());
        else if (a == "--rdo") rdo = 1;
        else if (a == "--no-rdo") rdo = 0;
        else if (a == "--rdo-lambda") rdo_lambda_q8 = (int)(std::atof(val()) * 256.0 + 0.5);
        else if (a == "--qp-search") qp_search = std::atoi(val());
        else if (a == "--qp-search-step") qp_step = std::atoi(val());
        else if (a == "--dc-lambda") dc_lambda = (int)(std::atof(val()) * 256.0 + 0.5);
        else if (a == "--no-dc-rdoq") dc_off = 1;
        else if (a == "--rdoq-effort") rdoq_effort = std::atoi(val());
        else if (a == "--me-effort") me_effort = std::atoi(val());
        else if (a == "--no-lambda-class") lambda_class_off = 1;
        else if (a == "--lambda-class") {
            // flat,texture,edge,text gains; the fit lives in RESULTS-rdo-b.md
            std::string v = val();
            size_t pos = 0;
            for (int i = 0; i < 4 && pos <= v.size(); ++i) {
                size_t c = v.find(',', pos);
                std::string one = v.substr(pos, c == std::string::npos
                                                    ? std::string::npos
                                                    : c - pos);
                lambda_class[i] = (int)(std::atof(one.c_str()) * 256.0 + 0.5);
                if (c == std::string::npos) break;
                pos = c + 1;
            }
        }
        else if (a == "--chroma-weight") chroma_weight = std::atoi(val());
        else if (a == "--preset") {
            // One name for a point on the encode-time / rate curve.  Every
            // knob a preset sets can still be given explicitly afterwards,
            // because the parse is left to right and the last write wins.
            // The preset is set on the CONFIG, not expanded here: the
            // library resolves it (resolve_effort), so an SDK caller and this
            // CLI cannot disagree about what `slow` means.  Individual knobs
            // given after it still win, because they are separate fields and
            // 0 means "take the preset's value".
            std::string v = val();
            if (v == "fast") {
                preset = NXVC_PRESET_FAST;
            } else if (v == "medium") {
                preset = NXVC_PRESET_MEDIUM;
            } else if (v == "slow") {
                preset = NXVC_PRESET_SLOW;
            } else {
                std::fprintf(stderr, "--preset: fast|medium|slow\n");
                return 2;
            }
        }
        else if (a == "--intra-dir") {
            std::string v = val();
            if (v == "on") { intra_dir = 1; intra_dir_layer = 0; }
            else if (v == "layer") { intra_dir = 1; intra_dir_layer = 1; }
            else if (v == "off") intra_dir = 0;
            else { std::fprintf(stderr, "--intra-dir: on|off|layer\n"); return 2; }
        }
        else if (a == "--intra-dir-cand") dir_cand = std::atoi(val());
        else if (a == "--split4x4" && i + 1 < argc)
            split4x4 = std::string(argv[++i]) == "on" ? 1 : 0;
        else if (a == "--cfl" && i + 1 < argc)
            cfl = std::string(argv[++i]) == "on" ? 1 : 0;
        else if (a == "--sign-hide") sign_hide = 1;
        else if (a == "--no-sign-hide") sign_hide = 0;
        else if (a == "--table-iters") table_iters = std::atoi(val());
        else if (a == "--tab") {
            std::string v = val();
            if (v == "v2") tab_v2 = 1;
            else if (v == "v1") tab_v2 = 0;
            else { std::fprintf(stderr, "--tab: v1|v2\n"); return 2; }
        }
        else if (a == "--entropy") {
            std::string v = val();
            if (v == "rans") entropy_lite = 0;
            else if (v == "lite-fixed") entropy_lite = 1;
            else if (v == "lite-rice") entropy_lite = 2;
            else { std::fprintf(stderr, "--entropy: rans|lite-fixed|lite-rice\n"); return 2; }
        }
        else if (a == "--ctx") {
            std::string v = val();
            if (v == "v3") { ctx_v2 = 1; ctx_v3 = 1; }
            else if (v == "v2") { ctx_v2 = 1; ctx_v3 = 0; }
            else if (v == "v1") { ctx_v2 = 0; ctx_v3 = 0; }
            else { std::fprintf(stderr, "--ctx: v1|v2|v3\n"); return 2; }
        }
        else if (a == "--xform") {
            std::string v = val();
            if (v == "8") xform = 0;
            else if (v == "16") xform = 1;
            else if (v == "32") xform = 2;
            else if (v == "auto") xform = 255;
            else { std::fprintf(stderr, "--xform: 8|16|32|auto\n"); return 2; }
        }
        else if (a == "--wm") { std::string v = val(); wm = v == "auto" ? 255 : std::atoi(v.c_str()); }
        else if (a == "--chroma-qp-off") chroma_qp_off = std::atoi(val());
        else if (a == "--tskip") {
            std::string v = val();
            tskip = v == "on" ? 1 : (v == "auto" ? 2 : 0);
        } else if (a == "--nsub") {
            std::string v = val();
            nsub = v == "auto" ? 255 : std::atoi(v.c_str());
        } else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    if (in.empty() || out.empty() || W <= 0 || H <= 0) { usage(); return 2; }
    if (pix != "yuv420p" && pix != "yuv444p") {
        std::fprintf(stderr, "--pix must be yuv420p or yuv444p\n");
        return 2;
    }

    if (eyes != 1 && eyes != 2) {
        std::fprintf(stderr, "--eyes must be 1 or 2\n");
        return 2;
    }
    if (eyes == 2 && (W % 2)) {
        std::fprintf(stderr, "--eyes 2 needs an even --w (side-by-side)\n");
        return 2;
    }
    // The per-frame head orientations the warp matrix is derived from.  The
    // parser is deliberately tiny: it pulls the "orientation_xyzw" arrays out
    // of the sidecar in file order, which is frame order.
    std::vector<std::array<double, 4>> orient;
    if (!poses_path.empty()) {
        std::FILE *pf = std::fopen(poses_path.c_str(), "rb");
        if (!pf) { std::perror("open poses"); return 1; }
        std::string txt;
        char chunk[4096];
        size_t got;
        while ((got = std::fread(chunk, 1, sizeof chunk, pf)) > 0)
            txt.append(chunk, got);
        std::fclose(pf);
        const std::string key = "\"orientation_xyzw\"";
        size_t pos = 0;
        while ((pos = txt.find(key, pos)) != std::string::npos) {
            size_t lb = txt.find('[', pos);
            size_t rb = txt.find(']', lb == std::string::npos ? pos : lb);
            if (lb == std::string::npos || rb == std::string::npos) break;
            std::array<double, 4> q{0, 0, 0, 1};
            const char *p2 = txt.c_str() + lb + 1;
            char *end = nullptr;
            for (int k = 0; k < 4; ++k) {
                q[k] = std::strtod(p2, &end);
                if (end == p2) break;
                p2 = end;
                while (*p2 == ',' || *p2 == ' ' || *p2 == '\n') ++p2;
            }
            orient.push_back(q);
            pos = rb;
        }
        if (orient.empty()) {
            std::fprintf(stderr, "%s: no orientation_xyzw entries\n",
                         poses_path.c_str());
            return 1;
        }

        // ---- conventions and FOV.
        //
        // Everything the homography needs beyond the quaternions themselves is
        // a convention, and until version 2 of this sidecar not one of them was
        // written down: the encoder simply assumed the set in
        // docs/WARP.md 2.1 and assumed 95x95 degrees of FOV.  The assumptions
        // happened to be right for `gen_synthetic.py` at its defaults and are
        // silently wrong for anything else -- `--hfov 110` measured 18.70 dB
        // against the 31.01 dB the correct FOV gives on the same frame pair
        // (docs/WARP-AUDIT.md section 5).  A wrong convention does not crash
        // and does not produce an illegal stream; it produces a worse picture,
        // which is indistinguishable from a codec that is merely bad.
        //
        // So: a version 2 sidecar states its conventions and this encoder
        // refuses one it does not implement, rather than guessing.  A version 1
        // sidecar (or a hand-written one) still works and still assumes, but
        // says so out loud.
        const std::string cid = json_string(txt, "\"id\"");
        if (!cid.empty() && cid != "nxv-openxr-1") {
            std::fprintf(stderr,
                         "%s: pose convention \"%s\" is not implemented by this "
                         "encoder (it implements \"nxv-openxr-1\", "
                         "docs/WARP.md 2.1).\nRefusing rather than deriving a "
                         "homography from a convention it does not know.\n",
                         poses_path.c_str(), cid.c_str());
            return 1;
        }
        double sh = 0.0, sv = 0.0;
        const bool have_fov = json_fov_deg(txt, &sh, &sv);
        if (have_fov && !fov_from_cli) {
            fov_h = sh;
            fov_v = sv;
        }
        if (!quiet) {
            std::printf("poses: %zu orientations from %s\n", orient.size(),
                        poses_path.c_str());
            if (fov_from_cli)
                std::printf("poses: fov %.4g,%.4g deg from --fov%s\n", fov_h,
                            fov_v,
                            have_fov ? " (overriding the sidecar)" : "");
            else if (have_fov)
                std::printf("poses: fov %.4g,%.4g deg from the sidecar\n",
                            fov_h, fov_v);
            else
                std::printf("poses: no fov in the sidecar, ASSUMING %.4g,%.4g "
                            "deg; if that is wrong the warp is wrong and "
                            "nothing else will say so\n",
                            fov_h, fov_v);
            if (cid.empty())
                std::printf("poses: no convention block (version 1 sidecar), "
                            "assuming \"nxv-openxr-1\" (docs/WARP.md 2.1)\n");
        }
    }

    nxvc_config cfg;
    BaseRefresh brefresh;
    std::FILE *fatlas = nullptr;
    std::vector<uint8_t> atlas_buf;
    if (!atlas_dump.empty()) {
        fatlas = std::fopen(atlas_dump.c_str(), "wb");
        if (!fatlas) { std::perror("open --atlas-dump"); return 1; }
    }
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)(W / eyes);
    cfg.height = (uint32_t)H;
    cfg.eyes = (uint32_t)eyes;
    cfg.inter = (uint32_t)inter;
    cfg.stereo = (uint32_t)stereo;
    cfg.intra_period = (uint32_t)(intra_period > 0 ? intra_period : 1);
    cfg.drift_refresh = (uint32_t)drift_refresh;
    cfg.drift_gate_q8 = (uint32_t)(drift_gate > 0 ? drift_gate : 0);
    cfg.preset = (uint32_t)preset;
    cfg.chroma_weight_q8 = (uint32_t)chroma_weight;
    cfg.near_skip = (uint32_t)near_skip;
    cfg.quad_mv = (uint32_t)quad_mv;
    cfg.atlas = (uint32_t)atlas;
    cfg.atlas_nbr = (uint32_t)atlas_nbr;
    cfg.atlas_skip_margin =
        (uint32_t)(atlas_skip_margin > 0 ? atlas_skip_margin : 0);
    cfg.row_present = (uint32_t)row_present;
    cfg.atlas_gen_max = (uint32_t)(atlas_gen_max > 0 ? atlas_gen_max : 0);
    cfg.atlas_picture_disp =
        (uint32_t)(atlas_picture_disp > 0 ? atlas_picture_disp : 0);
    cfg.atlas_picture_min_spacing =
        (uint32_t)(atlas_picture_spacing > 0 ? atlas_picture_spacing : 0);
    cfg.atlas_picture_period =
        (uint32_t)(atlas_picture_period > 0 ? atlas_picture_period : 0);
    cfg.atlas_coarse_disp = (uint32_t)(coarse_disp > 0 ? coarse_disp : 0);
    cfg.atlas_sched_bytes = (uint32_t)(sched_bytes > 0 ? sched_bytes : 0);
    // 13.12.3: a STATIC_MV entry is held unwarped, so a head-locked tile may
    // be skipped.  On by default with the atlas -- it is the one behavioural
    // change to an existing mode and it is a strict gain.
    cfg.atlas_static_skip = (uint32_t)atlas;
    cfg.ref_sel = (uint32_t)(ref_sel < 0 ? 0 : (ref_sel > 2 ? 2 : ref_sel));
    cfg.mv_range = (uint32_t)(mv_range > 0 ? mv_range : 16);
    cfg.threads = (uint32_t)(threads > 0 ? threads : 0);
    cfg.skip_thresh = (uint32_t)(skip_thresh > 0 ? skip_thresh : 0);
    cfg.inter_int_decision = (uint32_t)int_decision;
    cfg.int_rdoq = (uint32_t)int_rdoq;
    cfg.int_lambda_q8 = (uint32_t)(int_lambda > 0 ? int_lambda : 0);
    cfg.int_coded_vectors = (uint32_t)int_coded_vectors;
    cfg.int_intra_mad_q8 = (uint32_t)(int_intra_mad > 0 ? int_intra_mad : 0);
    cfg.mode_lambda_q8 = (uint32_t)(mode_lambda > 0 ? mode_lambda : 0);
    cfg.chroma = pix == "yuv444p" ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
    cfg.base_qp = (uint32_t)(qp < 0 ? 0 : (qp > 63 ? 63 : qp));
    cfg.quant_matrix = (uint32_t)matrix;
    cfg.rdo = (uint32_t)rdo;
    cfg.rdo_lambda_q8 = (uint32_t)rdo_lambda_q8;
    cfg.rdoq_effort = (uint32_t)(rdoq_effort > 0 ? rdoq_effort : 0);
    cfg.me_effort = (uint32_t)(me_effort > 0 ? me_effort : 0);
    cfg.lambda_class_off = (uint32_t)lambda_class_off;
    for (int i = 0; i < 4; ++i)
        cfg.lambda_class_q8[i] = (uint32_t)(lambda_class[i] > 0 ? lambda_class[i] : 0);
    cfg.qp_search_step = (uint32_t)(qp_step > 0 ? qp_step : 0);
    cfg.dc_lambda_q8 = (uint32_t)(dc_lambda > 0 ? dc_lambda : 0);
    cfg.dc_rdoq_off = (uint32_t)dc_off;
    cfg.qp_search = (uint32_t)qp_search;
    cfg.wm_id = (uint32_t)wm;
    cfg.intra_dir = (uint32_t)intra_dir;
    cfg.intra_dir_layer = (uint32_t)intra_dir_layer;
    cfg.intra_dir_cand = (uint32_t)dir_cand;
    cfg.xform_size = (uint32_t)xform;
    cfg.ctx_v2 = (uint32_t)ctx_v2;
    cfg.ctx_v3 = (uint32_t)ctx_v3;
    cfg.tab_v2 = (uint32_t)tab_v2;
    // 0 means off and is a value the caller can legitimately ask for, so the
    // CLI passes the "was it given" flag rather than laundering 0 into a
    // sentinel.
    if (table_iters >= 0) {
        cfg.table_iters = (uint32_t)table_iters;
        cfg.table_iters_set = 1;
    }
    cfg.sign_hide = (uint32_t)sign_hide;
    cfg.split4x4 = (uint32_t)split4x4;
    cfg.chroma_from_luma = (uint32_t)cfl;
    cfg.entropy_lite = (uint32_t)entropy_lite;
    cfg.chroma_qp_off = chroma_qp_off;
    cfg.lossless = (uint32_t)lossless;
    cfg.transform_skip = (uint32_t)tskip;
    cfg.nsub_log2 = (uint32_t)nsub;
    cfg.tile_chroma420 = (uint32_t)tile420;
    cfg.custom_tables = (uint32_t)custom_tables;
    cfg.color_transform = rgb ? NXVC_CT_YCOCGR : NXVC_CT_NONE;
    cfg.collect_stats = (uint32_t)stats;
    cfg.color_space = rgb ? (uint32_t)NXVC_CS_RGB : (uint32_t)color_space;

    if (rc_on) {
        // The allocator writes an ABSOLUTE per-tile QP and a tile header
        // carries `qp_delta` in [-32, 31], so the frame's base_qp has to sit
        // in the middle of the range for every value 0..63 to be reachable.
        cfg.base_qp = nxrc::EncDriver::kBaseQp;
        // `wm_id == 0` in a tile header means "the frame's matrix"
        // (SYNTAX.md 6.5), so the frame matrix must be the flat one for
        // nxrc's WM_FLAT..WM_CHROMA to address built-in matrices 0..3
        // directly.  A non-zero cfg.wm_id is also what declares the WM_ID
        // tool bit; the per-tile map then overrides the value.
        cfg.quant_matrix = nxrc::EncDriver::kFrameMatrix;
        cfg.wm_id = WM_LUMA_ID;
        cfg.qp_search = 0;   // the allocator owns the QP, not a local search
        if (lossless) {
            std::fprintf(stderr, "--rc and --lossless are exclusive\n");
            return 2;
        }
    }

    nxvc_status st;
    nxvc_encoder *enc = nxvc_encoder_create(&cfg, &st);
    if (!enc) {
        std::fprintf(stderr, "encoder create failed: %s\n", nxvc_status_string(st));
        return 1;
    }

    nxvc_tile_layout tl;
    nxvc_tile_layout_get_ex(cfg.width, cfg.height, cfg.eyes, &tl);
    if (!atlas_base_path.empty()) {
        if (!atlas || eyes != 1) {
            std::fprintf(stderr,
                         "--atlas-base needs --atlas on and a one-eye stream\n");
            return 1;
        }
        if (!brefresh.open(atlas_base_path, cfg.width, cfg.height,
                           (uint32_t)(atlas_base_margin > 0 ? atlas_base_margin
                                                            : 0),
                           tl.tile_count)) {
            std::perror("open --atlas-base");
            return 1;
        }
    }
    const size_t cw = cfg.chroma == NXVC_CHROMA_444 ? (size_t)W : (size_t)((W + 1) / 2);
    const size_t chh = cfg.chroma == NXVC_CHROMA_444 ? (size_t)H : (size_t)((H + 1) / 2);
    const size_t ysz = (size_t)W * H, csz = cw * chh;

    std::FILE *fi = std::fopen(in.c_str(), "rb");
    if (!fi) { std::perror("open input"); return 1; }
    std::FILE *fo = std::fopen(out.c_str(), "wb");
    if (!fo) { std::perror("open output"); return 1; }
    std::FILE *fr = nullptr, *fq = nullptr, *fs = nullptr;
    if (!resmap_path.empty()) {
        fr = std::fopen(resmap_path.c_str(), "rb");
        if (!fr) { std::perror("open res map"); return 1; }
    }
    if (!qpmap_path.empty()) {
        fq = std::fopen(qpmap_path.c_str(), "rb");
        if (!fq) { std::perror("open qp map"); return 1; }
    }
    if (!skipmap_path.empty()) {
        fs = std::fopen(skipmap_path.c_str(), "rb");
        if (!fs) { std::perror("open skip map"); return 1; }
    }

    std::vector<uint8_t> hdr(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(enc, hdr.data(), hdr.size(), &hl);
    if (st != NXVC_OK) { std::fprintf(stderr, "header: %s\n", nxvc_status_string(st)); return 1; }
    std::fwrite(hdr.data(), 1, hl, fo);

    // The rate-control driver: everything per-tile the encoder is told in
    // --rc mode comes out of this object (docs/RATECONTROL.md, PAPER.md 4.6).
    std::unique_ptr<nxrc::EncDriver> drv;
    std::FILE *fm = nullptr;
    if (rc_on) {
        nxrc::EncDriveConfig dc;
        dc.width = (int)cfg.width;
        dc.height = (int)cfg.height;
        dc.eyes = eyes;
        dc.fps = (float)rc_fps;
        dc.bitrate_mbps = (float)rc_bitrate;
        dc.foveation = rc_fov != 0;
        dc.temporal = rc_temporal != 0;
        dc.gaze_valid = gaze_valid;
        dc.gaze_x = (float)gaze_x;
        dc.gaze_y = (float)gaze_y;
        dc.fov_h_deg = (float)rc_fov_h;
        dc.fov_v_deg = (float)rc_fov_v;
        dc.panel_px_per_eye = rc_panel;
        dc.act_strength = (float)rc_act;
        drv = std::make_unique<nxrc::EncDriver>(dc);
        if (drv->tile_count() != tl.tile_count) {
            std::fprintf(stderr, "rc: tile count %zu != encoder's %u\n",
                         drv->tile_count(), tl.tile_count);
            return 1;
        }
        if (!rc_map_path.empty()) {
            fm = std::fopen(rc_map_path.c_str(), "wb");
            if (!fm) { std::perror("open rc map"); return 1; }
            std::fprintf(fm, "frame,tile,eye,col,row,ecc_deg,fov_level,class,"
                             "qp,res_level,wm_id,force_skip,coded,bits,"
                             "pressure,gate\n");
        }
        if (!quiet)
            std::printf("rc: %.4g Mbit/s at %.4g fps, foveation %s, temporal "
                        "%s, panel %d px/eye over %.4g deg\n",
                        rc_bitrate, rc_fps, rc_fov ? "on" : "off",
                        rc_temporal ? "on" : "off", rc_panel, rc_fov_h);
    }

    std::vector<uint8_t> Y(ysz), U(csz), V(csz);
    // Per-tile coarse debt: 1 while a tile's atlas entry holds upsampled
    // res_level > 0 pixels that no later frame has refined yet.
    std::vector<uint8_t> coarse_debt(tl.tile_count, 0);
    std::vector<uint8_t> stale_map(tl.tile_count, 0);
    // Coded SAMPLES, not coded tiles: a res_level 1 tile codes a quarter of
    // them, and Pass B on the decoder is proportional to this rather than to
    // the tile count.
    uint64_t coded_samples = 0, coded_tiles_full = 0, coded_tiles_coarse = 0;
    uint64_t c_landed = 0, c_refined = 0, c_relanded_dirty = 0, c_coded_full = 0;
    std::vector<uint8_t> rmap(tl.tile_count), qmap(tl.tile_count),
        smap(tl.tile_count);
    std::vector<uint8_t> outbuf(ysz * 4 + csz * 8 + (1u << 20));
    size_t total = hl;
    int n = 0;
    while (frames < 0 || n < frames) {
        if (!read_exact(fi, Y.data(), ysz)) break;
        if (!read_exact(fi, U.data(), csz) || !read_exact(fi, V.data(), csz)) {
            std::fprintf(stderr, "short frame %d\n", n);
            break;
        }
        if (inter) {
            // Both eyes are rendered with the same head orientation; they
            // differ by the IPD translation, which a rotation-only warp does
            // not use (PAPER 2.2).
            nxvc_view views[2];
            const size_t idx = orient.empty() ? 0
                                              : (size_t)n < orient.size()
                                                    ? (size_t)n
                                                    : orient.size() - 1;
            for (int k = 0; k < eyes; ++k) {
                nxvc_view v{};
                if (!orient.empty()) {
                    v.qx = orient[idx][0];
                    v.qy = orient[idx][1];
                    v.qz = orient[idx][2];
                    v.qw = orient[idx][3];
                } else {
                    v.qw = 1.0;
                }
                const double hx = fov_h * 3.14159265358979323846 / 360.0;
                const double hy = fov_v * 3.14159265358979323846 / 360.0;
                v.fov_left = -hx;
                v.fov_right = hx;
                v.fov_up = hy;
                v.fov_down = -hy;
                views[k] = v;
            }
            nxvc_encoder_set_views(enc, views, (uint32_t)eyes);
        }
        const uint8_t *rm = nullptr, *qm = nullptr;
        if (fr && read_exact(fr, rmap.data(), rmap.size())) rm = rmap.data();
        // TWO-LEVEL REFRESH: the whole policy, as a res_map.
        //   a tile with no coarse debt   -> res_level R  (land it cheap)
        //   a tile carrying coarse debt  -> res_level 0  (refine it)
        // Whether either actually happens is the encoder's RD decision: a
        // tile only lands if it is being refreshed at all, and a coarse tile
        // only refines if its upsampled pixels are a bad enough predictor to
        // be worth coding.  The budget caps how many may refine in one frame.
        if (coarse_level > 0) {
            int spent = 0;
            for (uint32_t t = 0; t < (uint32_t)tl.tile_count; ++t) {
                if (coarse_debt[t]) {
                    if (coarse_budget == 0 || spent < coarse_budget) {
                        rmap[t] = 0;
                        ++spent;
                    } else {
                        rmap[t] = (uint8_t)coarse_level;   // wait its turn
                    }
                } else {
                    rmap[t] = (uint8_t)coarse_level;
                }
            }
            rm = rmap.data();
        }
        if (fq && read_exact(fq, qmap.data(), qmap.size())) qm = qmap.data();
        if (fs && read_exact(fs, smap.data(), smap.size()))
            nxvc_encoder_set_skip_map(enc, smap.data(), (uint32_t)smap.size());
        if (drv) {
            double hs = 0.0;
            if (orient.size() > 1 && n > 0) {
                const size_t i1 = (size_t)n < orient.size() ? (size_t)n
                                                           : orient.size() - 1;
                hs = head_speed_deg_s(orient[i1 - 1], orient[i1], rc_fps);
            }
            drv->set_head_speed((float)hs);
            drv->analyse(Y.data(), W, n);
            qm = drv->qp_map().data();
            rm = drv->res_map().data();
            st = nxvc_encoder_set_wm_map(enc, drv->wm_map().data(),
                                         tl.tile_count);
            if (st != NXVC_OK) {
                std::fprintf(stderr, "rc: set_wm_map: %s\n",
                             nxvc_status_string(st));
                return 1;
            }
            if (rc_temporal)
                nxvc_encoder_set_skip_map(enc, drv->skip_map().data(),
                                          tl.tile_count);
        }
        nxvc_image img{};
        img.plane[0] = Y.data(); img.stride[0] = W;
        img.plane[1] = U.data(); img.stride[1] = (int)cw;
        img.plane[2] = V.data(); img.stride[2] = (int)cw;
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(enc, &img, qm, rm, outbuf.data(),
                                       outbuf.size(), &ol);
        if (st != NXVC_OK) {
            std::fprintf(stderr, "encode frame %d: %s\n", n, nxvc_status_string(st));
            return 1;
        }
        std::fwrite(outbuf.data(), 1, ol, fo);
        total += ol;
        if (coarse_disp > 0) {
            uint32_t tc2 = 0;
            const nxvc_tile_info *ti2 = nxvc_encoder_tiles(enc, &tc2);
            for (uint32_t t = 0; t < tc2 && t < (uint32_t)tl.tile_count; ++t) {
                if (ti2[t].skipped) continue;
                const int side = 64 >> ti2[t].res_level;
                coded_samples += (uint64_t)side * side;
                if (ti2[t].res_level > 0) ++coded_tiles_coarse;
                else ++coded_tiles_full;
            }
        }
        // TWO-LEVEL accounting, read back from what the encoder actually did.
        if (coarse_level > 0) {
            uint32_t tc2 = 0;
            const nxvc_tile_info *ti2 = nxvc_encoder_tiles(enc, &tc2);
            for (uint32_t t = 0; t < tc2 && t < (uint32_t)tl.tile_count; ++t) {
                if (ti2[t].skipped) continue;          // untouched this frame
                if (ti2[t].res_level > 0) {
                    // A coarse landing.  If the tile already carried debt, its
                    // previous coarse pixels are being REPLACED before anyone
                    // refined them: those bytes bought nothing that survived.
                    if (coarse_debt[t]) ++c_relanded_dirty;
                    coarse_debt[t] = 1;
                    ++c_landed;
                } else {
                    if (coarse_debt[t]) ++c_refined;   // the refinement
                    else ++c_coded_full;
                    coarse_debt[t] = 0;
                }
            }
        }
        // Base-layer refresh, applied AFTER the frame and BEFORE the dump so
        // the dump covers the patched atlas -- which is what the decoder will
        // dump too, and therefore what proves the two agree.
        if (brefresh.f &&
            !brefresh.step(
                (uint32_t)n,
                [&](uint32_t m, uint8_t *o, uint32_t c, uint32_t *ns) {
                    return nxvc_encoder_atlas_stale_tiles(enc, m, o, c, ns);
                },
                [&](const nxvc_base_patch *pp, uint32_t *ap, uint32_t *su) {
                    return nxvc_encoder_atlas_patch_base(enc, pp, ap, su);
                })) {
            std::fprintf(stderr, "base refresh failed at frame %d\n", n);
            return 1;
        }
        // The NORMATIVE output under the atlas: dump the per-tile table so a
        // test can compare it against the decoder's, byte for byte (13.12.1).
        if (fatlas) {
            const size_t nb = nxvc_encoder_atlas_table_size(enc);
            if (nb) {
                atlas_buf.resize(nb);
                if (nxvc_encoder_atlas_table(enc, atlas_buf.data(), nb) ==
                    NXVC_OK)
                    std::fwrite(atlas_buf.data(), 1, nb, fatlas);
                uint8_t dg[32];
                atlas_digest(atlas_plane_shim, enc, dg);
                std::fwrite(dg, 1, sizeof(dg), fatlas);
                if (std::getenv("NXV_ATLAS_TILEDIG")) {
                    uint32_t w=0,h=0,stq=0;
                    const uint16_t *d1 = atlas_plane_shim(enc, 1, &w, &h, &stq);
                    for (uint32_t ty2 = 0; ty2 * 32 < h; ++ty2)
                      for (uint32_t tx2 = 0; tx2 * 32 < w; ++tx2) {
                        uint64_t hh = 1469598103934665603ull;
                        for (uint32_t y = 0; y < 32; ++y)
                          for (uint32_t x = 0; x < 32; ++x) {
                            uint32_t gy = ty2*32+y, gx = tx2*32+x;
                            uint16_t v = (gy<h&&gx<w)? d1[(size_t)gy*stq+gx] : 0;
                            hh = (hh ^ (v & 0xff)) * 1099511628211ull;
                            hh = (hh ^ (v >> 8)) * 1099511628211ull;
                          }
                        uint8_t b[8];
                        for (int k=0;k<8;k++) b[k]=(uint8_t)(hh>>(8*k));
                        std::fwrite(b,1,8,fatlas);
                      }
                }
            }
        }
        if (drv) {
            uint32_t tc = 0;
            const nxvc_tile_info *ti = nxvc_encoder_tiles(enc, &tc);
            if (fm) {
                const auto &a = drv->alloc();
                const auto &fv = drv->fov();
                const auto &cl = drv->classes();
                const auto &sk = drv->skip_map();
                const int txe = drv->tiles_x();
                for (uint32_t t = 0; t < tc; ++t) {
                    const int row = (int)(t / (unsigned)(txe * eyes));
                    const int rem = (int)(t % (unsigned)(txe * eyes));
                    std::fprintf(fm,
                                 "%d,%u,%d,%d,%d,%.3f,%u,%u,%u,%u,%u,%u,%u,%u,"
                                 "%.4f,%.5f\n",
                                 n, t, rem / txe, rem % txe, row,
                                 (double)fv.ecc_deg[t], fv.level[t], cl[t],
                                 a.qp[t], a.res_level[t], a.wm_id[t], sk[t],
                                 ti[t].skipped ? 0u : 1u,
                                 (unsigned)(ti[t].payload_len * 8),
                                 (double)a.pressure, (double)drv->stats().gate);
                }
            }
            drv->feedback(ti, tc);
        }
        if (!quiet) {
            std::printf("frame %d: %zu bytes  %.4f bpp\n", n, ol,
                        ol * 8.0 / ((double)W * H));
            if (drv) {
                const auto &ds = drv->stats();
                std::printf("  rc: budget %.0f, predicted %.0f, actual %.0f "
                            "bits; P %.2f, gate %.3f, forced skips %d, "
                            "encoder skips %d, head %.1f deg/s\n",
                            (double)ds.budget_bits, (double)ds.predicted_bits,
                            (double)ds.actual_bits, (double)ds.pressure,
                            (double)ds.gate, ds.forced_skips, ds.actual_skips,
                            (double)ds.head_speed_deg_s);
            }
        }
        if (stats) {
            nxvc_encode_stats st2;
            nxvc_encoder_stats(enc, &st2);
            double px = (double)W * H;
            auto row = [&](const char *name, double bytes) {
                std::printf("  %-18s %9.0f B  %6.2f%%  %.5f bpp\n", name, bytes,
                            100.0 * bytes / (double)st2.bytes_total,
                            bytes * 8.0 / px);
            };
            std::printf("bit breakdown, frame %d (%llu tiles, %.2f lanes/tile, "
                        "%llu transform-skip)\n", n,
                        (unsigned long long)st2.tiles,
                        st2.tiles ? (double)st2.lanes_total / (double)st2.tiles : 0.0,
                        (unsigned long long)st2.tiles_tskip);
            row("frame header", (double)st2.bytes_frame_header);
            row("prob tables", (double)st2.bytes_tables);
            row("tile-row headers", (double)st2.bytes_row_headers);
            row("tile headers", (double)st2.bytes_tile_headers);
            row("rANS init/flush", (double)st2.bytes_rans_init);
            row("  DC planes", st2.bits_dc_plane / 8.0);
            row("  luma blocks", st2.bits_luma_blocks / 8.0);
            row("  chroma blocks", st2.bits_chroma_blocks / 8.0);
            if (st2.bits_alpha_blocks)
                row("  alpha blocks", st2.bits_alpha_blocks / 8.0);
            row("payload total", (double)st2.bytes_payload);
            if (st2.bits_predicted_q10) {
                // What the encoder's rate model told the mode decision, the
                // trellis and the QP search this payload would cost, against
                // what it cost.  A gap here means every RD decision in the
                // frame was taken against the wrong number.
                double pred = (double)st2.bits_predicted_q10 / 1024.0 / 8.0;
                double act = (double)st2.bytes_payload -
                             (double)st2.bytes_rans_init;
                std::printf("  rate model: predicted %.0f B, coded %.0f B "
                            "(%+.2f %%)\n",
                            pred, act, act > 0 ? (pred - act) / act * 100.0 : 0.0);
            }
            // ADR-0029: what the atlas maintenance cost this frame, as counts.
            // A rebase (13.12.10) is 34 us a tile on the Pico 4 and a
            // base-sourced refresh is 1.9 us a tile, so these two numbers ARE
            // the GPU cost model -- there is nothing to estimate.
            if (st2.atlas_rebased || st2.tiles_base_refreshed ||
                st2.tiles_margin_forced)
                std::printf("  atlas: rebase %llu (%llu tiles), base refresh "
                            "%llu tiles, margin-forced %llu tiles\n",
                            (unsigned long long)st2.atlas_rebased,
                            (unsigned long long)st2.tiles_rebased,
                            (unsigned long long)st2.tiles_base_refreshed,
                            (unsigned long long)st2.tiles_margin_forced);
            std::printf("  res levels 0/1/2: %llu / %llu / %llu\n",
                        (unsigned long long)st2.tiles_res[0],
                        (unsigned long long)st2.tiles_res[1],
                        (unsigned long long)st2.tiles_res[2]);
        }
        ++n;
    }
    if (fatlas) std::fclose(fatlas);
    std::fclose(fo);
    std::fclose(fi);
    if (coarse_disp > 0 && !coarse_stats_path.empty()) {
        std::FILE *cs = std::fopen(coarse_stats_path.c_str(), "wb");
        if (cs) {
            const double fr = (double)(n > 1 ? n - 1 : 1);
            std::fprintf(cs,
                         "{\"coarse_disp\": %d, \"coded_samples\": %llu, "
                         "\"coded_samples_per_frame\": %.1f, "
                         "\"coded_full\": %llu, \"coded_coarse\": %llu}\n",
                         coarse_disp, (unsigned long long)coded_samples,
                         (double)coded_samples / fr,
                         (unsigned long long)coded_tiles_full,
                         (unsigned long long)coded_tiles_coarse);
            std::fclose(cs);
        }
    }
    if (coarse_level > 0 && !coarse_stats_path.empty()) {
        uint64_t outstanding = 0;
        for (uint32_t t = 0; t < (uint32_t)tl.tile_count; ++t)
            outstanding += coarse_debt[t] ? 1u : 0u;
        std::FILE *cs = std::fopen(coarse_stats_path.c_str(), "wb");
        if (cs) {
            std::fprintf(cs,
                         "{\"coarse_level\": %d, \"budget\": %d, "
                         "\"landed\": %llu, \"refined\": %llu, "
                         "\"relanded_before_refine\": %llu, "
                         "\"coded_full\": %llu, \"outstanding\": %llu}\n",
                         coarse_level, coarse_budget,
                         (unsigned long long)c_landed,
                         (unsigned long long)c_refined,
                         (unsigned long long)c_relanded_dirty,
                         (unsigned long long)c_coded_full,
                         (unsigned long long)outstanding);
            std::fclose(cs);
        }
    }
    if (fr) std::fclose(fr);
    if (fq) std::fclose(fq);
    if (fs) std::fclose(fs);
    if (fm) std::fclose(fm);
    if (!quiet)
        std::printf("%d frame(s), %zu bytes total, %.4f bpp mean\n", n, total,
                    n ? total * 8.0 / ((double)W * H * n) : 0.0);
    nxvc_encoder_destroy(enc);
    return n > 0 ? 0 : 1;
}
