// Decoder robustness: random and mutated bitstreams must never crash, hang or
// read out of bounds; they may only be accepted or rejected with a status.
#include "test_util.h"
#include "nxvc/nxvc.h"

#include "fuzz_target.h"

int main() {
    // 1. Pure random bytes of many lengths.
    {
        Rng rng(0xC0FFEE);
        std::vector<uint8_t> b;
        for (int trial = 0; trial < 4000; ++trial) {
            size_t n = (size_t)(rng.next() % 900) + 1;
            b.resize(n);
            for (auto &x : b) x = (uint8_t)rng.next();
            nxvc_fuzz_decode(b.data(), b.size());
        }
    }

    // 2. Random bytes behind a valid stream header (exercises the frame and
    //    tile parsers rather than the magic check).
    {
        nxvc_config cfg;
        nxvc_config_default(&cfg);
        cfg.width = 128;
        cfg.height = 128;
        nxvc_status st;
        nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
        std::vector<uint8_t> hdr(4096);
        size_t hl = 0;
        nxvc_encoder_stream_header(e, hdr.data(), hdr.size(), &hl);
        nxvc_encoder_destroy(e);
        hdr.resize(hl);
        Rng rng(0xBADF00D);
        for (int trial = 0; trial < 4000; ++trial) {
            std::vector<uint8_t> b = hdr;
            size_t n = (size_t)(rng.next() % 2000) + 1;
            for (size_t i = 0; i < n; ++i) b.push_back((uint8_t)rng.next());
            nxvc_fuzz_decode(b.data(), b.size());
        }
    }

    // 3. Bit-flip mutations of a real stream.
    {
        nxvc_config cfg;
        nxvc_config_default(&cfg);
        cfg.width = 128;
        cfg.height = 128;
        cfg.base_qp = 26;
        nxvc_status st;
        nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
        std::vector<uint8_t> stream(4096);
        size_t hl = 0;
        nxvc_encoder_stream_header(e, stream.data(), stream.size(), &hl);
        stream.resize(hl);
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
        stream.insert(stream.end(), fr.begin(), fr.begin() + ol);

        Rng rng(0x5EED);
        for (int trial = 0; trial < 6000; ++trial) {
            std::vector<uint8_t> b = stream;
            int nmut = 1 + (int)(rng.next() % 6);
            for (int m = 0; m < nmut; ++m) {
                size_t pos = rng.next() % b.size();
                b[pos] ^= (uint8_t)(1u << (rng.next() & 7));
            }
            if (rng.next() % 4 == 0) b.resize(1 + rng.next() % b.size());
            nxvc_fuzz_decode(b.data(), b.size());
        }
    }

    std::printf("test_fuzz_smoke: ok (no crashes)\n");
    return 0;
}
