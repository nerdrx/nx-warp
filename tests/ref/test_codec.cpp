// End-to-end encode/decode: PSNR at several QPs, lossless bit-exactness,
// every tool combination the Phase 1 encoder can emit.
#include "test_util.h"
#include "nxvc/nxvc.h"

struct Coded {
    std::vector<uint8_t> header, frame;
    TestImage out;
    nxvc_status enc_status = NXVC_OK, dec_status = NXVC_OK;
};

static bool code(const nxvc_config &cfg_in, const TestImage &im, Coded &r,
                 const uint8_t *qp_map = nullptr,
                 const uint8_t *res_map = nullptr) {
    nxvc_config cfg = cfg_in;
    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) { r.enc_status = st; return false; }
    r.header.assign(4096, 0);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(e, r.header.data(), r.header.size(), &hl);
    if (st != NXVC_OK) { r.enc_status = st; nxvc_encoder_destroy(e); return false; }
    r.header.resize(hl);

    nxvc_image img{};
    for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
    img.stride[0] = im.w; img.stride[1] = im.cw;
    img.stride[2] = im.cw; img.stride[3] = im.w;

    r.frame.assign((size_t)im.w * im.h * 6 + (1u << 20), 0);
    size_t ol = 0;
    st = nxvc_encoder_encode_frame(e, &img, qp_map, res_map, r.frame.data(),
                                   r.frame.size(), &ol);
    nxvc_encoder_destroy(e);
    if (st != NXVC_OK) { r.enc_status = st; return false; }
    r.frame.resize(ol);

    nxvc_decoder *d = nxvc_decoder_create(&st);
    size_t consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, r.header.data(), r.header.size(),
                                          &consumed);
    if (st != NXVC_OK) { r.dec_status = st; nxvc_decoder_destroy(d); return false; }
    r.out = im;
    for (int p = 0; p < 4; ++p) std::fill(r.out.p[p].begin(), r.out.p[p].end(), 0);
    nxvc_image oi{};
    for (int p = 0; p < 4; ++p) oi.plane[p] = r.out.p[p].data();
    oi.stride[0] = im.w; oi.stride[1] = im.cw;
    oi.stride[2] = im.cw; oi.stride[3] = im.w;
    st = nxvc_decoder_decode_frame(d, r.frame.data(), r.frame.size(), &oi,
                                   &consumed);
    nxvc_decoder_destroy(d);
    if (st != NXVC_OK) { r.dec_status = st; return false; }
    if (consumed != r.frame.size()) { r.dec_status = NXVC_ERR_BITSTREAM; return false; }
    return true;
}

int main() {
    const int W = 192, H = 128;

    // 1. Rate/quality behaves monotonically and stays above a floor.
    {
        TestImage im = make_image(W, H, false, 1);
        double prev_psnr = 1e9;
        size_t prev_bytes = 0;
        const int qps[] = {0, 8, 16, 24, 32, 40, 48, 56, 63};
        const double floors[] = {48, 44, 38, 33, 29, 26, 22, 18, 14};
        for (int i = 0; i < 9; ++i) {
            nxvc_config cfg;
            nxvc_config_default(&cfg);
            cfg.width = W; cfg.height = H; cfg.base_qp = qps[i];
            Coded r;
            CHECK(code(cfg, im, r), "encode/decode qp %d (%s / %s)", qps[i],
                  nxvc_status_string(r.enc_status), nxvc_status_string(r.dec_status));
            if (r.out.p[0].empty()) continue;
            double p = psnr8(im.p[0].data(), r.out.p[0].data(), im.p[0].size());
            CHECK(p >= floors[i], "qp %d luma psnr %.2f below floor %.1f", qps[i],
                  p, floors[i]);
            CHECK(p <= prev_psnr + 0.01, "qp %d psnr %.2f rose above qp %d",
                  qps[i], p, qps[i - 1 < 0 ? 0 : i - 1]);
            if (i) CHECK(r.frame.size() <= prev_bytes, "qp %d grew: %zu > %zu",
                         qps[i], r.frame.size(), prev_bytes);
            prev_psnr = p;
            prev_bytes = r.frame.size();
        }
    }

    // 2. Lossless is bit exact, for every chroma format, with and without alpha
    //    and through the YCoCg-R path.
    {
        for (int kind = 0; kind <= 4; ++kind)
            for (int c444 = 0; c444 <= 1; ++c444)
                for (int alpha = 0; alpha <= 1; ++alpha) {
                    TestImage im = make_image(W, H, c444 != 0, kind, 900 + kind);
                    nxvc_config cfg;
                    nxvc_config_default(&cfg);
                    cfg.width = W; cfg.height = H;
                    cfg.chroma = c444 ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
                    cfg.alpha = (uint32_t)alpha;
                    cfg.lossless = 1;
                    Coded r;
                    CHECK(code(cfg, im, r), "lossless kind %d c444 %d alpha %d",
                          kind, c444, alpha);
                    if (r.out.p[0].empty()) continue;
                    CHECK(r.out.p[0] == im.p[0], "lossless Y mismatch k%d c%d a%d",
                          kind, c444, alpha);
                    CHECK(r.out.p[1] == im.p[1], "lossless Co mismatch k%d c%d a%d",
                          kind, c444, alpha);
                    CHECK(r.out.p[2] == im.p[2], "lossless Cg mismatch k%d c%d a%d",
                          kind, c444, alpha);
                    if (alpha)
                        CHECK(r.out.p[3] == im.p[3], "lossless A mismatch k%d c%d",
                              kind, c444);
                }
    }

    // 3. Lossless through YCoCg-R (RGB in, RGB out) is bit exact.
    {
        for (int kind = 0; kind <= 3; ++kind) {
            TestImage im = make_image(W, H, true, kind, 400 + kind);
            nxvc_config cfg;
            nxvc_config_default(&cfg);
            cfg.width = W; cfg.height = H;
            cfg.chroma = NXVC_CHROMA_444;
            cfg.color_transform = NXVC_CT_YCOCGR;
            cfg.color_space = NXVC_CS_RGB;
            cfg.lossless = 1;
            Coded r;
            CHECK(code(cfg, im, r), "ycocgr lossless kind %d", kind);
            if (r.out.p[0].empty()) continue;
            for (int p = 0; p < 3; ++p)
                CHECK(r.out.p[p] == im.p[p], "ycocgr lossless plane %d kind %d", p,
                      kind);
        }
    }

    // 4. The YCoCg-R lifting itself is exactly reversible.
    {
        Rng rng(4242);
        std::vector<uint8_t> R(4096), G(4096), B(4096), r2(4096), g2(4096), b2(4096);
        std::vector<uint8_t> Y(4096);
        std::vector<uint16_t> Co(4096), Cg(4096);
        for (int i = 0; i < 4096; ++i) {
            R[i] = (uint8_t)rng.next(); G[i] = (uint8_t)rng.next();
            B[i] = (uint8_t)rng.next();
        }
        nxvc_ycocgr_forward(R.data(), G.data(), B.data(), Y.data(), Co.data(),
                            Cg.data(), 4096);
        nxvc_ycocgr_inverse(Y.data(), Co.data(), Cg.data(), r2.data(), g2.data(),
                            b2.data(), 4096);
        CHECK(R == r2 && G == g2 && B == b2, "YCoCg-R is not reversible");
        for (int i = 0; i < 4096; ++i)
            CHECK(Co[i] <= 511 && Cg[i] <= 511, "chroma out of 9-bit range");
    }

    // 5. Every tool combination the encoder can emit round trips.
    {
        TestImage im420 = make_image(W, H, false, 1);
        TestImage im444 = make_image(W, H, true, 1);
        struct Case { const char *name; int c444, alpha, tskip, nsub, tab, t420, ct; };
        const Case cases[] = {
            {"420 base", 0, 0, 0, 3, 0, 0, 0},
            {"420 alpha", 0, 1, 0, 3, 0, 0, 0},
            {"420 tskip", 0, 0, 1, 3, 0, 0, 0},
            {"420 tskip auto", 0, 0, 2, 3, 0, 0, 0},
            {"420 custom tables", 0, 0, 0, 3, 1, 0, 0},
            {"420 nsub auto", 0, 0, 0, 255, 0, 0, 0},
            {"444 base", 1, 0, 0, 3, 0, 0, 0},
            {"444 alpha", 1, 1, 0, 3, 0, 0, 0},
            {"444 tile 420", 1, 0, 0, 3, 0, 1, 0},
            {"444 ycocgr", 1, 0, 0, 3, 0, 0, 1},
            {"444 ycocgr t420", 1, 0, 0, 3, 0, 1, 1},
        };
        for (const Case &c : cases) {
            for (int n = 0; n <= 5; ++n) {
                nxvc_config cfg;
                nxvc_config_default(&cfg);
                cfg.width = W; cfg.height = H;
                cfg.chroma = c.c444 ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
                cfg.alpha = (uint32_t)c.alpha;
                cfg.transform_skip = (uint32_t)c.tskip;
                cfg.nsub_log2 = c.nsub == 255 ? 255u : (uint32_t)n;
                cfg.custom_tables = (uint32_t)c.tab;
                cfg.tile_chroma420 = (uint32_t)c.t420;
                cfg.color_transform = (uint32_t)c.ct;
                cfg.color_space = c.ct ? (uint32_t)NXVC_CS_RGB : 0u;
                cfg.base_qp = 26;
                Coded r;
                CHECK(code(cfg, c.c444 ? im444 : im420, r),
                      "%s nsub %d: %s / %s", c.name, n,
                      nxvc_status_string(r.enc_status),
                      nxvc_status_string(r.dec_status));
                if (c.nsub == 255) break;
            }
        }
    }

    // 6. Per-tile QP and resolution maps.
    {
        TestImage im = make_image(W, H, false, 1);
        nxvc_tile_layout tl;
        nxvc_tile_layout_get(W, H, &tl);
        std::vector<uint8_t> qm(tl.tile_count), rm(tl.tile_count);
        for (uint32_t i = 0; i < tl.tile_count; ++i) {
            qm[i] = (uint8_t)(12 + (i * 7) % 40);
            rm[i] = (uint8_t)(i % 3);
        }
        nxvc_config cfg;
        nxvc_config_default(&cfg);
        cfg.width = W; cfg.height = H; cfg.base_qp = 28;
        Coded r;
        CHECK(code(cfg, im, r, qm.data(), rm.data()), "qp+res map: %s / %s",
              nxvc_status_string(r.enc_status), nxvc_status_string(r.dec_status));
        // The res-level tiles must still be coarsely right.
        if (!r.out.p[0].empty()) {
            double p = psnr8(im.p[0].data(), r.out.p[0].data(), im.p[0].size());
            CHECK(p > 20.0, "res-mapped psnr %.2f", p);
        }
        // res_level and the resolved QP must survive the header round trip.
        nxvc_status st;
        nxvc_decoder *d = nxvc_decoder_create(&st);
        size_t cons;
        nxvc_decoder_parse_stream_header(d, r.header.data(), r.header.size(), &cons);
        TestImage o = im;
        nxvc_image oi{};
        for (int p = 0; p < 4; ++p) oi.plane[p] = o.p[p].data();
        oi.stride[0] = W; oi.stride[1] = o.cw; oi.stride[2] = o.cw; oi.stride[3] = W;
        nxvc_decoder_decode_frame(d, r.frame.data(), r.frame.size(), &oi, &cons);
        uint32_t n = 0;
        const nxvc_tile_info *ti = nxvc_decoder_tiles(d, &n);
        CHECK(n == tl.tile_count, "tile count %u vs %u", n, tl.tile_count);
        for (uint32_t i = 0; i < n && i < tl.tile_count; ++i) {
            CHECK(ti[i].res_level == rm[i], "tile %u res %u vs %u", i,
                  ti[i].res_level, rm[i]);
            CHECK(ti[i].qp == qm[i], "tile %u qp %u vs %u", i, ti[i].qp, qm[i]);
            CHECK(ti[i].mode == NXVC_MODE_INTRA, "tile %u mode %u", i, ti[i].mode);
        }
        nxvc_decoder_destroy(d);
    }

    // 7. Odd (non-multiple-of-64) picture sizes.
    {
        const int sizes[][2] = {{16, 16}, {66, 34}, {130, 66}, {200, 140}, {320, 68}};
        for (auto &s : sizes) {
            TestImage im = make_image(s[0], s[1], false, 1, 77);
            nxvc_config cfg;
            nxvc_config_default(&cfg);
            cfg.width = (uint32_t)s[0];
            cfg.height = (uint32_t)s[1];
            cfg.lossless = 1;
            Coded r;
            CHECK(code(cfg, im, r), "size %dx%d: %s / %s", s[0], s[1],
                  nxvc_status_string(r.enc_status),
                  nxvc_status_string(r.dec_status));
            if (!r.out.p[0].empty())
                CHECK(r.out.p[0] == im.p[0], "size %dx%d not lossless", s[0], s[1]);
        }
    }

    // 8. Multi-frame streams decode in sequence.
    {
        TestImage im = make_image(W, H, false, 1);
        nxvc_config cfg;
        nxvc_config_default(&cfg);
        cfg.width = W; cfg.height = H; cfg.base_qp = 30;
        nxvc_status st;
        nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
        std::vector<uint8_t> hdr(4096);
        size_t hl;
        nxvc_encoder_stream_header(e, hdr.data(), hdr.size(), &hl);
        std::vector<uint8_t> stream(hdr.begin(), hdr.begin() + hl);
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
        img.stride[0] = W; img.stride[1] = im.cw; img.stride[2] = im.cw;
        img.stride[3] = W;
        std::vector<uint8_t> tmp((size_t)W * H * 6);
        for (int f = 0; f < 4; ++f) {
            size_t ol = 0;
            CHECK(nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, tmp.data(),
                                            tmp.size(), &ol) == NXVC_OK,
                  "multiframe encode %d", f);
            stream.insert(stream.end(), tmp.begin(), tmp.begin() + ol);
        }
        nxvc_encoder_destroy(e);
        nxvc_decoder *d = nxvc_decoder_create(&st);
        size_t off = 0, cons = 0;
        CHECK(nxvc_decoder_parse_stream_header(d, stream.data(), stream.size(),
                                               &cons) == NXVC_OK, "mf header");
        off = cons;
        TestImage o = im;
        nxvc_image oi{};
        for (int p = 0; p < 4; ++p) oi.plane[p] = o.p[p].data();
        oi.stride[0] = W; oi.stride[1] = o.cw; oi.stride[2] = o.cw; oi.stride[3] = W;
        int frames = 0;
        while (off < stream.size()) {
            st = nxvc_decoder_decode_frame(d, stream.data() + off,
                                           stream.size() - off, &oi, &cons);
            CHECK(st == NXVC_OK, "mf decode %d: %s", frames,
                  nxvc_status_string(st));
            if (st != NXVC_OK) break;
            nxvc_frame_info fi;
            nxvc_decoder_frame_info(d, &fi);
            CHECK(fi.frame_number == (uint32_t)frames, "frame number %u vs %d",
                  fi.frame_number, frames);
            off += cons;
            ++frames;
        }
        CHECK(frames == 4, "decoded %d frames", frames);
        nxvc_decoder_destroy(d);
    }

    return test_report("test_codec");
}
