// nxv-info: dump the headers of an .nxv stream.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"

static const char *mode_name(int m) {
    switch (m) {
        case NXVC_MODE_WARP_SKIP: return "WARP_SKIP";
        case NXVC_MODE_STATIC_MV: return "STATIC_MV";
        case NXVC_MODE_WARP_MV: return "WARP_MV";
        case NXVC_MODE_INTRA: return "INTRA";
        case NXVC_MODE_STEREO: return "STEREO";
    }
    return "?";
}

int main(int argc, char **argv) {
    static const char *kUsage =
        "usage: nxv-info --in file.nxv [--tiles] [--modes]\n"
        "  --tiles  one line per tile of every frame\n"
        "  --modes  a whole-stream histogram of the tile modes and of the\n"
        "           syntax v1.5 per-tile forms (SYNTAX.md 13.9 to 13.11)\n";
    std::string in;
    int tiles = 0, modes = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--in" && i + 1 < argc) in = argv[++i];
        else if (a == "--tiles") tiles = 1;
        else if (a == "--modes") modes = 1;
        else { std::fputs(kUsage, stderr); return 2; }
    }
    if (in.empty()) { std::fputs(kUsage, stderr); return 2; }
    // The histogram needs the tile records, which only a full decode produces.
    const int walk_tiles = tiles || modes;
    unsigned long hist[5] = {}, n_skipped = 0, n_near = 0, n_near_ac = 0,
                  n_quad = 0, n_sub = 0, n_tiles = 0, bytes_payload = 0;

    std::FILE *f = std::fopen(in.c_str(), "rb");
    if (!f) { std::perror("open"); return 1; }
    std::fseek(f, 0, SEEK_END);
    long fsz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d((size_t)(fsz > 0 ? fsz : 0));
    if (d.empty() || std::fread(d.data(), 1, d.size(), f) != d.size()) {
        std::fprintf(stderr, "read failed\n");
        return 1;
    }
    std::fclose(f);

    nxvc_status st;
    nxvc_decoder *dec = nxvc_decoder_create(&st);
    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(dec, d.data(), d.size(), &consumed);
    if (st != NXVC_OK) {
        std::fprintf(stderr, "stream header: %s\n", nxvc_status_string(st));
        return 1;
    }
    off = consumed;
    nxvc_stream_info si;
    nxvc_decoder_stream_info(dec, &si);
    nxvc_tile_layout tl;
    nxvc_tile_layout_get_ex(si.width, si.height, si.eyes, &tl);
    std::printf("stream header (%zu bytes)\n", consumed);
    std::printf("  magic         0x%08x\n", si.magic);
    std::printf("  version       %u\n", si.version);
    std::printf("  profile/level %u/%u\n", si.profile, si.level);
    std::printf("  tile_size     %s\n", (si.tile_size & 1) ? "32x32" : "64x64");
    std::printf("  size          %ux%u  eyes %u  bitdepth %u\n", si.width,
                si.height, si.eyes, si.bit_depth);
    std::printf("  chroma        %s\n", si.chroma ? "4:4:4" : "4:2:0");
    static const char *cs[4] = {"unspecified", "YCbCr BT.709 limited",
                                "YCbCr BT.709 full", "RGB"};
    std::printf("  color xform   %s\n", si.color_transform ? "YCoCg-R" : "none");
    std::printf("  color space   %s\n", cs[si.color_space & 3]);
    std::printf("  alpha         %u\n", si.alpha);
    std::printf("  layers        %u\n", si.num_layers);
    std::printf("  tools         0x%016llx\n", (unsigned long long)si.tools);
    std::printf("  ext_len       %u (%u TLVs, %u unknown)\n", si.ext_len,
                si.ext_tlv_count, si.ext_unknown_count);
    std::printf("  tile grid     %ux%u per eye, cols %u = %u tiles\n",
                tl.tiles_x, tl.tiles_y, tl.tiles_x * si.eyes, tl.tile_count);

    int n = 0;
    while (off < d.size()) {
        nxvc_frame_info fi;
        st = nxvc_decoder_scan_frame(dec, d.data() + off, d.size() - off, &fi,
                                     &consumed);
        if (st != NXVC_OK) {
            std::fprintf(stderr, "frame %d: %s\n", n, nxvc_status_string(st));
            return 1;
        }
        std::printf("frame %d @%zu: num %u  bytes %u  qp %u  cqpo %d  aqpo %d  "
                    "matrix %u  tables 0x%02x  refs 0x%02x  flags 0x%02x\n",
                    n, off, fi.frame_number, fi.frame_bytes, fi.base_qp,
                    fi.chroma_qp_off, fi.alpha_qp_off, fi.quant_matrix,
                    fi.tables_present, fi.ref_slots, fi.flags);
        std::printf("  pose:");
        for (int i = 0; i < 26; ++i) std::printf(" %02x", fi.pose[i]);
        std::printf("\n");
        if (fi.warp_present) {
            for (uint32_t eye = 0; eye < si.eyes; ++eye) {
                std::printf("  warp_ext eye %u  Q10.21 [%d %d %d / %d %d %d]  "
                            "Q2.29 [%d %d %d]\n", eye,
                            fi.warp[eye][0], fi.warp[eye][1], fi.warp[eye][2],
                            fi.warp[eye][3], fi.warp[eye][4], fi.warp[eye][5],
                            fi.warp[eye][6], fi.warp[eye][7], fi.warp[eye][8]);
            }
        }
        if (walk_tiles) {
            // A full decode is needed to walk the tile headers.
            uint32_t yw, yh, cw, ch;
            nxvc_decoder_plane_size(dec, 0, &yw, &yh);
            nxvc_decoder_plane_size(dec, 1, &cw, &ch);
            std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch),
                V((size_t)cw * ch), A((size_t)yw * yh);
            nxvc_image img{};
            img.plane[0] = Y.data(); img.stride[0] = (int)yw;
            img.plane[1] = U.data(); img.stride[1] = (int)cw;
            img.plane[2] = V.data(); img.stride[2] = (int)cw;
            img.plane[3] = A.data(); img.stride[3] = (int)yw;
            size_t c2;
            if (nxvc_decoder_decode_frame(dec, d.data() + off, d.size() - off,
                                          &img, &c2) == NXVC_OK) {
                uint32_t count = 0;
                const nxvc_tile_info *ti = nxvc_decoder_tiles(dec, &count);
                for (uint32_t i = 0; i < count; ++i) {
                    const nxvc_tile_info &t = ti[i];
                    ++n_tiles;
                    if (t.mode < 5) ++hist[t.mode];
                    n_skipped += t.skipped;
                    n_near += t.near_skip;
                    n_near_ac += t.near_skip_ac;
                    n_quad += t.quad_mv;
                    n_sub += t.sub_intra;
                    bytes_payload += t.payload_len;
                    if (!tiles) continue;
                    char vec[32] = "";
                    if (t.mode == NXVC_MODE_STEREO)
                        std::snprintf(vec, sizeof vec, " d%u", t.disparity);
                    else if (t.mv_present || t.skipped)
                        std::snprintf(vec, sizeof vec, " mv%+d,%+d ref%u",
                                      t.mv_x, t.mv_y, t.ref_sel);
                    std::printf("  tile %4u e%u %-9s res%u %s qp%2u dq%+3d ts%u "
                                "a%u wm%u tab%u ns%u  %5u B%s%s\n",
                                i, t.eye, mode_name(t.mode), t.res_level,
                                t.chroma444 ? "444" : "420", t.qp, t.qp_delta,
                                t.tskip, t.alpha_mode, t.wm_id, t.table_set,
                                t.nsub_log2, t.payload_len, vec,
                                t.concealed ? " CONCEALED" : "");
                }
            }
        }
        off += consumed;
        ++n;
    }
    if (modes && n_tiles) {
        const double pc = 100.0 / (double)n_tiles;
        std::printf("tile modes over %lu tiles\n", n_tiles);
        for (int m = 0; m < 5; ++m)
            std::printf("  %-10s %8lu  %5.1f %%\n", mode_name(m), hist[m],
                        hist[m] * pc);
        std::printf("  of which skip_bitmap %lu (%.1f %%)\n", n_skipped,
                    n_skipped * pc);
        std::printf("syntax v1.5 forms\n");
        std::printf("  near_skip  %8lu  %5.1f %%  (%lu with the ramps)\n",
                    n_near, n_near * pc, n_near_ac);
        std::printf("  quad_mv    %8lu  %5.1f %%\n", n_quad, n_quad * pc);
        std::printf("  sub_intra  %8lu  %5.1f %%\n", n_sub, n_sub * pc);
        std::printf("  payload    %lu bytes over all tiles\n", bytes_payload);
    }
    std::printf("%d frame(s)\n", n);
    nxvc_decoder_destroy(dec);
    return 0;
}
