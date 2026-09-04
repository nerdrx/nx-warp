// Stream-header / TLV forward compatibility and header validation.
#include "test_util.h"
#include "nxvc/nxvc.h"

static std::vector<uint8_t> make_header(const nxvc_config &cfg,
                                        void (*add)(nxvc_encoder *)) {
    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) return {};
    if (add) add(e);
    std::vector<uint8_t> h(8192);
    size_t hl = 0;
    nxvc_encoder_stream_header(e, h.data(), h.size(), &hl);
    nxvc_encoder_destroy(e);
    h.resize(hl);
    return h;
}

int main() {
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = 128;
    cfg.height = 128;

    // 1. A bare header is exactly 64 bytes and parses.
    {
        std::vector<uint8_t> h = make_header(cfg, nullptr);
        CHECK(h.size() == 64, "bare header is %zu bytes", h.size());
        nxvc_status st;
        nxvc_decoder *d = nxvc_decoder_create(&st);
        size_t cons = 0;
        CHECK(nxvc_decoder_parse_stream_header(d, h.data(), h.size(), &cons) ==
                  NXVC_OK, "bare header parse");
        CHECK(cons == 64, "consumed %zu", cons);
        nxvc_stream_info si;
        nxvc_decoder_stream_info(d, &si);
        CHECK(si.width == 128 && si.height == 128, "geometry");
        CHECK(si.version == 1, "version %u", si.version);
        CHECK(si.ext_len == 0, "ext_len %u", si.ext_len);
        nxvc_decoder_destroy(d);
    }

    // 2. Unknown TLVs are skipped, and a frame after them still decodes.
    {
        nxvc_status st;
        nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
        const uint8_t p1[5] = {1, 2, 3, 4, 5};
        const uint8_t p2[16] = {0};
        CHECK(nxvc_encoder_add_tlv(e, 0x8001, p1, 5) == NXVC_OK, "tlv 1");
        CHECK(nxvc_encoder_add_tlv(e, 0xBEEF, p2, 16) == NXVC_OK, "tlv 2");
        CHECK(nxvc_encoder_add_tlv(e, 0x8002, nullptr, 0) == NXVC_OK, "tlv 3");
        std::vector<uint8_t> h(8192);
        size_t hl = 0;
        CHECK(nxvc_encoder_stream_header(e, h.data(), h.size(), &hl) == NXVC_OK,
              "header with tlvs");
        // 64 + (4+5+3) + (4+16) + (4+0) = 64 + 12 + 20 + 4 = 100
        CHECK(hl == 100, "header with tlvs is %zu bytes", hl);

        TestImage im = make_image(128, 128, false, 1);
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
        img.stride[0] = 128; img.stride[1] = im.cw; img.stride[2] = im.cw;
        img.stride[3] = 128;
        std::vector<uint8_t> fr(1 << 20);
        size_t ol = 0;
        CHECK(nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, fr.data(),
                                        fr.size(), &ol) == NXVC_OK, "encode");
        nxvc_encoder_destroy(e);

        nxvc_decoder *d = nxvc_decoder_create(&st);
        size_t cons = 0;
        CHECK(nxvc_decoder_parse_stream_header(d, h.data(), hl, &cons) == NXVC_OK,
              "parse header with unknown tlvs");
        CHECK(cons == hl, "consumed %zu of %zu", cons, hl);
        nxvc_stream_info si;
        nxvc_decoder_stream_info(d, &si);
        CHECK(si.ext_tlv_count == 3, "saw %u tlvs", si.ext_tlv_count);
        CHECK(si.ext_unknown_count == 3, "unknown %u", si.ext_unknown_count);
        TestImage o = im;
        nxvc_image oi{};
        for (int p = 0; p < 4; ++p) oi.plane[p] = o.p[p].data();
        oi.stride[0] = 128; oi.stride[1] = o.cw; oi.stride[2] = o.cw;
        oi.stride[3] = 128;
        CHECK(nxvc_decoder_decode_frame(d, fr.data(), ol, &oi, &cons) == NXVC_OK,
              "decode after unknown tlvs");
        nxvc_decoder_destroy(d);
    }

    // 3. Malformed headers are refused with the right status.
    {
        std::vector<uint8_t> h = make_header(cfg, nullptr);
        auto parse = [&](std::vector<uint8_t> b) {
            nxvc_status st;
            nxvc_decoder *d = nxvc_decoder_create(&st);
            size_t cons = 0;
            nxvc_status r =
                nxvc_decoder_parse_stream_header(d, b.data(), b.size(), &cons);
            nxvc_decoder_destroy(d);
            return r;
        };
        std::vector<uint8_t> bad;

        bad = h; bad[0] ^= 0xff;
        CHECK(parse(bad) == NXVC_ERR_VERSION, "bad magic");

        bad = h; bad[4] = 2;
        CHECK(parse(bad) == NXVC_ERR_VERSION, "bad version");

        bad = h; bad[7] = 1;  // tile_size 32x32
        CHECK(parse(bad) == NXVC_ERR_UNSUPPORTED, "32x32 tiles");

        bad = h; bad[7] = 2;  // reserved bits set
        CHECK(parse(bad) == NXVC_ERR_BITSTREAM, "reserved tile_size bits");

        bad = h; bad[39] = 0x80;  // unknown mandatory tool bit (bit 63)
        CHECK(parse(bad) == NXVC_ERR_VERSION, "unknown tool bit");

        bad = h; bad[12] = 3;  // eyes = 3
        CHECK(parse(bad) == NXVC_ERR_UNSUPPORTED, "eyes 3");

        bad = h; bad[13] = 10;  // bit depth 10
        CHECK(parse(bad) == NXVC_ERR_UNSUPPORTED, "10-bit");

        bad = h; bad[8] = 5; bad[9] = 0;  // width 5
        CHECK(parse(bad) == NXVC_ERR_BITSTREAM, "odd tiny width");

        bad = h; bad.resize(63);
        CHECK(parse(bad) == NXVC_ERR_TRUNCATED, "short header");

        bad = h; bad[62] = 8; bad[63] = 0;  // ext_len 8 with no ext bytes
        CHECK(parse(bad) == NXVC_ERR_TRUNCATED, "ext_len past end");

        // A TLV whose length runs past the extension area.
        bad = h;
        bad[62] = 8; bad[63] = 0;
        bad.insert(bad.end(), {0x01, 0x80, 0xff, 0x00, 0, 0, 0, 0});
        CHECK(parse(bad) == NXVC_ERR_BITSTREAM, "tlv overrun");
    }

    // 4. Frame headers: truncation and illegal fields.
    {
        nxvc_status st;
        nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
        std::vector<uint8_t> h(4096);
        size_t hl = 0;
        nxvc_encoder_stream_header(e, h.data(), h.size(), &hl);
        TestImage im = make_image(128, 128, false, 1);
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
        img.stride[0] = 128; img.stride[1] = im.cw; img.stride[2] = im.cw;
        img.stride[3] = 128;
        std::vector<uint8_t> fr(1 << 20);
        size_t ol = 0;
        nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, fr.data(), fr.size(),
                                  &ol);
        nxvc_encoder_destroy(e);
        fr.resize(ol);

        TestImage o = im;
        auto decode = [&](const std::vector<uint8_t> &b) {
            nxvc_decoder *d = nxvc_decoder_create(&st);
            size_t cons = 0;
            nxvc_decoder_parse_stream_header(d, h.data(), hl, &cons);
            nxvc_image oi{};
            for (int p = 0; p < 4; ++p) oi.plane[p] = o.p[p].data();
            oi.stride[0] = 128; oi.stride[1] = o.cw; oi.stride[2] = o.cw;
            oi.stride[3] = 128;
            nxvc_status r =
                nxvc_decoder_decode_frame(d, b.data(), b.size(), &oi, &cons);
            nxvc_decoder_destroy(d);
            return r;
        };
        CHECK(decode(fr) == NXVC_OK, "reference frame decodes");
        for (size_t cut = 1; cut < fr.size(); cut = cut * 2 + 1) {
            std::vector<uint8_t> t(fr.begin(), fr.begin() + cut);
            nxvc_status r = decode(t);
            CHECK(r != NXVC_OK, "truncated frame at %zu accepted", cut);
        }
        std::vector<uint8_t> bad = fr;
        bad[28] = 64;  // base_qp out of range
        CHECK(decode(bad) == NXVC_ERR_BITSTREAM, "base_qp 64");
        bad = fr;
        bad[31] = 7;  // quant_matrix 7
        CHECK(decode(bad) == NXVC_ERR_BITSTREAM, "quant_matrix 7");
        bad = fr;
        bad[0] ^= 0x55;  // frame_number mismatch against the tile-row headers
        CHECK(decode(bad) == NXVC_ERR_BITSTREAM, "frame number mismatch");
    }

    // 5. A maximum-width stereo header is legal, and its plane is twice as
    //    wide as one eye.  `width` is per eye (SYNTAX.md 3.3), so
    //    nxvc_decoder_plane_size reports `eyes * width` -- 8192 at the widest
    //    legal width, not 4096.  FINDINGS F11: the headers fuzz harness
    //    asserted the mono bound here and aborted the CI campaign on an input
    //    the decoder was right to accept.  Height stays small so the four
    //    reference slots this allocates do not dominate the sanitizer run.
    {
        nxvc_config wide;
        nxvc_config_default(&wide);
        wide.width = 4096;
        wide.height = 64;
        std::vector<uint8_t> h = make_header(wide, nullptr);
        CHECK(h.size() == 64, "stereo-width header is %zu bytes", h.size());
        h[8] = 0x00; h[9] = 0x10;   // width  = 4096
        h[10] = 64;  h[11] = 0;     // height = 64
        h[12] = 2;                  // eyes   = 2

        nxvc_status st;
        nxvc_decoder *d = nxvc_decoder_create(&st);
        size_t cons = 0;
        CHECK(nxvc_decoder_parse_stream_header(d, h.data(), h.size(), &cons) ==
                  NXVC_OK, "4096-wide stereo header parse");
        nxvc_stream_info si;
        nxvc_decoder_stream_info(d, &si);
        CHECK(si.width == 4096 && si.eyes == 2, "stereo geometry %u x %u eyes",
              si.width, si.eyes);
        uint32_t yw = 0, yh = 0;
        CHECK(nxvc_decoder_plane_size(d, 0, &yw, &yh) == NXVC_OK, "plane 0");
        CHECK(yw == 8192, "stereo luma plane width %u, expected 8192", yw);
        CHECK(yh == 64, "stereo luma plane height %u", yh);
        nxvc_decoder_destroy(d);
    }

    return test_report("test_headers");
}
