// nxv-dec: decode an .nxv stream to raw planar 8-bit YUV frames.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"
#include "base_refresh.h"

static void usage() {
    std::fprintf(stderr,
        "usage: nxv-dec --in out.nxv --out out.yuv [--pix yuv444p|yuv420p]\n"
        "  --frames N   decode at most N frames\n"
        "  --decode-every N  decode only every Nth frame and skip the\n"
        "               rest without parsing them, as a client that\n"
        "               cannot keep up does; the skipped frames leave\n"
        "               holes in the reference ring\n"
        "  --nv12       write Y then interleaved UV (4:2:0 streams only)\n"
        "  --atlas-dump P  write the atlas per-tile table after each frame\n"
        "               to P, 64 bytes per tile per frame (SYNTAX 13.12.1)\n"
        "  --lose-every N  mark tiles lost on every Nth frame\n"
        "  --lose-frac M   ...one tile in M of them (default 4)\n"
        "  --quiet\n");
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
    return nxvc_decoder_atlas_plane((const nxvc_decoder *)c, pl, w, h, st);
}
int main(int argc, char **argv) {
    std::string in, out, pix;
    int frames = -1, quiet = 0, nv12 = 0, decode_every = 1;
    // --atlas-dump writes the NORMATIVE output under the atlas: the per-tile
    // table after each decoded frame (13.12.1), which is what a conformance
    // comparison reads.  --lose-* injects tile loss deterministically.
    std::string atlas_dump, atlas_base_path;
    int atlas_base_margin = 8;
    int lose_every = 0, lose_frac = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--in") in = val();
        else if (a == "--out") out = val();
        else if (a == "--pix") pix = val();
        else if (a == "--frames") frames = std::atoi(val());
        else if (a == "--decode-every") decode_every = std::atoi(val());
        else if (a == "--quiet") quiet = 1;
        else if (a == "--nv12") nv12 = 1;
        else if (a == "--atlas-dump") atlas_dump = val();
        else if (a == "--atlas-base") atlas_base_path = val();
        else if (a == "--atlas-base-margin")
            atlas_base_margin = std::atoi(val());
        else if (a == "--lose-every") lose_every = std::atoi(val());
        else if (a == "--lose-frac") lose_frac = std::atoi(val());
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (in.empty() || out.empty()) { usage(); return 2; }

    std::FILE *fi = std::fopen(in.c_str(), "rb");
    if (!fi) { std::perror("open input"); return 1; }
    std::fseek(fi, 0, SEEK_END);
    long fsz = std::ftell(fi);
    std::fseek(fi, 0, SEEK_SET);
    if (fsz <= 0) { std::fprintf(stderr, "empty input\n"); return 1; }
    std::vector<uint8_t> data((size_t)fsz);
    if (std::fread(data.data(), 1, data.size(), fi) != data.size()) {
        std::fprintf(stderr, "short read\n");
        return 1;
    }
    std::fclose(fi);

    std::FILE *fatlas = nullptr;
    BaseRefresh brefresh;
    std::vector<uint8_t> atlas_buf, lost;
    if (!atlas_dump.empty()) {
        fatlas = std::fopen(atlas_dump.c_str(), "wb");
        if (!fatlas) { std::perror("open --atlas-dump"); return 1; }
    }
    nxvc_status st;
    nxvc_decoder *dec = nxvc_decoder_create(&st);
    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(dec, data.data(), data.size(), &consumed);
    if (st != NXVC_OK) {
        std::fprintf(stderr, "stream header: %s\n", nxvc_status_string(st));
        return 1;
    }
    off += consumed;
    nxvc_stream_info si;
    nxvc_decoder_stream_info(dec, &si);
    if (nv12 && si.chroma != NXVC_CHROMA_420) {
        std::fprintf(stderr, "--nv12 requires a 4:2:0 stream\n");
        return 1;
    }
    const char *want = si.chroma == NXVC_CHROMA_444 ? "yuv444p" : "yuv420p";
    if (!pix.empty() && pix != want) {
        std::fprintf(stderr, "stream is %s, --pix says %s\n", want, pix.c_str());
        return 1;
    }
    uint32_t yw, yh, cw, ch;
    nxvc_decoder_plane_size(dec, 0, &yw, &yh);
    nxvc_decoder_plane_size(dec, 1, &cw, &ch);
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch), V((size_t)cw * ch),
        A((size_t)yw * yh, 255);

    if (!atlas_base_path.empty()) {
        // The decoder's side of the SAME rule the encoder ran: same threshold,
        // same base pictures, tile selection from the same codec function.
        if (si.eyes != 1) {
            std::fprintf(stderr, "--atlas-base needs a one-eye stream\n");
            return 1;
        }
        if (!brefresh.open(atlas_base_path, yw, yh,
                           (uint32_t)(atlas_base_margin > 0 ? atlas_base_margin
                                                            : 0),
                           nxvc_decoder_tile_count(dec))) {
            std::perror("open --atlas-base");
            return 1;
        }
    }
    std::FILE *fo = std::fopen(out.c_str(), "wb");
    if (!fo) { std::perror("open output"); return 1; }
    int n = 0;
    while (off < data.size() && (frames < 0 || n < frames)) {
        /* A frame this client does not even try: skipped whole, from the
         * length in its own header (SYNTAX.md 3.1, bytes 36-39).  The
         * decoder never sees it, so its ring slot stays empty -- which is
         * exactly the state that makes a later frame referencing it a
         * BITSTREAM error. */
        if (decode_every > 1 && (n % decode_every) != 0) {
            if (off + 40 > data.size()) break;
            const uint8_t *fh = data.data() + off;
            const size_t fb = (size_t)fh[36] | ((size_t)fh[37] << 8) |
                              ((size_t)fh[38] << 16) | ((size_t)fh[39] << 24);
            if (fb < 40 || off + fb > data.size()) {
                std::fprintf(stderr, "frame %d: bad frame length %zu\n", n, fb);
                return 1;
            }
            off += fb;
            ++n;
            continue;
        }
        nxvc_image img{};
        img.plane[0] = Y.data(); img.stride[0] = (int)yw;
        img.plane[1] = U.data(); img.stride[1] = (int)cw;
        img.plane[2] = V.data(); img.stride[2] = (int)cw;
        img.plane[3] = A.data(); img.stride[3] = (int)yw;
        // Deterministic tile loss.  Under the atlas this is the whole of
        // loss handling on the client side: the tile's atlas entry is simply
        // not updated (13.12.6).
        if (lose_every > 0 && (n % lose_every) == 0 && n > 0) {
            const uint32_t tc = nxvc_decoder_tile_count(dec);
            const int frac = lose_frac > 0 ? lose_frac : 4;
            lost.assign(tc, 0);
            for (uint32_t t = 0; t < tc; ++t)
                if ((int)((t + (uint32_t)n) % (uint32_t)frac) == 0) lost[t] = 1;
            nxvc_decoder_set_lost_tiles(dec, lost.data(), tc);
        }
        st = nxvc_decoder_decode_frame(dec, data.data() + off, data.size() - off,
                                       &img, &consumed);
        if (st != NXVC_OK) {
            std::fprintf(stderr, "frame %d: %s\n", n, nxvc_status_string(st));
            return 1;
        }
        if (brefresh.f &&
            !brefresh.step(
                (uint32_t)n,
                [&](uint32_t m, uint8_t *o, uint32_t c, uint32_t *ns) {
                    return nxvc_decoder_atlas_stale_tiles(dec, m, o, c, ns);
                },
                [&](const nxvc_base_patch *pp, uint32_t *ap, uint32_t *su) {
                    return nxvc_decoder_atlas_patch_base(dec, pp, ap, su);
                })) {
            std::fprintf(stderr, "base refresh failed at frame %d\n", n);
            return 1;
        }
        if (fatlas) {
            const size_t nb = nxvc_decoder_atlas_table_size(dec);
            if (nb) {
                atlas_buf.resize(nb);
                if (nxvc_decoder_atlas_table(dec, atlas_buf.data(), nb) ==
                    NXVC_OK)
                    std::fwrite(atlas_buf.data(), 1, nb, fatlas);
                uint8_t dg[32];
                atlas_digest(atlas_plane_shim, dec, dg);
                std::fwrite(dg, 1, sizeof(dg), fatlas);
                if (std::getenv("NXV_ATLAS_TILEDIG")) {
                    uint32_t w=0,h=0,stq=0;
                    const uint16_t *d1 = atlas_plane_shim(dec, 1, &w, &h, &stq);
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
        std::fwrite(Y.data(), 1, Y.size(), fo);
        if (nv12) {
            std::vector<uint8_t> uv(U.size() * 2);
            for (size_t i = 0; i < U.size(); ++i) {
                uv[i * 2] = U[i];
                uv[i * 2 + 1] = V[i];
            }
            std::fwrite(uv.data(), 1, uv.size(), fo);
        } else {
            std::fwrite(U.data(), 1, U.size(), fo);
            std::fwrite(V.data(), 1, V.size(), fo);
        }
        if (si.alpha) std::fwrite(A.data(), 1, A.size(), fo);
        off += consumed;
        ++n;
    }
    if (fatlas) std::fclose(fatlas);
    std::fclose(fo);
    if (!quiet)
        std::printf("%d frame(s), %ux%u %s%s\n", n, yw, yh, want,
                    si.alpha ? " +alpha" : "");
    nxvc_decoder_destroy(dec);
    return n > 0 ? 0 : 1;
}
