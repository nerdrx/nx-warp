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
        "  --rdo-lambda F       RD lambda scale (default 0.30)\n"
        "  --qp-search N        try per-tile qp_delta in [-N, +N] (default 0)\n"
        "  --intra-dir on|off|layer  directional intra (tool 17); `layer`\n"
        "                       predicts the DC-plane residual instead\n"
        "  --intra-dir-cand N   modes RD-checked per block (default 2)\n"
        "  --ctx v1|v2          12 or 16 entropy contexts (tool 21)\n"
        "  --no-sign-hide       code every sign (default: hide one per unit)\n"
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
        "                       (default 180; 1 = every tile every frame)\n"
        "  --ref-sel 0..2       reference distance inter tiles ask for\n"
        "  --stereo on|off      STEREO inter-view mode on the right eye\n"
        "  --mv-range N         coarse search radius in samples (default 16)\n"
        "  --skip-thresh F      WARP_SKIP early-out gate, multiples of the\n"
        "                       quantiser noise floor qstep^2/12 (default 1)\n"
        "  --skip-map FILE      per-tile force_warp_skip flags, tile_count\n"
        "                       bytes per frame (docs/RATECONTROL.md 8.7).\n"
        "                       Applied after the mode search; the encoder\n"
        "                       overrides it where a coded tile is required\n"
        "  --mode-lambda F      lambda scale of the per-tile mode decision,\n"
        "                       relative to the trellis (default 0.25)\n"
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
    return 2.0 * std::acos(dot) * 180.0 / 3.14159265358979323846 * fps;
}

static bool read_exact(std::FILE *f, void *p, size_t n) {
    return std::fread(p, 1, n, f) == n;
}

int main(int argc, char **argv) {
    std::string in, out, pix = "yuv420p", resmap_path, qpmap_path;
    int W = 0, H = 0, qp = 24, frames = -1, matrix = 1, chroma_qp_off = 0;
    int lossless = 0, tile420 = 0, custom_tables = 1, rgb = 0, quiet = 0;
    int tskip = 0, nsub = 255, stats = 0;  // nsub 255 = auto lane count
    int color_space = 0;
    int rdo = 1, rdo_lambda_q8 = 0, qp_search = 0, wm = 0;
    // These mirror nxvc_config_default(): the v2 intra tools are on.
    int intra_dir = 1, intra_dir_layer = 0, ctx_v2 = 1, dir_cand = 0;
    int sign_hide = 1;
    int inter = 0, eyes = 1, intra_period = 180, ref_sel = 0, stereo = 0;
    int mv_range = 16, skip_thresh = 0, mode_lambda = 0;
    double fov_h = 95.0, fov_v = 95.0;
    bool fov_from_cli = false;
    std::string poses_path, skipmap_path;
    int rc_on = 0, rc_fov = 1, rc_temporal = 1, rc_panel = 2160;
    double rc_bitrate = 40.0, rc_fps = 90.0;
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
        else if (a == "--ref-sel") ref_sel = std::atoi(val());
        else if (a == "--mv-range") mv_range = std::atoi(val());
        else if (a == "--skip-thresh")
            skip_thresh = (int)(std::atof(val()) * 256.0 + 0.5);
        else if (a == "--rc") rc_on = 1;
        else if (a == "--rc-bitrate") rc_bitrate = std::atof(val());
        else if (a == "--rc-panel") rc_panel = std::atoi(val());
        else if (a == "--rc-map") rc_map_path = val();
        else if (a == "--rc-fps") rc_fps = std::atof(val());
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
        else if (a == "--intra-dir") {
            std::string v = val();
            if (v == "on") { intra_dir = 1; intra_dir_layer = 0; }
            else if (v == "layer") { intra_dir = 1; intra_dir_layer = 1; }
            else if (v == "off") intra_dir = 0;
            else { std::fprintf(stderr, "--intra-dir: on|off|layer\n"); return 2; }
        }
        else if (a == "--intra-dir-cand") dir_cand = std::atoi(val());
        else if (a == "--sign-hide") sign_hide = 1;
        else if (a == "--no-sign-hide") sign_hide = 0;
        else if (a == "--ctx") {
            std::string v = val();
            if (v == "v2") ctx_v2 = 1;
            else if (v == "v1") ctx_v2 = 0;
            else { std::fprintf(stderr, "--ctx: v1|v2\n"); return 2; }
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
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)(W / eyes);
    cfg.height = (uint32_t)H;
    cfg.eyes = (uint32_t)eyes;
    cfg.inter = (uint32_t)inter;
    cfg.stereo = (uint32_t)stereo;
    cfg.intra_period = (uint32_t)(intra_period > 0 ? intra_period : 1);
    cfg.ref_sel = (uint32_t)(ref_sel < 0 ? 0 : (ref_sel > 2 ? 2 : ref_sel));
    cfg.mv_range = (uint32_t)(mv_range > 0 ? mv_range : 16);
    cfg.skip_thresh = (uint32_t)(skip_thresh > 0 ? skip_thresh : 0);
    cfg.mode_lambda_q8 = (uint32_t)(mode_lambda > 0 ? mode_lambda : 0);
    cfg.chroma = pix == "yuv444p" ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
    cfg.base_qp = (uint32_t)(qp < 0 ? 0 : (qp > 63 ? 63 : qp));
    cfg.quant_matrix = (uint32_t)matrix;
    cfg.rdo = (uint32_t)rdo;
    cfg.rdo_lambda_q8 = (uint32_t)rdo_lambda_q8;
    cfg.qp_search = (uint32_t)qp_search;
    cfg.wm_id = (uint32_t)wm;
    cfg.intra_dir = (uint32_t)intra_dir;
    cfg.intra_dir_layer = (uint32_t)intra_dir_layer;
    cfg.intra_dir_cand = (uint32_t)dir_cand;
    cfg.ctx_v2 = (uint32_t)ctx_v2;
    cfg.sign_hide = (uint32_t)sign_hide;
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
            std::printf("  res levels 0/1/2: %llu / %llu / %llu\n",
                        (unsigned long long)st2.tiles_res[0],
                        (unsigned long long)st2.tiles_res[1],
                        (unsigned long long)st2.tiles_res[2]);
        }
        ++n;
    }
    std::fclose(fo);
    std::fclose(fi);
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
