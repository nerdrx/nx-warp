// tile_walk.cpp -- open a .nxv and look at what the encoder actually decided.
//
//   Prints the stream header, then for each frame a per-tile table of
//   mode / QP / res_level / transform-skip / table set / payload bytes, plus
//   histograms and a compact ASCII bitrate map of the tile grid.
//
// Run:
//   nxvc-example-tilewalk --in out.nxv [--frame N] [--no-table] [--csv t.csv]
//
// This is the tool you open first when a stream looks wrong.  "The picture is
// soft in the corners" is a res_level map question.  "The bitrate spiked" is a
// per-tile bytes question.  "Why is this frame 3x the size of the last one" is
// a mode histogram question.  All three are one run of this program.
//
// How the layout query works, and why it needs a full decode:
//
//   * nxvc_tile_layout_get(w, h, &tl) is pure arithmetic -- it needs no stream
//     at all, just the picture size, and gives tiles_x/tiles_y/tile_count.
//     That is the grid every qp_map and res_map is indexed in, raster order.
//   * nxvc_decoder_scan_frame() parses only the frame header.  Cheap, and it
//     gives frame_number, base_qp and frame_bytes -- but NOT the tile records.
//   * nxvc_decoder_tiles() returns the records of the most recently DECODED
//     frame.  The per-tile fields live in the tile headers interleaved with the
//     payloads, so getting them means walking the frame; the reference API is
//     honest about that rather than pretending there is a free index.
//
// So: scan for the cheap fields, decode for the tile table.  This example does
// both, which is also a small consistency check between the two paths.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <nxvc/nxvc.h>

namespace {

const char* mode_name(unsigned m) {
    switch (m) {
        case NXVC_MODE_WARP_SKIP: return "WARP_SKIP";
        case NXVC_MODE_STATIC_MV: return "STATIC_MV";
        case NXVC_MODE_WARP_MV:   return "WARP_MV";
        case NXVC_MODE_INTRA:     return "INTRA";
        case NXVC_MODE_STEREO:    return "STEREO";
        default:                  return "?";
    }
}

// One character per tile, log-ish scale in payload bytes.  Reading a 68x34 grid
// as a picture beats reading 2312 numbers.
char density_char(uint32_t bytes, uint32_t p95) {
    if (bytes == 0) return '.';
    static const char ramp[] = " .:-=+*#%@";
    const double t = p95 ? double(bytes) / double(p95) : 0.0;
    int i = int(t * 8.0) + 1;
    if (i > 9) i = 9;
    if (i < 1) i = 1;
    return ramp[i];
}

std::vector<uint8_t> read_all(const char* path) {
    std::vector<uint8_t> v;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return v; }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::rewind(f);
    if (n > 0) {
        v.resize(size_t(n));
        if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    }
    std::fclose(f);
    return v;
}

void usage() {
    std::fprintf(stderr,
                 "usage: nxvc-example-tilewalk --in FILE.nxv [--frame N] "
                 "[--no-table] [--csv FILE]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string in_path, csv_path;
    long want_frame = -1;  // -1 = every frame
    bool table = true;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        auto take = [&](const char* name) { return a == name && v && ++i; };
        if (take("--in")) in_path = v;
        else if (take("--csv")) csv_path = v;
        else if (take("--frame")) want_frame = std::strtol(v, nullptr, 10);
        else if (a == "--no-table") table = false;
        else { usage(); return 2; }
    }
    if (in_path.empty()) { usage(); return 2; }

    std::vector<uint8_t> data = read_all(in_path.c_str());
    if (data.empty()) return 1;

    nxvc_status st{};
    nxvc_decoder* dec = nxvc_decoder_create(&st);
    if (!dec) {
        std::fprintf(stderr, "decoder_create: %s\n", nxvc_status_string(st));
        return 1;
    }

    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(dec, data.data(), data.size(), &consumed);
    if (st != NXVC_OK) {
        std::fprintf(stderr, "stream header: %s\n", nxvc_status_string(st));
        nxvc_decoder_destroy(dec);
        return 1;
    }
    off += consumed;

    nxvc_stream_info si;
    nxvc_decoder_stream_info(dec, &si);

    // Pure arithmetic: no stream needed, and it must agree with what the
    // decoder reports below.
    nxvc_tile_layout tl;
    nxvc_tile_layout_get(si.width, si.height, &tl);

    std::printf("stream   %s\n", in_path.c_str());
    std::printf("  size          %ux%u, %s%s, %u-bit, %u eye(s), %u layer(s)\n",
                si.width, si.height,
                si.chroma == NXVC_CHROMA_444 ? "4:4:4" : "4:2:0",
                si.alpha ? " + alpha" : "", si.bit_depth, si.eyes, si.num_layers);
    std::printf("  profile/level %u/%u, version %u, ext %u byte(s) in %u TLV(s)"
                " (%u unknown)\n",
                si.profile, si.level, si.version, si.ext_len, si.ext_tlv_count,
                si.ext_unknown_count);
    std::printf("  tile grid     %u x %u = %u tiles of %ux%u\n", tl.tiles_x,
                tl.tiles_y, tl.tile_count, tl.tile_size, tl.tile_size);
    std::printf("  tools         0x%016llx\n", (unsigned long long)si.tools);
    {
        // Naming the set bits turns "0x2f" into a sentence.
        static const struct { unsigned long long bit; const char* name; } kNames[] = {
            {NXVC_TOOL_INTRA_DC_PLANE, "intra_dc_plane"},
            {NXVC_TOOL_TRANSFORM_SKIP, "transform_skip"},
            {NXVC_TOOL_RES_LEVEL, "res_level"},
            {NXVC_TOOL_CHROMA444, "chroma444"},
            {NXVC_TOOL_ALPHA, "alpha"},
            {NXVC_TOOL_LOSSLESS, "lossless"},
            {NXVC_TOOL_CUSTOM_TABLES, "custom_tables"},
            {NXVC_TOOL_NSUB_VAR, "nsub_var"},
            {NXVC_TOOL_PER_TILE_CHROMA, "per_tile_chroma"},
            {NXVC_TOOL_YCOCGR, "ycocgr"},
            {NXVC_TOOL_INTER, "inter"},
            {NXVC_TOOL_WARP, "warp"},
            {NXVC_TOOL_STEREO, "stereo"},
            {NXVC_TOOL_LAYERS, "layers"},
        };
        std::printf("                ");
        bool any = false;
        for (const auto& e : kNames)
            if (si.tools & e.bit) { std::printf("%s%s", any ? " " : "", e.name); any = true; }
        std::printf("%s\n", any ? "" : "(none)");
    }

    // Output planes for the decode.  We throw the pixels away; we are here for
    // the tile records.
    const int nplanes = si.alpha ? 4 : 3;
    std::vector<std::vector<uint8_t>> planes(nplanes);
    nxvc_image img{};
    for (int p = 0; p < nplanes; p++) {
        uint32_t pw = 0, ph = 0;
        nxvc_decoder_plane_size(dec, p, &pw, &ph);
        planes[p].resize(size_t(pw) * ph);
        img.plane[p] = planes[p].data();
        img.stride[p] = int32_t(pw);
    }

    std::FILE* fcsv = nullptr;
    if (!csv_path.empty()) {
        fcsv = std::fopen(csv_path.c_str(), "w");
        if (!fcsv) { std::perror(csv_path.c_str()); return 1; }
        std::fprintf(fcsv,
                     "frame,tile,tx,ty,mode,qp,res_level,tskip,table_set,"
                     "nsub_log2,chroma444,alpha_mode,mv_x,mv_y,bytes\n");
    }

    long fno = 0;
    int rc = 0;
    while (off < data.size()) {
        // Cheap pass first: header only.
        nxvc_frame_info fi{};
        size_t scanned = 0;
        st = nxvc_decoder_scan_frame(dec, data.data() + off, data.size() - off, &fi,
                                     &scanned);
        if (st != NXVC_OK) {
            std::fprintf(stderr, "frame %ld header at %zu: %s\n", fno, off,
                         nxvc_status_string(st));
            rc = 1;
            break;
        }

        if (want_frame >= 0 && fno != want_frame) {
            off += scanned;
            fno++;
            continue;
        }

        // Full decode, so nxvc_decoder_tiles() has something to return.
        st = nxvc_decoder_decode_frame(dec, data.data() + off, data.size() - off,
                                       &img, &consumed);
        if (st != NXVC_OK) {
            std::fprintf(stderr, "frame %ld at %zu: %s\n", fno, off,
                         nxvc_status_string(st));
            rc = 1;
            break;
        }
        if (consumed != scanned) {
            // scan_frame and decode_frame disagreeing about a frame's length is
            // a bitstream bug worth shouting about, not a rounding difference.
            std::fprintf(stderr,
                         "frame %ld: scan says %zu bytes, decode consumed %zu\n",
                         fno, scanned, consumed);
            rc = 1;
        }

        uint32_t count = 0;
        const nxvc_tile_info* ti = nxvc_decoder_tiles(dec, &count);
        if (!ti) { std::fprintf(stderr, "no tile records\n"); rc = 1; break; }

        std::printf("\nframe %u  base_qp %u  chroma_qp_off %+d  %u bytes  "
                    "%u tiles  flags 0x%x\n",
                    fi.frame_number, fi.base_qp, fi.chroma_qp_off, fi.frame_bytes,
                    count, fi.flags);
        if (count != tl.tile_count)
            std::printf("  NOTE: %u tile records for a %u-tile grid\n", count,
                        tl.tile_count);

        if (table) {
            std::printf("  %6s %4s %4s %-10s %3s %3s %5s %5s %4s %8s\n", "tile",
                        "tx", "ty", "mode", "qp", "res", "tskip", "tabs", "nsub",
                        "bytes");
            for (uint32_t i = 0; i < count; i++) {
                const nxvc_tile_info& t = ti[i];
                std::printf("  %6u %4u %4u %-10s %3u %3u %5u %5u %4u %8u\n",
                            unsigned(t.tile_index),
                            tl.tiles_x ? unsigned(t.tile_index % tl.tiles_x) : 0u,
                            tl.tiles_x ? unsigned(t.tile_index / tl.tiles_x) : 0u,
                            mode_name(t.mode), unsigned(t.qp),
                            unsigned(t.res_level), unsigned(t.tskip),
                            unsigned(t.table_set), unsigned(t.nsub_log2),
                            unsigned(t.payload_len));
            }
        }

        if (fcsv)
            for (uint32_t i = 0; i < count; i++) {
                const nxvc_tile_info& t = ti[i];
                std::fprintf(fcsv, "%u,%u,%u,%u,%s,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u\n",
                             fi.frame_number, unsigned(t.tile_index),
                             tl.tiles_x ? unsigned(t.tile_index % tl.tiles_x) : 0u,
                             tl.tiles_x ? unsigned(t.tile_index / tl.tiles_x) : 0u,
                             mode_name(t.mode), unsigned(t.qp),
                             unsigned(t.res_level), unsigned(t.tskip),
                             unsigned(t.table_set), unsigned(t.nsub_log2),
                             unsigned(t.chroma444), unsigned(t.alpha_mode),
                             int(t.mv_x), int(t.mv_y), unsigned(t.payload_len));
            }

        // ------------------------------------------------------- aggregates
        uint32_t mode_hist[8] = {0}, res_hist[4] = {0}, tab_hist[8] = {0};
        uint32_t qp_min = 255, qp_max = 0, tskip_n = 0;
        uint64_t total = 0, biggest = 0;
        uint32_t biggest_i = 0;
        std::vector<uint32_t> sizes(count);
        for (uint32_t i = 0; i < count; i++) {
            const nxvc_tile_info& t = ti[i];
            if (t.mode < 8) mode_hist[t.mode]++;
            if (t.res_level < 4) res_hist[t.res_level]++;
            if (t.table_set < 8) tab_hist[t.table_set]++;
            qp_min = std::min<uint32_t>(qp_min, t.qp);
            qp_max = std::max<uint32_t>(qp_max, t.qp);
            tskip_n += t.tskip ? 1u : 0u;
            total += t.payload_len;
            sizes[i] = t.payload_len;
            if (t.payload_len > biggest) { biggest = t.payload_len; biggest_i = i; }
        }

        std::printf("  modes        ");
        for (unsigned m = 0; m < 8; m++)
            if (mode_hist[m]) std::printf("%s=%u ", mode_name(m), mode_hist[m]);
        std::printf("\n  res_level    ");
        for (unsigned r = 0; r < 4; r++)
            if (res_hist[r]) std::printf("%u=%u ", r, res_hist[r]);
        std::printf("\n  table_set    ");
        for (unsigned s = 0; s < 8; s++)
            if (tab_hist[s]) std::printf("%u=%u ", s, tab_hist[s]);
        std::printf("\n  qp           %u..%u", qp_min, qp_max);
        std::printf("   tskip %u/%u tiles\n", tskip_n, count);
        std::printf("  payload      %llu bytes total, %.1f mean, %llu max "
                    "(tile %u at %u,%u)\n",
                    (unsigned long long)total,
                    count ? double(total) / count : 0.0,
                    (unsigned long long)biggest, biggest_i,
                    tl.tiles_x ? biggest_i % tl.tiles_x : 0,
                    tl.tiles_x ? biggest_i / tl.tiles_x : 0);
        // Tile-header + directory overhead is the number that decides whether
        // the transport's run packing is worth it (TRANSPORT.md 3).
        std::printf("  overhead     %llu bytes of frame not in tile payloads "
                    "(%.1f%%)\n",
                    (unsigned long long)(fi.frame_bytes - total),
                    fi.frame_bytes ? 100.0 * double(fi.frame_bytes - total) /
                                         double(fi.frame_bytes)
                                   : 0.0);

        if (!sizes.empty()) {
            std::vector<uint32_t> sorted = sizes;
            std::sort(sorted.begin(), sorted.end());
            const uint32_t p95 = sorted[size_t(0.95 * (sorted.size() - 1))];
            std::printf("  bytes/tile   p50 %u  p95 %u  (map scaled to p95)\n",
                        sorted[sorted.size() / 2], p95);
            for (uint32_t y = 0; y < tl.tiles_y; y++) {
                std::printf("   |");
                for (uint32_t x = 0; x < tl.tiles_x; x++) {
                    const uint32_t i = y * tl.tiles_x + x;
                    std::putchar(i < count ? density_char(sizes[i], p95) : '?');
                }
                std::printf("|\n");
            }
        }

        off += consumed ? consumed : scanned;
        fno++;
        if (want_frame >= 0) break;
    }

    if (fcsv) std::fclose(fcsv);
    nxvc_decoder_destroy(dec);
    if (fno == 0) {
        std::fprintf(stderr, "no frames in %s\n", in_path.c_str());
        return 1;
    }
    return rc;
}
