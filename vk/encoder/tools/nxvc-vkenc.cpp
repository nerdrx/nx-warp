/* nxvc-vkenc -- the NX Warp GPU encoder's host harness.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mirrors the flags of `ref/tools/nxv-enc` that this pipeline implements, so
 * the two can be pointed at the same input and their output compared byte for
 * byte.  That comparison is the acid test of docs/PAPER.md 3.9: a frame the
 * GPU encoded must be the frame the reference encoder would have produced for
 * the same decisions, and must therefore decode with `nxv-dec` to exactly the
 * same pixels.
 *
 *   nxvc-vkenc --in f.yuv --w W --h H --pix yuv420p --qp N --out f.nxv
 *
 * The pipeline it drives is intra-only: E3 `forward.comp`, E4
 * `rans_encode.comp`, E5 `packetize.comp`, with E0's colour conversion stood
 * in for on the host because the input is a file rather than a compositor
 * image.  Inter tools, res levels, alpha and custom probability tables are out
 * of scope and are refused rather than silently ignored.
 *
 *   --cpu          run the CPU models instead of the GPU (always available)
 *   --device N     physical device index
 *   --check        also run the CPU models and diff every intermediate
 *   --bench N      time the passes over N iterations
 */

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nxe_host.h"
#include "nxe_vk.h"

namespace nxe {
int selftest(int device, bool cpu_only, bool print_digests, bool quiet);
int selftest_dump(const char *prefix);
int inter_fixture_dump(const char *prefix, int w, int h, int frames);
int ring_check(const char *prefix, const char *decoded, int w, int h, int frames);
}

static void usage() {
    std::fprintf(stderr,
        "usage: nxvc-vkenc --in file.yuv --w W --h H --pix yuv444p|yuv420p\n"
        "                  --qp N --out out.nxv\n"
        "optional:\n"
        "  --frames N           encode at most N frames\n"
        "  --eyes 1|2           2 = side-by-side stereo, --w is the full width\n"
        "  --matrix 0..3        frame weighting matrix (default 1)\n"
        "  --wm 0..3            per-tile weighting matrix id (default 0)\n"
        "  --nsub 0..5          rANS lane count log2 (default 3 = 8 lanes)\n"
        "  --tskip off|on       transform skip (default off)\n"
        "  --ctx v1|v2|v3       12, 16 or 27 entropy contexts (tools 21,\n"
        "                       25); default v2, and v3 implies v2\n"
        "  --custom-tables      train the eight probability table sets on\n"
        "                       the frame and transmit those that pay (6)\n"
        "  --tab v1|v2          transmitted-table coding: flat 5-bit rows\n"
        "                       or the compact per-row-flag form (26);\n"
        "                       v2 needs --custom-tables\n"
        "  --entropy rans|lite-fixed  entropy tool: interleaved rANS\n"
        "                       (default) or ENTROPY_LITE / FIXED (30).\n"
        "                       `lite` is accepted for `lite-fixed`.  Lite\n"
        "                       forces --no-sign-hide, --no-custom-tables\n"
        "                       and --nsub 3, as nxv-enc does.  lite-rice is\n"
        "                       refused: Pass A implements only FIXED\n"
        "  --table-iters N      Lloyd iterations refining the trained\n"
        "                       sets (default 3, 0 = the v1.4 encoder)\n"
        "  --no-sign-hide       code every sign (default: hide one per unit)\n"
        "  --intra-dir on|off|layer   directional intra, modes from the host\n"
        "  --dir-mode-seed N    fill the per-block modes from a PRNG (test aid)\n"
        "  --ref-sel 0..2       reference distance an inter frame asks for\n"
        "                       first; a floor, the encoder walks outwards to\n"
        "                       the newest reference the client still holds\n"
        "  --modes              per-frame tile mode census and coded count\n"
        "  --display-psnr       PSNR-Y of the DISPLAYED picture vs the source\n"
        "                       (under --atlas, one warp from the atlas)\n"
        "  --atlas              the per-tile atlas reference, tool bit 31\n"
        "  --warp-mv            also offer WARP_MV, scored from Pass W's own\n"
        "                       prediction (implies --coded-vectors)\n"
        "  --drift-refresh      the INTRA cap is per-tile AGE (nxv-enc's\n"
        "                       default) rather than the staggered rule\n"
        "  --atlas-mode         the atlas as a per-frame MODE, tool bit 34\n"
        "                       ([SYN] 13.12.11): each frame is an ATLAS\n"
        "                       frame or a PICTURE frame.  Needs --atlas\n"
        "  --atlas-picture-d N  the mode trigger threshold D in luma\n"
        "                       samples (default 8, ADR-0029 sweep)\n"
        "  --atlas-dump P       write the encoder's shadow atlas after each\n"
        "                       frame to P: the 64-byte records of [SYN]\n"
        "                       13.12.1 followed by a 32-byte digest of the\n"
        "                       atlas planes, in the layout nxv-enc and\n"
        "                       nxv-dec use.  This is the NORMATIVE output\n"
        "                       under ATLAS; the stream is only how two\n"
        "                       implementations arrive at one\n"
        "                       ([SYN] 13.12).  Needs --inter; forces ref_sel 0\n"
        "  --tile-map P         write every frame's per-tile decision to P\n"
        "                       as CSV (frame,tile,row,col,eye,mode,picture)\n"
        "  --atlas-layout-selftest  check that a patch buffer built from\n"
        "                       nxvc_vk_encoder_atlas_layout() addresses the\n"
        "                       same samples the encoder's copies do, then exit\n"
        "  --row-present        elide the 12-byte header of a tile row with\n"
        "                       no coded tile and name the rows that are\n"
        "                       there in a bitmap after warp_ext(), tool\n"
        "                       bit 32 ([SYN] 3.1.2).  Orthogonal to --atlas\n"
        "  --atlas-disp-margin N  skip a tile only when the largest\n"
        "                       displacement over its four corners is under N\n"
        "                       luma samples (ADR-0029's cross-tile gather\n"
        "                       bound).  0 = off; 64 is the tile itself and\n"
        "                       so no bound; the useful range is 4 to 16.\n"
        "                       Costs forced refresh, reported at the end\n"
        "  --atlas-refresh-cap N  Cheat 3: code at most N refresh-driven\n"
        "                       tiles a frame, chosen by fovea distance plus\n"
        "                       age.  0 = off (every candidate is coded)\n"
        "  --motion-skip Q8     scale the skip threshold by head angular\n"
        "                       velocity (Cheats 5).  0, the default, is off\n"
        "  --hold-every N       simulate a client that reconstructs only\n"
        "                       every Nth frame and reports the rest not\n"
        "                       held.  0 = holds everything (the default)\n"
        "  --ack-delay K        that client also CONFIRMS the frames it did\n"
        "                       reconstruct, K frames later.  Off unless\n"
        "                       given, which is what leaves every fixture\n"
        "                       stream unchanged\n"
        "  --report-delay K     frames of latency on the NOT-held report\n"
        "                       (default 0, which no real link delivers)\n"
        "  --int-rdoq N         integer requantiser: 0 off (the default),\n"
        "                       1 drop a +-1 level that does not pay for\n"
        "                       itself.  The library's effort 1, and\n"
        "                       `nxv-enc --int-rdoq N`\n"
        "  --mv-range N         coarse integer search radius in samples\n"
        "                       (default 16); the library's effort 2 raises\n"
        "                       it, and it is `nxv-enc --mv-range N`\n"
        "  --qp-ladder L        per-tile QP offsets: a comma-separated\n"
        "                       ladder of qp_delta candidates the\n"
        "                       rate-distortion decision scores, e.g.\n"
        "                       \"-4,-2,0,2,4\".  Empty (the default) is\n"
        "                       the decision off.  0 is always a\n"
        "                       candidate and wins ties.  Measured a\n"
        "                       wash on rANS and about -2%% BD-rate on\n"
        "                       Lite, which is why it is not an effort\n"
        "                       level -- see vk/encoder/README.md\n"
        "  --qp-lambda K        override the decision's lambda constant\n"
        "                       (default 901 = ref kLambdaScale 0.22)\n"
        "  --qp-table-search    price each candidate under the best of\n"
        "                       the eight table sets; a diagnostic\n"
        "  --trellis N          effort 2: the rate-distortion trellis in\n"
        "                       exact integers (0 off, 1 on).  Matches\n"
        "                       `nxv-enc --int-trellis 1 --rdoq-effort 3`\n"
        "  --rate-check         measure the integer rate model of\n"
        "                       nxe_rate.h against the bytes the entropy\n"
        "                       coder actually produced, per tile, and\n"
        "                       print the distribution.  Changes no byte\n"
        "                       of the stream\n"
        "  --snap-identity N    snap a nearly-still warp to the IDENTITY when\n"
        "                       every tile corner moves less than N/16 of a\n"
        "                       sample (0 = off).  Encoder-side, no syntax:\n"
        "                       every skipped tile is then a copy on the\n"
        "                       decoder instead of an integer warp\n"
        "  --chroma-qp-off N    chroma QP offset\n"
        "  --device N           Vulkan physical device index (default 0)\n"
        "  --cpu                run the CPU models, no Vulkan\n"
        "  --check              run both and diff every intermediate\n"
        "  --bench N            time each pass over N iterations\n"
        "  --list               list Vulkan devices and exit\n"
        "  --selftest           run the built-in configuration table:\n"
        "                       GPU against the CPU models, and the CPU\n"
        "                       models against pinned stream digests\n"
        "  --print-digests      with --selftest, print them instead\n"
        "  --quiet\n"
        "\n"
        "Exit code 77 means \"no usable Vulkan device\"; ctest reports it as a\n"
        "skip.  --cpu never returns it.\n");
}

int main(int argc, char **argv) {
    nxe::Config cfg;
    bool list = false, check = false, self = false, digests = false;
    const char *dump = nullptr;
    const char *dump_inter = nullptr;
    /* The inter fixture's picture size and length.  Defaults are the
     * 256x192x8 clip the acid test has always used; the 1088x1088 leg
     * overrides them, because 289 tiles is the headset's real tile count
     * and every other fixture in this tree is under 256 tiles. */
    int fx_w = 256, fx_h = 192, fx_frames = 8;
    const char *ring_prefix = nullptr, *ring_decoded = nullptr;
    /* Where to write the encoder's shadow atlas after each frame, so a test
     * can hold it against the atlas nxv-dec builds from this encoder's own
     * stream.  [SYN] 13.12 makes the atlas the normative output; the stream
     * agreeing byte for byte does not imply the two sides agree about the
     * reference the client now holds, and that is the divergence that shows up
     * as drift rather than as a broken frame. */
    const char *atlas_dump = nullptr;
    const char *tile_map = nullptr;
    bool atlas_layout_selftest = false;
    /* A client that keeps up with only one frame in `hold_every`.  It drives
     * nxvc_vk_encoder_set_frame_held()'s half of the reference walk from the
     * command line, which is what the 289-tile drop-pattern test needs and
     * what nothing else in this tool can express.  0 holds everything. */
    int hold_every = 0;
    /* Frames of latency on the POSITIVE report -- the confirmation that the
     * headset reconstructed a frame.  On a real link it is one feedback
     * period, which is a fraction of a frame; here it is whole frames, so 1 is
     * already pessimistic.  Negative means the client sends no confirmations
     * at all, which is every fixture and is what leaves their streams
     * untouched. */
    int ack_delay = -1;
    /* Frames of latency on the NEGATIVE report.  Zero -- the encoder is told
     * about a drop before it codes the next frame -- is the case no real link
     * delivers, and it is the case in which the chain-derived record is
     * already right.  A realistic value is one or two frames, and that is
     * where the difference between guessing and being told shows up. */
    int report_delay = 0;
    int ring_frames = 0;
    std::string pix = "yuv420p";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--in") cfg.in = val();
        else if (a == "--out") cfg.out = val();
        else if (a == "--w") cfg.w = std::atoi(val());
        else if (a == "--h") cfg.h = std::atoi(val());
        else if (a == "--pix") pix = val();
        else if (a == "--qp") cfg.qp = std::atoi(val());
        else if (a == "--inter") cfg.inter = true;
        else if (a == "--poses") cfg.poses = val();
        else if (a == "--coded-vectors") cfg.int_coded_vectors = true;
        else if (a == "--ref-sel") cfg.ref_sel = std::atoi(val());
        else if (a == "--atlas") cfg.atlas = true;
        else if (a == "--warp-mv") { cfg.int_coded_vectors = true; cfg.int_warp_mv = true; }
        else if (a == "--drift-refresh") cfg.drift_refresh = true;
        else if (a == "--atlas-mode") cfg.atlas_mode = true;
        else if (a == "--atlas-picture-d") cfg.atlas_picture_d = std::atoi(val());
        else if (a == "--row-present") cfg.row_present = true;
        else if (a == "--atlas-dump") atlas_dump = val();
        else if (a == "--tile-map") tile_map = val();
        else if (a == "--atlas-layout-selftest") atlas_layout_selftest = true;
        else if (a == "--atlas-disp-margin")
            cfg.atlas_disp_margin = std::atoi(val());
        else if (a == "--atlas-refresh-cap")
            cfg.atlas_refresh_cap = std::atoi(val());
        else if (a == "--modes") cfg.mode_census = true;
        else if (a == "--display-psnr") cfg.display_psnr = true;
        else if (a == "--motion-skip") cfg.motion_skip_gain_q8 = std::atoi(val());
        else if (a == "--int-rdoq") cfg.int_rdoq = std::atoi(val());
        else if (a == "--rate-check") cfg.rate_check = true;
        else if (a == "--trellis") cfg.trellis = std::atoi(val());
        else if (a == "--qp-ladder") cfg.qp_ladder = val();
        else if (a == "--qp-lambda") cfg.qp_lambda_q12 = std::atoi(val());
        else if (a == "--qp-table-search") cfg.qp_table_search = true;
        else if (a == "--mv-range") cfg.mv_range = std::atoi(val());
        else if (a == "--snap-identity") cfg.snap_identity = std::atoi(val());
        else if (a == "--hold-every") hold_every = std::atoi(val());
        else if (a == "--ack-delay") {
            ack_delay = std::atoi(val());
            /* A client that confirms is one whose confirmations may be
             * required from the first frame. */
            cfg.ref_confirm = ack_delay >= 0;
        }
        else if (a == "--report-delay") report_delay = std::atoi(val());
        else if (a == "--intra-period") cfg.intra_period = std::atoi(val());
        else if (a == "--skip-thresh")
            cfg.skip_thresh = (int)(std::atof(val()) * 256.0 + 0.5);
        else if (a == "--frames") cfg.frames = std::atoi(val());
        else if (a == "--eyes") cfg.eyes = std::atoi(val());
        else if (a == "--matrix") cfg.matrix = std::atoi(val());
        else if (a == "--wm") cfg.wm_id = std::atoi(val());
        else if (a == "--nsub") cfg.nsub_log2 = std::atoi(val());
        else if (a == "--chroma-qp-off") cfg.chroma_qp_off = std::atoi(val());
        else if (a == "--tskip") cfg.tskip = std::strcmp(val(), "on") == 0 ? 1 : 0;
        else if (a == "--ctx") {
            const char *v = val();
            /* v3 is a refinement of v2 and the stream header refuses bit 25
             * without bit 21, so v3 sets both. */
            cfg.ctx_v3 = std::strcmp(v, "v3") == 0;
            cfg.ctx_v2 = cfg.ctx_v3 || std::strcmp(v, "v2") == 0;
        }
        else if (a == "--entropy") {
            const char *v = val();
            /* Same spelling and same numbering as `nxv-enc --entropy`: the
             * two tools are driven from one description by the acid test and
             * a divergence here would be a divergence in the test. */
            if (std::strcmp(v, "lite-fixed") == 0 ||
                std::strcmp(v, "lite") == 0)
                cfg.entropy_lite = 1;
            else if (std::strcmp(v, "rans") == 0) cfg.entropy_lite = 0;
            else {
                std::fprintf(stderr, "--entropy: rans|lite-fixed\n");
                return 2;
            }
        }
        else if (a == "--custom-tables") cfg.custom_tables = true;
        else if (a == "--no-custom-tables") cfg.custom_tables = false;
        else if (a == "--tab" && i + 1 < argc) {
            const char *v = argv[++i];
            cfg.tab_v2 = std::strcmp(v, "v2") == 0;
        } else if (a == "--table-iters" && i + 1 < argc)
            cfg.table_iters = std::atoi(argv[++i]);
        else if (a == "--no-sign-hide") cfg.sign_hide = false;
        else if (a == "--sign-hide") cfg.sign_hide = true;
        else if (a == "--intra-dir") {
            std::string v = val();
            if (v == "on") { cfg.intra_dir = true; cfg.dir_layer = false; }
            else if (v == "layer") { cfg.intra_dir = true; cfg.dir_layer = true; }
            else cfg.intra_dir = false;
        }
        else if (a == "--dir-mode-seed") cfg.dir_mode_seed = (uint32_t)std::strtoul(val(), nullptr, 0);
        else if (a == "--device") cfg.device = std::atoi(val());
        else if (a == "--cpu") cfg.cpu_only = true;
        else if (a == "--check") check = true;
        else if (a == "--bench") { cfg.bench = true; cfg.bench_iters = std::atoi(val()); }
        else if (a == "--list") list = true;
        else if (a == "--selftest") self = true;
        else if (a == "--print-digests") digests = true;
        else if (a == "--dump-selftest-yuv") dump = val();
        else if (a == "--dump-inter") dump_inter = val();
        else if (a == "--dump-inter-size") {
            fx_w = std::atoi(val());
            fx_h = std::atoi(val());
            fx_frames = std::atoi(val());
        }
        else if (a == "--check-ring") ring_prefix = val();
        else if (a == "--check-ring-decoded") ring_decoded = val();
        else if (a == "--check-ring-frames") ring_frames = std::atoi(val());
        else if (a == "--quiet") cfg.quiet = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }

    if (list) return nxe::vk_list_devices();
    if (dump) return nxe::selftest_dump(dump);
    if (dump_inter)
        return nxe::inter_fixture_dump(dump_inter, fx_w, fx_h, fx_frames);
    if (ring_prefix && ring_decoded)
        return nxe::ring_check(ring_prefix, ring_decoded, cfg.w, cfg.h,
                               ring_frames);
    if (self)
        return nxe::selftest(cfg.device, cfg.cpu_only, digests, cfg.quiet);

    if (cfg.in.empty() || cfg.out.empty() || cfg.w <= 0 || cfg.h <= 0) {
        usage();
        return 2;
    }
    if (pix != "yuv420p" && pix != "yuv444p") {
        std::fprintf(stderr, "--pix must be yuv420p or yuv444p\n");
        return 2;
    }
    cfg.chroma444 = (pix == "yuv444p");
    if (cfg.eyes != 1 && cfg.eyes != 2) {
        std::fprintf(stderr, "--eyes must be 1 or 2\n");
        return 2;
    }
    if (cfg.qp < 0 || cfg.qp > 63) { std::fprintf(stderr, "--qp 0..63\n"); return 2; }
    if (cfg.nsub_log2 < 0 || cfg.nsub_log2 > 5) {
        std::fprintf(stderr, "--nsub 0..5\n");
        return 2;
    }

    nxe::Frame f;
    nxe::setup(cfg, f);
    nxe::build_tables(cfg, f);

    std::FILE *fi = std::fopen(cfg.in.c_str(), "rb");
    if (!fi) { std::perror("open input"); return 1; }
    std::FILE *fo = std::fopen(cfg.out.c_str(), "wb");
    if (!fo) { std::perror("open output"); return 1; }
    std::FILE *fat = nullptr;
    if (atlas_dump) {
        if (!cfg.atlas) {
            std::fprintf(stderr,
                         "nxvc-vkenc: --atlas-dump needs --atlas; there is no "
                         "atlas to dump without it\n");
            std::fclose(fi);
            std::fclose(fo);
            return 2;
        }
        fat = std::fopen(atlas_dump, "wb");
        if (!fat) { std::perror("open --atlas-dump"); return 1; }
    }
    std::FILE *ftm = nullptr;
    if (tile_map) {
        ftm = std::fopen(tile_map, "w");
        if (!ftm) { std::perror("open --tile-map"); return 1; }
        /* `mode` is the nxvw value E1c settled on: 0 WARP_SKIP, 1 STATIC_MV,
         * 2 WARP_MV, 3 INTRA.  `picture` is 1 on a frame [SYN] 13.12.11 coded
         * as a PICTURE frame, which is a property of the FRAME and repeated on
         * every one of its rows so a reader needs no join. */
        std::fprintf(ftm, "frame,tile,row,col,eye,mode,picture\n");
    }

    std::vector<uint8_t> hdr = nxe::stream_header(cfg, f);
    std::fwrite(hdr.data(), 1, hdr.size(), fo);

    /* The pose sidecar, scraped rather than parsed -- exactly as nxv-enc does
     * it, and deliberately so: the two encoders have to read one file the same
     * way or a byte-identity test compares two different warps.  The only keys
     * that matter are `orientation_xyzw` and the FOV, and a version 2 sidecar
     * that names a convention this encoder does not implement is REFUSED
     * rather than guessed at, because a wrong convention does not crash and
     * does not make an illegal stream: it makes a worse picture, which looks
     * exactly like a codec that is merely bad. */
    std::vector<std::array<double, 4>> poses;
    double fov_h = 95.0, fov_v = 95.0;
    if (!cfg.poses.empty()) {
        std::FILE *pf = std::fopen(cfg.poses.c_str(), "rb");
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
            poses.push_back(q);
            pos = rb;
        }
        if (poses.empty()) {
            std::fprintf(stderr, "%s: no orientation_xyzw entries\n",
                         cfg.poses.c_str());
            return 1;
        }
        const size_t cid = txt.find("\"id\"");
        if (cid != std::string::npos) {
            const size_t q0 = txt.find('"', txt.find(':', cid));
            const size_t q1 = txt.find('"', q0 + 1);
            const std::string id = txt.substr(q0 + 1, q1 - q0 - 1);
            if (id != "nxv-openxr-1") {
                std::fprintf(stderr,
                             "%s: pose convention \"%s\" is not implemented "
                             "(this encoder implements \"nxv-openxr-1\", "
                             "docs/WARP.md 2.1)\n",
                             cfg.poses.c_str(), id.c_str());
                return 1;
            }
        }
        const size_t fd = txt.find("\"fov_deg\"");
        if (fd != std::string::npos) {
            const size_t hh = txt.find("\"h\"", fd), vv = txt.find("\"v\"", fd);
            if (hh != std::string::npos)
                fov_h = std::strtod(txt.c_str() + txt.find(':', hh) + 1, nullptr);
            if (vv != std::string::npos)
                fov_v = std::strtod(txt.c_str() + txt.find(':', vv) + 1, nullptr);
        }
        if (!cfg.quiet)
            std::printf("poses: %zu orientations, fov %.4g,%.4g deg\n",
                        poses.size(), fov_h, fov_v);
    }

    nxe::VkEncoder gpu;
    if (!cfg.cpu_only) {
        std::string err;
        if (!gpu.create(cfg, f, err)) {
            std::fprintf(stderr, "nxvc-vkenc: %s\n", err.c_str());
            std::fclose(fi);
            std::fclose(fo);
            if (fat) std::fclose(fat);
    if (ftm) std::fclose(ftm);
            std::remove(cfg.out.c_str());
            return 77;
        }
        /* The accessor's contract, checked against the copies themselves
         * before anything is encoded: a caller building a patch buffer from
         * nxvc_vk_encoder_atlas_layout() must land on the samples the
         * encoder's own region builder addresses. */
        if (atlas_layout_selftest) {
            if (!cfg.atlas) {
                std::fprintf(stderr, "nxvc-vkenc: --atlas-layout-selftest "
                                     "needs --atlas\n");
                return 2;
            }
            std::string lerr;
            if (!gpu.atlas_layout_roundtrip(lerr)) {
                std::fprintf(stderr,
                             "nxvc-vkenc: atlas layout round-trip FAILED: %s\n",
                             lerr.c_str());
                return 1;
            }
            std::fclose(fi);
            std::fclose(fo);
            if (fat) std::fclose(fat);
    if (ftm) std::fclose(ftm);
            std::remove(cfg.out.c_str());
            return 0;
        }
    }

    size_t total = hdr.size();
    double psnr_sum = 0;
    int psnr_n = 0;
    int n = 0;
    int rc = 0;
    while (cfg.frames < 0 || n < cfg.frames) {
        if (!nxe::read_frame(fi, cfg, f)) break;
        nxe::fill_modes(cfg, f, (uint32_t)n);
        if (!poses.empty()) {
            /* Both eyes get the same orientation: the sidecar is a head pose,
             * and the per-eye difference is a translation the homography does
             * not carry (docs/WARP.md 2.1). */
            const std::array<double, 4> &q =
                poses[(size_t)n < poses.size() ? (size_t)n : poses.size() - 1];
            const double hr = fov_h * 3.14159265358979323846 / 360.0;
            const double vr = fov_v * 3.14159265358979323846 / 360.0;
            nxe::View v[2];
            for (int e = 0; e < 2; ++e) {
                v[e].qx = q[0]; v[e].qy = q[1]; v[e].qz = q[2]; v[e].qw = q[3];
                v[e].fov_left = -hr; v[e].fov_right = hr;
                v[e].fov_up = vr; v[e].fov_down = -vr;
            }
            gpu.set_views(v, cfg.eyes, (uint32_t)n);
        }
        if (cfg.cpu_only) {
            nxe::encode_frame_cpu(f, (uint32_t)n);
        } else if (!gpu.encode_frame(f, (uint32_t)n, check, cfg.quiet)) {
            std::fprintf(stderr, "nxvc-vkenc: GPU encode failed on frame %d\n", n);
            rc = 1;
            break;
        }
        /* The client's verdict on the frame just coded, delivered before the
         * next encode.  A real link delivers it a round trip later; this is
         * the zero-latency case, which is the one that isolates the reference
         * walk from the transport's timing. */
        if (hold_every > 1 && cfg.inter) {
            const int reportable = n - report_delay;
            if (reportable >= 0 && (reportable % hold_every) != 0)
                gpu.set_frame_held((uint32_t)reportable, false);
        }
        /* And the positive one, `ack_delay` frames behind.  A frame the
         * simulated client reconstructed is one it can predict from, and
         * saying so is what lets the encoder reference it. */
        if (ack_delay >= 0 && cfg.inter) {
            const int ackable = n - ack_delay;
            if (ackable >= 0 &&
                (hold_every <= 1 || (ackable % hold_every) == 0))
                gpu.set_frame_held((uint32_t)ackable, true);
        }
        /* NXE_DUMP_RING=<path> writes the ring slot this frame just wrote,
         * luma only, as raw uint16.  It is what the ring-vs-decoder test
         * compares; the encoder is otherwise the only thing that can see it. */
        if (const char *rp = std::getenv("NXE_DUMP_RING")) {
            /* `nsamp`, not `n`: `n` is the frame counter of the loop this
             * sits in, and shadowing it wrote every frame to one file named
             * after the sample count and read slot (count & 3). */
            const size_t nsamp = (size_t)((cfg.w + 1) & ~1) * (size_t)cfg.h;
            std::vector<uint16_t> ring(nsamp, 0);
            if (gpu.read_ring_luma((uint32_t)(n & 3), ring.data(), nsamp)) {
                char path[512];
                std::snprintf(path, sizeof path, "%s.%d", rp, n);
                if (std::FILE *rf = std::fopen(path, "wb")) {
                    std::fwrite(ring.data(), 2, nsamp, rf);
                    std::fclose(rf);
                }
            }
        }
        std::fwrite(f.out.data(), 1, f.out.size(), fo);
        total += f.out.size();
        if (!cfg.quiet)
            std::printf("frame %d: %zu bytes  %.4f bpp\n", n, f.out.size(),
                        f.out.size() * 8.0 / ((double)cfg.w * cfg.h));
        /* The per-frame mode census.  Under ATLAS the CODED count -- every
         * mode but WARP_SKIP -- is the quantity the whole model is about: it
         * is what the decoder pays for, what the atlas write-back touches, and
         * the term that does not amortise across display intervals.  It is
         * printed rather than derived from the stream because the stream does
         * not carry a mode histogram and reconstructing one means parsing. */
        if (ftm && cfg.inter) {
            const uint32_t cols = f.fp.tiles_x * f.fp.eyes;
            const int pic = (!cfg.cpu_only && gpu.last_picture_frame()) ? 1 : 0;
            for (uint32_t t = 0; t < f.fp.ntiles; ++t) {
                const uint32_t row = t / cols;
                const uint32_t rem = t % cols;
                const uint32_t eye = rem / f.fp.tiles_x;
                const uint32_t col = rem % f.fp.tiles_x;
                std::fprintf(ftm, "%d,%u,%u,%u,%u,%u,%d\n", n, t, row, col,
                             eye, f.jobs[t].mode, pic);
            }
        }
        if (cfg.mode_census && cfg.inter) {
            unsigned c[5] = {0, 0, 0, 0, 0};
            for (uint32_t t = 0; t < f.fp.ntiles; ++t) {
                const uint32_t m = f.jobs[t].mode;
                if (m < 5) ++c[m];
            }
            const unsigned coded = f.fp.ntiles - c[0];
            std::printf("  modes %u: skip %u  static %u  warp %u  intra %u  "
                        "coded %u (%.1f %%)\n",
                        n, c[0], c[1], c[2], c[3], coded,
                        100.0 * coded / (double)f.fp.ntiles);
            /* The same census through the public report, which is what a
             * caller of the ABI sees.  Printed beside the harness's own count
             * so the two are visibly the same numbers. */
            if (!cfg.cpu_only) {
                const nxvc_vke_frame_report &r = gpu.last_frame_report();
                static const char *kMode[] = {"non-atlas", "ATLAS", "PICTURE"};
                std::printf("  report %u: %s  skip %u static %u warp %u intra "
                            "%u  coded %u  assembled %u  disp %.2f px  "
                            "%u bytes\n",
                            r.frame_number,
                            kMode[r.mode < 3 ? r.mode : 0], r.skip,
                            r.static_mv, r.warp_mv, r.intra, r.coded,
                            r.assembled, r.worst_disp_q4 / 16.0, r.bytes);
            }
        }
        /* [SYN] 13.12.5.  The displayed picture, which under ATLAS is NOT the
         * normative object and is not what a conformance vector compares --
         * which is exactly why it is the thing to measure when pricing the
         * model against the picture-based one.  Comparing reconstructions
         * would compare an object the two models do not both have. */
        /* The normative output of 13.12, in the layout nxv-enc --atlas-dump
         * and nxv-dec --atlas-dump write: the whole per-tile table, then a
         * 32-byte digest of the atlas planes.  Written after the frame is
         * coded, which is after step 3's write-back, so it is the atlas as it
         * stands at the END of frame `n` -- the state the next frame's step 1
         * advances. */
        if (fat && !cfg.cpu_only) {
            std::vector<uint8_t> tab;
            uint8_t dg[32];
            if (gpu.atlas_table(tab) && gpu.atlas_pixel_digest(dg)) {
                std::fwrite(tab.data(), 1, tab.size(), fat);
                std::fwrite(dg, 1, sizeof(dg), fat);
            }
        }
        if (cfg.display_psnr && cfg.inter && !cfg.cpu_only) {
            std::vector<uint16_t> shown;
            if (gpu.read_displayed_luma((uint32_t)n, shown)) {
                const double p = nxe::luma_psnr_tilemajor(
                    shown.data(), f.src[0].data(), f.fp.ntiles, 255);
                psnr_sum += p;
                ++psnr_n;
                if (!cfg.quiet)
                    std::printf("  display %d: PSNR-Y %.4f dB\n", n, p);
            }
        }
        ++n;
    }
    std::fclose(fo);
    std::fclose(fi);
    if (fat) std::fclose(fat);
    if (ftm) std::fclose(ftm);

    if (psnr_n)
        std::printf("displayed PSNR-Y: %.4f dB mean over %d frame(s)\n",
                    psnr_sum / psnr_n, psnr_n);
    /* The displacement bound's price, stated whenever the bound is on: how
     * many tiles it refused to skip.  A rule whose cost is not reported is a
     * rule nobody can decide about. */
    if (cfg.atlas_disp_margin > 0 && !cfg.cpu_only && n > 0) {
        const unsigned long long forced = gpu.atlas_disp_forced();
        std::printf("disp-margin %d: forced refresh %llu tiles, %.2f per "
                    "frame (%.1f %% of %d)\n",
                    cfg.atlas_disp_margin, forced, (double)forced / n,
                    100.0 * (double)forced / ((double)n * f.fp.ntiles),
                    (int)f.fp.ntiles);
    }

    if (cfg.bench && !cfg.cpu_only && n > 0) gpu.bench(f, cfg.bench_iters);

    if (!cfg.quiet)
        std::printf("%d frame(s), %zu bytes total, %.4f bpp mean\n", n, total,
                    n ? total * 8.0 / ((double)cfg.w * cfg.h * n) : 0.0);
    /* --snap-identity: how often the threshold actually fired.  Printed
     * whatever the answer, including "on 0 of 8", because a tool that was
     * asked for and never triggered is the thing a sweep most needs to see. */
    if (cfg.inter && !cfg.cpu_only) {
        uint64_t idt = 0, idtot = 0;
        gpu.identity_stats(idt, idtot);
        if (idtot)
            std::printf("identity tiles: %llu of %llu (%.1f %%) over the inter "
                        "frames\n",
                        (unsigned long long)idt, (unsigned long long)idtot,
                        100.0 * (double)idt / (double)idtot);
    }
    if (cfg.snap_identity > 0 && !cfg.cpu_only) {
        uint32_t sf = 0, sa = 0;
        gpu.snap_stats(sf, sa);
        std::printf("snap-identity %d/16 sample: snapped %u of %u inter frame(s)"
                    " (%.1f %%)\n",
                    cfg.snap_identity, sa, sf,
                    sf ? 100.0 * (double)sa / (double)sf : 0.0);
        double lo = 0, mean = 0, hi = 0;
        gpu.warp_offset_stats(lo, mean, hi);
        if (sf)
            std::printf("  warp corner displacement: min %.1f, mean %.1f, "
                        "max %.1f /16 sample\n", lo, mean, hi);
    }
    /* --rate-check: the integer rate model of nxe_rate.h against the bytes the
     * entropy coder produced, over every tile of the run.  The mean signed
     * error is the model's bias and the mean absolute error is its spread; the
     * first is the number a rate-distortion decision cares about, because a
     * constant bias cancels between two candidates and a spread does not. */
    if (cfg.rate_check && f.rc_tiles) {
        const double nz = (double)(f.rc_tiles - f.rc_tiny);
        std::printf("rate model: %llu tiles (%llu too small to score), "
                    "estimate %.0f bits vs coded %llu bits, total %+.3f%%\n",
                    (unsigned long long)f.rc_tiles,
                    (unsigned long long)f.rc_tiny,
                    (double)f.rc_est_q10 / 1024.0,
                    (unsigned long long)f.rc_real_bits,
                    100.0 * ((double)f.rc_est_q10 / 1024.0 -
                             (double)f.rc_real_bits) /
                            (double)f.rc_real_bits);
        if (nz > 0)
            std::printf("rate model: per tile mean %+.3f%%, mean |err| %.3f%%, "
                        "range %+.3f%% .. %+.3f%%\n",
                        f.rc_err_sum / nz, f.rc_err_abs_sum / nz,
                        f.rc_err_min, f.rc_err_max);
    }
    if (rc) return rc;
    return n > 0 ? 0 : 1;
}
