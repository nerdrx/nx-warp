// nxrc::EncDriver against the real encoder: the four invariants that make a
// rate-controlled encode safe to ship.
//
//   1. determinism -- the same frames in, the same bytes out, twice;
//   2. a text panel is never resampled and never has its residual withheld
//      while it is changing (docs/RATECONTROL.md 4.4 and 8, PAPER.md 4.6.1);
//   3. no tile inside the fovea is ever skipped by the temporal ladder;
//   4. the maps the driver hands the encoder are the ones that come back out
//      of the bitstream, so the wiring is not silently dropping them.
//
// The material is nxrc::synth, the same generators the rc unit tests and
// nxvc-rcsim classify, so a classification failure here is a failure of the
// same code path they cover rather than of a private fixture.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstring>
#include <string>
#include <vector>

#include "nxrc/encdrive.hpp"
#include "nxrc/synth.hpp"
#include "nxvc/nxvc.h"
#include "rc_test_util.h"

namespace {

constexpr int kTile = NXVC_TILE_SIZE;
constexpr int kEyeW = 512;          // 8 x 8 tiles per eye
constexpr int kH    = 512;
constexpr int kEyes = 2;
constexpr int kTX   = kEyeW / kTile;
constexpr int kTY   = kH / kTile;
constexpr int kW    = kEyeW * kEyes;

// The panel: one tile column and row inside each eye, near the axis, filled
// with glyphs.  It is redrawn with a different seed every frame so it is
// always "changing" in the sense invariant 2 is about.
constexpr int kPanelCol = 3, kPanelRow = 3;

size_t tile_id(int row, int eye, int col) {
    return size_t(row) * kEyes * kTX + size_t(eye) * kTX + size_t(col);
}

// One frame: glyphs in the panel tile, band-limited texture everywhere else,
// scrolled by `frame` so the whole picture has real inter residual.
void make_frame(std::vector<uint8_t>& y, int frame) {
    y.assign(size_t(kW) * kH, 0);
    std::vector<uint8_t> t(size_t(kTile) * kTile);
    for (int row = 0; row < kTY; ++row)
        for (int eye = 0; eye < kEyes; ++eye)
            for (int col = 0; col < kTX; ++col) {
                const bool panel = (col == kPanelCol && row == kPanelRow);
                if (panel)
                    nxrc::synth::text_glyphs(t.data(), kTile,
                                             uint32_t(7 + frame));
                else
                    nxrc::synth::noise_texture(
                        t.data(), kTile,
                        uint32_t(1 + col + row * 31 + frame * 101));
                const int x0 = eye * kEyeW + col * kTile, y0 = row * kTile;
                for (int r = 0; r < kTile; ++r)
                    std::memcpy(&y[size_t(y0 + r) * kW + x0],
                                &t[size_t(r) * kTile], kTile);
            }
}

void make_chroma(std::vector<uint8_t>& u, std::vector<uint8_t>& v) {
    u.assign(size_t(kW) * kH, 128);
    v.assign(size_t(kW) * kH, 128);
}

nxrc::EncDriveConfig drive_config() {
    nxrc::EncDriveConfig dc;
    dc.width = kEyeW;
    dc.height = kH;
    dc.eyes = kEyes;
    dc.fps = 90.0f;
    // Low enough that the ladder has to engage; the invariants are about what
    // the ladder is NOT allowed to do when it is under pressure.
    dc.bitrate_mbps = 8.0f;
    return dc;
}

nxvc_config encoder_config() {
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = kEyeW;
    cfg.height = kH;
    cfg.eyes = kEyes;
    cfg.chroma = NXVC_CHROMA_444;
    cfg.inter = 1;
    cfg.base_qp = nxrc::EncDriver::kBaseQp;
    cfg.quant_matrix = nxrc::EncDriver::kFrameMatrix;
    cfg.wm_id = 1;
    return cfg;
}

// Encode `frames` frames through the driver.  Returns the concatenated
// bitstream; `sink` sees every frame's driver state and tile records.
template <typename Sink>
std::vector<uint8_t> drive(int frames, Sink&& sink) {
    nxrc::EncDriver drv(drive_config());
    nxvc_config cfg = encoder_config();
    nxvc_status st;
    nxvc_encoder* enc = nxvc_encoder_create(&cfg, &st);
    if (!enc) {
        CHECK_MSG(false, std::string("encoder_create: ") + nxvc_status_string(st));
        return {};
    }
    std::vector<uint8_t> out(65536), buf(size_t(kW) * kH * 6);
    size_t hl = 0;
    nxvc_encoder_stream_header(enc, out.data(), out.size(), &hl);
    out.resize(hl);

    std::vector<uint8_t> y, u, v;
    make_chroma(u, v);
    for (int f = 0; f < frames; ++f) {
        make_frame(y, f);
        drv.analyse(y.data(), kW, f);
        st = nxvc_encoder_set_wm_map(enc, drv.wm_map().data(),
                                     uint32_t(drv.tile_count()));
        CHECK_EQ(int(st), int(NXVC_OK));
        nxvc_encoder_set_skip_map(enc, drv.skip_map().data(),
                                  uint32_t(drv.tile_count()));
        nxvc_image img{};
        img.plane[0] = y.data(); img.stride[0] = kW;
        img.plane[1] = u.data(); img.stride[1] = kW;
        img.plane[2] = v.data(); img.stride[2] = kW;
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(enc, &img, drv.qp_map().data(),
                                       drv.res_map().data(), buf.data(),
                                       buf.size(), &ol);
        CHECK_EQ(int(st), int(NXVC_OK));
        out.insert(out.end(), buf.begin(), buf.begin() + ol);
        uint32_t tc = 0;
        const nxvc_tile_info* ti = nxvc_encoder_tiles(enc, &tc);
        sink(f, drv, ti, tc);
        drv.feedback(ti, tc);
    }
    nxvc_encoder_destroy(enc);
    return out;
}

// --------------------------------------------------------------------------

void test_determinism() {
    rct::begin("determinism");
    auto nil = [](int, const nxrc::EncDriver&, const nxvc_tile_info*, uint32_t) {};
    const std::vector<uint8_t> a = drive(6, nil);
    const std::vector<uint8_t> b = drive(6, nil);
    CHECK_MSG(!a.empty(), "the run produced a stream");
    CHECK_EQ(a.size(), b.size());
    CHECK_MSG(a == b, "two rc-driven encodes of the same frames are byte-identical");
}

void test_panel_and_fovea() {
    rct::begin("panel and fovea");
    int panel_seen = 0, fovea_tiles = 0;
    drive(8, [&](int f, const nxrc::EncDriver& drv, const nxvc_tile_info* ti,
                 uint32_t tc) {
        const auto& fov = drv.fov();
        const auto cls = drv.classes();
        CHECK_EQ(size_t(tc), drv.tile_count());
        for (int eye = 0; eye < kEyes; ++eye) {
            const size_t p = tile_id(kPanelRow, eye, kPanelCol);
            if (cls[p] != uint8_t(nxrc::TileClass::Text)) continue;
            ++panel_seen;
            // Invariant 2.  The panel is redrawn every frame, so it is never
            // static: neither the ladder nor the scheduler may take its
            // resolution or its residual away.
            CHECK_MSG(drv.res_map()[p] == 0,
                      "text tile kept full resolution, frame " + std::to_string(f));
            CHECK_MSG(drv.skip_map()[p] == 0,
                      "changing text tile was not force-skipped, frame " +
                          std::to_string(f));
            CHECK_MSG(ti[p].res_level == 0,
                      "the encoder coded the text tile at full resolution");
        }
        // Invariant 3: nothing inside the fovea is ever withheld.  The fovea
        // is RefreshConfig::fovea_full_deg, not just the eye box.
        const float fovea_deg = drv.refresh_config().fovea_full_deg;
        for (size_t i = 0; i < drv.tile_count(); ++i) {
            if (fov.ecc_deg[i] > fovea_deg) continue;
            ++fovea_tiles;
            CHECK_MSG(drv.skip_map()[i] == 0,
                      "fovea tile " + std::to_string(i) +
                          " was not force-skipped, frame " + std::to_string(f));
        }
    });
    CHECK_MSG(panel_seen >= 8, "the panel tile classified as Text on most frames (" +
                                   std::to_string(panel_seen) + ")");
    CHECK_MSG(fovea_tiles > 0, "the map has a fovea at all");
}

void test_maps_reach_the_bitstream() {
    rct::begin("maps reach the bitstream");
    int checked = 0, res_nonzero = 0, wm_nonzero = 0;
    drive(6, [&](int f, const nxrc::EncDriver& drv, const nxvc_tile_info* ti,
                 uint32_t tc) {
        if (f == 0) return;   // frame 0 is all-intra, before any feedback
        for (uint32_t i = 0; i < tc; ++i) {
            if (ti[i].skipped) continue;   // a skipped tile carries no header
            ++checked;
            CHECK_MSG(ti[i].qp == drv.qp_map()[i],
                      "tile " + std::to_string(i) + " coded at the QP the "
                      "allocator asked for");
            CHECK_MSG(ti[i].res_level == drv.res_map()[i],
                      "tile " + std::to_string(i) + " coded at the res_level "
                      "the ladder asked for");
            CHECK_MSG(ti[i].wm_id == drv.wm_map()[i],
                      "tile " + std::to_string(i) + " coded with the weighting "
                      "matrix the ladder asked for");
            res_nonzero += ti[i].res_level ? 1 : 0;
            wm_nonzero += ti[i].wm_id != 1 ? 1 : 0;
        }
    });
    CHECK_MSG(checked > 0, "some tiles were coded");
    CHECK_MSG(res_nonzero > 0, "the foveation map actually reduced some tile's "
                               "resolution, so the check above is not vacuous");
    (void)wm_nonzero;
}

// The warped residual the encoder reports is what the driver uses as the next
// frame's complexity; a zero there would silently turn every tile static.
void test_warp_mad_is_reported() {
    rct::begin("warp_mad_q8");
    int measured = 0, unmeasured = 0;
    drive(5, [&](int f, const nxrc::EncDriver&, const nxvc_tile_info* ti,
                 uint32_t tc) {
        for (uint32_t i = 0; i < tc; ++i) {
            if (ti[i].warp_mad_q8 == NXVC_WARP_MAD_UNMEASURED) ++unmeasured;
            else ++measured;
        }
        if (f == 0)
            CHECK_MSG(unmeasured == int(tc),
                      "the first frame has no reference, so nothing is measured");
    });
    CHECK_MSG(measured > 0, "later frames report a warped residual");
}

} // namespace

int main() {
    test_determinism();
    test_panel_and_fovea();
    test_maps_reach_the_bitstream();
    test_warp_mad_is_reported();
    return rct::finish("rcenc");
}
