// Every effort configuration must actually quantize the tile.
//
// `quantize_tile_ex` has three ways to leave a tile ready to code: the
// two-pass path (`rdoq_effort != fast`), the trellis path (`intra_dir ||
// use_rdo`), and the plain single dead-zone pass.  The third one did not
// exist.  With `fast` rdoq the first pass is skipped, and with the trellis off
// and the tile not directional the second is skipped too, so nothing wrote a
// coefficient and `count_units` walked a buffer that had never been filled.
// Every tile coded as all-zero and the encoder emitted a legal, tiny, ruined
// stream: on a 1088x1088 frame at QP 30, 3872 bytes and 12.9 dB where the
// same picture is 33182 bytes and 38.3 dB.  Nothing failed, nothing was
// logged, and the only symptom was the rate -- which is the symptom a codec
// that is merely aggressive also has.
//
// `--preset fast --no-rdo` and `--rdoq-effort 1 --no-rdo` are the two ways to
// reach it from the CLI, and both are natural things to ask for: `--no-rdo`
// is what the GPU encoder's acid test pins, and `--preset fast` is what a
// real-time caller sets.  The combination was the one nobody had run.
//
// The property this file pins is not a byte pattern.  It is that the effort
// knobs are *effort* knobs: rdoq_effort and rdo change how long the encoder
// looks for a good answer, never whether it codes the picture at all.  So the
// check is on the rate and the reconstruction, against the same configuration
// at the effort that was known to work, with a generous tolerance -- a real
// coding difference between two efforts is a few percent, and the bug was 8x.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"
#include "test_util.h"

namespace {

struct Clip {
    int w = 0, h = 0, cw = 0, ch = 0;
    std::vector<std::vector<uint8_t>> Y, U, V;
};

// Structured, high-contrast content: a tile of this codes to real bits at any
// sane QP, so "the stream got small" cannot be confused with "the picture was
// flat".
Clip make_clip(int w, int h, int nframes) {
    Clip c;
    c.w = w;
    c.h = h;
    c.cw = (w + 1) / 2;
    c.ch = (h + 1) / 2;
    for (int f = 0; f < nframes; ++f) {
        std::vector<uint8_t> Y((size_t)w * h), U((size_t)c.cw * c.ch),
            V((size_t)c.cw * c.ch);
        const double px = f * 3.7, py = f * 2.1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double sx = x + px, sy = y + py;
                double v = 128 + 60 * std::sin(sx * 0.041) * std::cos(sy * 0.037) +
                           30 * std::sin((sx * 3 + sy * 5) * 0.11);
                v += ((int)(sx / 9 + sy / 7) % 2) ? 18 : -18;
                Y[(size_t)y * w + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        for (int y = 0; y < c.ch; ++y)
            for (int x = 0; x < c.cw; ++x) {
                const int sx = x + (int)px, sy = y + (int)py;
                U[(size_t)y * c.cw + x] = (uint8_t)(108 + ((sx * 5 + sy * 3) % 64));
                V[(size_t)y * c.cw + x] = (uint8_t)(146 - ((sx * 3 + sy * 7) % 64));
            }
        c.Y.push_back(std::move(Y));
        c.U.push_back(std::move(U));
        c.V.push_back(std::move(V));
    }
    return c;
}

struct Result {
    size_t bytes = 0;
    double psnr = 0;
    bool ok = false;
};

// Encode the clip, decode it back, and report the rate and the luma PSNR.
// Both halves matter: the bug made the stream small AND the picture wrong, and
// a check on either alone would pass some other way of being broken.
Result run(const nxvc_config &base, const Clip &c) {
    Result r;
    nxvc_config cfg = base;
    nxvc_status st = NXVC_OK;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) return r;

    std::vector<uint8_t> header(4096, 0);
    size_t hl = 0;
    if (nxvc_encoder_stream_header(e, header.data(), header.size(), &hl) !=
        NXVC_OK) {
        nxvc_encoder_destroy(e);
        return r;
    }
    header.resize(hl);

    nxvc_decoder *d = nxvc_decoder_create(&st);
    size_t consumed = 0;
    if (!d || nxvc_decoder_parse_stream_header(d, header.data(), header.size(),
                                               &consumed) != NXVC_OK) {
        if (d) nxvc_decoder_destroy(d);
        nxvc_encoder_destroy(e);
        return r;
    }

    // The decoder writes into planes the caller owns, so they are allocated
    // once at the clip's geometry and reused for every frame.
    std::vector<uint8_t> oY((size_t)c.w * c.h), oU((size_t)c.cw * c.ch),
        oV((size_t)c.cw * c.ch);
    std::vector<uint8_t> buf((size_t)c.w * c.h * 4 + (1 << 20));
    double se = 0;
    size_t n_samples = 0;
    for (size_t f = 0; f < c.Y.size(); ++f) {
        nxvc_view v[1];
        const double a = 0.003 * (double)f;
        v[0] = nxvc_view{0, std::sin(a), 0, std::cos(a), -0.8, 0.8, 0.8, -0.8};
        nxvc_encoder_set_views(e, v, 1);

        nxvc_image img{};
        img.plane[0] = const_cast<uint8_t *>(c.Y[f].data());
        img.plane[1] = const_cast<uint8_t *>(c.U[f].data());
        img.plane[2] = const_cast<uint8_t *>(c.V[f].data());
        img.stride[0] = c.w;
        img.stride[1] = c.cw;
        img.stride[2] = c.cw;
        size_t n = 0;
        if (nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, buf.data(),
                                      buf.size(), &n) != NXVC_OK)
            break;
        r.bytes += n;

        nxvc_image out{};
        out.plane[0] = oY.data();
        out.plane[1] = oU.data();
        out.plane[2] = oV.data();
        out.stride[0] = c.w;
        out.stride[1] = c.cw;
        out.stride[2] = c.cw;
        if (nxvc_decoder_decode_frame(d, buf.data(), n, &out, &consumed) !=
            NXVC_OK)
            break;
        for (int y = 0; y < c.h; ++y)
            for (int x = 0; x < c.w; ++x) {
                const double dd = (double)c.Y[f][(size_t)y * c.w + x] -
                                  (double)oY[(size_t)y * c.w + x];
                se += dd * dd;
                ++n_samples;
            }
    }
    nxvc_decoder_destroy(d);
    nxvc_encoder_destroy(e);
    if (!n_samples) return r;
    const double mse = se / (double)n_samples;
    r.psnr = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
    r.ok = true;
    return r;
}

// `name` is compared against `ref`: the same configuration at an effort that
// was never broken.  A working effort knob moves the rate by a few percent;
// the bug moved it by 8x and the PSNR by 25 dB, so the bounds are wide enough
// to be about correctness and not about tuning.
void expect_codes(const char *name, const nxvc_config &cfg, const Clip &c,
                  const Result &ref) {
    const Result r = run(cfg, c);
    CHECK(r.ok, "%s: encode/decode failed", name);
    if (!r.ok) return;
    CHECK(r.bytes * 2 > ref.bytes,
          "%s: %zu bytes against the reference effort's %zu -- more than 2x "
          "smaller means the tile was never quantized",
          name, r.bytes, ref.bytes);
    CHECK(r.psnr > ref.psnr - 3.0,
          "%s: %.2f dB against the reference effort's %.2f dB", name, r.psnr,
          ref.psnr);
}

}  // namespace

int main() {
    const Clip c = make_clip(256, 256, 3);

    // The reference point: default effort (medium rdoq), trellis on.  This is
    // the configuration every existing test exercises.
    nxvc_config base;
    nxvc_config_default(&base);
    base.width = 256;
    base.height = 256;
    base.base_qp = 30;
    base.intra_dir = 0;

    const Result ref = run(base, c);
    CHECK(ref.ok, "reference effort: encode/decode failed");
    if (!ref.ok) return test_report("effort");
    CHECK(ref.bytes > 4000, "reference effort produced only %zu bytes",
          ref.bytes);
    CHECK(ref.psnr > 25.0, "reference effort produced only %.2f dB", ref.psnr);

    // The four ways to turn the trellis and the two-pass table feedback off,
    // in both orders the CLI can express them.  Every one of these must still
    // code the picture.
    nxvc_config cfg;

    cfg = base;
    cfg.rdo = 0;
    expect_codes("no-rdo", cfg, c, ref);

    cfg = base;
    cfg.preset = NXVC_PRESET_FAST;
    expect_codes("preset-fast", cfg, c, ref);

    // The combination that was broken.
    cfg = base;
    cfg.preset = NXVC_PRESET_FAST;
    cfg.rdo = 0;
    expect_codes("preset-fast+no-rdo", cfg, c, ref);

    // The same thing reached through the knob rather than the preset.
    cfg = base;
    cfg.rdoq_effort = 1;  // 1 == kRdoqFast
    cfg.rdo = 0;
    expect_codes("rdoq-fast+no-rdo", cfg, c, ref);

    // And with inter prediction on, which is the configuration the GPU
    // encoder's acid test will pin: the mode decision calls the same
    // `quantize_tile_ex` to score its intra candidate, so a tile that never
    // quantizes makes INTRA look free and distorts every decision as well as
    // every stream.
    cfg = base;
    cfg.preset = NXVC_PRESET_FAST;
    cfg.rdo = 0;
    cfg.inter = 1;
    cfg.intra_period = 180;
    const Result ri = run(cfg, c);
    CHECK(ri.ok, "inter/preset-fast+no-rdo: encode/decode failed");
    if (ri.ok)
        CHECK(ri.psnr > ref.psnr - 6.0,
              "inter/preset-fast+no-rdo: %.2f dB against intra's %.2f dB",
              ri.psnr, ref.psnr);

    return test_report("effort");
}
