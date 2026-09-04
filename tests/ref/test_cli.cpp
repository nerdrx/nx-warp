// End-to-end test of the three CLIs: nxv-enc, nxv-dec, nxv-info.
// argv: <nxv-enc> <nxv-dec> <nxv-info> <work-dir>
#include "test_util.h"
#include "nxvc/nxvc.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static std::string g_enc, g_dec, g_info, g_work;

static int run(const std::string &cmd) {
    int rc = std::system(cmd.c_str());
    return rc;
}

static bool write_frame(const std::string &path, int w, int h, bool c444,
                        int kind, uint32_t seed) {
    TestImage im = make_image(w, h, c444, kind, seed);
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(im.p[0].data(), 1, im.p[0].size(), f);
    std::fwrite(im.p[1].data(), 1, im.p[1].size(), f);
    std::fwrite(im.p[2].data(), 1, im.p[2].size(), f);
    std::fclose(f);
    return true;
}

static std::vector<uint8_t> slurp(const std::string &path) {
    std::vector<uint8_t> v;
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        v.resize((size_t)n);
        if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    }
    std::fclose(f);
    return v;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s ENC DEC INFO WORKDIR\n", argv[0]);
        return 2;
    }
    g_enc = argv[1]; g_dec = argv[2]; g_info = argv[3]; g_work = argv[4];
    const int W = 192, H = 128;
    const size_t ysz = (size_t)W * H;

    std::string src = g_work + "/src420.yuv";
    std::string src444 = g_work + "/src444.yuv";
    CHECK(write_frame(src, W, H, false, 1, 5), "write 420 source");
    CHECK(write_frame(src444, W, H, true, 1, 5), "write 444 source");

    // 1. Lossy round trip through the documented flags.
    {
        std::string nxv = g_work + "/out.nxv", yuv = g_work + "/out.yuv";
        CHECK(run(g_enc + " --in " + src + " --w 192 --h 128 --pix yuv420p"
                  " --qp 28 --out " + nxv + " --quiet") == 0, "nxv-enc");
        CHECK(run(g_dec + " --in " + nxv + " --out " + yuv +
                  " --pix yuv420p --quiet") == 0, "nxv-dec");
        std::vector<uint8_t> a = slurp(src), b = slurp(yuv);
        CHECK(a.size() == b.size(), "decoded size %zu vs %zu", b.size(), a.size());
        if (a.size() == b.size() && !a.empty()) {
            double p = psnr8(a.data(), b.data(), ysz);
            CHECK(p > 25.0, "cli luma psnr %.2f", p);
        }
        CHECK(run(g_info + " --in " + nxv + " --tiles > " + g_work +
                  "/info.txt") == 0, "nxv-info");
        std::vector<uint8_t> info = slurp(g_work + "/info.txt");
        CHECK(info.size() > 200, "nxv-info output is %zu bytes", info.size());
    }

    // 2. Lossless CLI round trip is byte identical.
    {
        std::string nxv = g_work + "/ll.nxv", yuv = g_work + "/ll.yuv";
        CHECK(run(g_enc + " --in " + src + " --w 192 --h 128 --pix yuv420p"
                  " --lossless --out " + nxv + " --quiet") == 0, "lossless enc");
        CHECK(run(g_dec + " --in " + nxv + " --out " + yuv + " --quiet") == 0,
              "lossless dec");
        CHECK(slurp(src) == slurp(yuv), "lossless CLI round trip differs");
    }

    // 3. 4:4:4 and the --pix mismatch guard.
    {
        std::string nxv = g_work + "/out444.nxv", yuv = g_work + "/out444.yuv";
        CHECK(run(g_enc + " --in " + src444 + " --w 192 --h 128 --pix yuv444p"
                  " --qp 24 --out " + nxv + " --quiet") == 0, "444 enc");
        CHECK(run(g_dec + " --in " + nxv + " --out " + yuv + " --quiet") == 0,
              "444 dec");
        CHECK(slurp(yuv).size() == ysz * 3, "444 output size");
        CHECK(run(g_dec + " --in " + nxv + " --out " + yuv +
                  " --pix yuv420p --quiet 2>/dev/null") != 0,
              "--pix mismatch must fail");
    }

    // 4. A per-tile resolution map file.
    {
        nxvc_tile_layout tl;
        nxvc_tile_layout_get(W, H, &tl);
        std::string mp = g_work + "/res.map";
        std::FILE *f = std::fopen(mp.c_str(), "wb");
        CHECK(f != nullptr, "open res map");
        if (f) {
            for (uint32_t i = 0; i < tl.tile_count; ++i) {
                uint8_t v = (uint8_t)(i % 3);
                std::fwrite(&v, 1, 1, f);
            }
            std::fclose(f);
        }
        std::string nxv = g_work + "/res.nxv", yuv = g_work + "/res.yuv";
        CHECK(run(g_enc + " --in " + src + " --w 192 --h 128 --pix yuv420p"
                  " --qp 30 --res-map " + mp + " --out " + nxv + " --quiet") == 0,
              "res-map enc");
        CHECK(run(g_dec + " --in " + nxv + " --out " + yuv + " --quiet") == 0,
              "res-map dec");
        CHECK(slurp(yuv).size() == ysz * 3 / 2, "res-map output size");
    }

    // 5. Multi-frame files: three concatenated frames in, three out.
    {
        std::string mf = g_work + "/mf.yuv";
        std::FILE *f = std::fopen(mf.c_str(), "wb");
        CHECK(f != nullptr, "open multiframe source");
        if (f) {
            for (int k = 0; k < 3; ++k) {
                TestImage im = make_image(W, H, false, 1, 100 + k);
                std::fwrite(im.p[0].data(), 1, im.p[0].size(), f);
                std::fwrite(im.p[1].data(), 1, im.p[1].size(), f);
                std::fwrite(im.p[2].data(), 1, im.p[2].size(), f);
            }
            std::fclose(f);
        }
        std::string nxv = g_work + "/mf.nxv", yuv = g_work + "/mf.yuv.out";
        CHECK(run(g_enc + " --in " + mf + " --w 192 --h 128 --pix yuv420p"
                  " --qp 26 --out " + nxv + " --quiet") == 0, "mf enc");
        CHECK(run(g_dec + " --in " + nxv + " --out " + yuv + " --quiet") == 0,
              "mf dec");
        CHECK(slurp(yuv).size() == ysz * 3 / 2 * 3, "multiframe output size");
    }

    return test_report("test_cli");
}
