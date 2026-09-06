// Base-layer refresh as an ATLAS refresh source -- ADR-0029 cheat 7, the
// mechanism of SYNTAX.md 13.12.9 driven by a staleness threshold instead of by
// an external scheduler.
//
// This is a HARNESS, not a codec feature: it exists so the base layer can be
// PRICED against the coded path on the same fixtures, with the base's own
// HEVC bytes counted rather than nxvc intra bytes.  The rule it implements is
// deliberately one both sides can run from state they already hold:
//
//   after every frame, every atlas position whose composed corner
//   displacement exceeds `margin` luma samples is refreshed from the base
//   picture of that frame.
//
// The tile selection comes from the codec (nxvc_*_atlas_stale_tiles), so the
// encoder and the decoder cannot disagree about it; what is duplicated here is
// only the plane shuffle.  The threshold is policy and is not in the
// bitstream: a real deployment would carry it in the stream header, and the
// pricing question is whether it is worth carrying at all.
#ifndef NXVC_TOOLS_BASE_REFRESH_H
#define NXVC_TOOLS_BASE_REFRESH_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"

struct BaseRefresh {
    std::FILE *f = nullptr;
    uint32_t W = 0, H = 0, margin = 0, ntiles = 0;
    std::vector<uint8_t> y, uv, tiles;
    uint64_t applied_total = 0, superseded_total = 0, stale_total = 0;

    bool open(const std::string &path, uint32_t w, uint32_t h, uint32_t m,
              uint32_t nt) {
        f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        W = w; H = h; margin = m; ntiles = nt;
        y.assign((size_t)W * H, 0);
        uv.assign((size_t)(W / 2) * (H / 2) * 2, 0);
        tiles.assign(ntiles, 0);
        return true;
    }

    // Read frame `n` of a raw yuv420p base decode and interleave its chroma
    // into the (Cb,Cr) pair order the patch API's CB_CR case names.
    bool load(uint32_t n) {
        if (!f) return false;
        const size_t fsz = (size_t)W * H * 3 / 2;
        if (std::fseek(f, (long)((size_t)n * fsz), SEEK_SET) != 0) return false;
        if (std::fread(y.data(), 1, y.size(), f) != y.size()) return false;
        const size_t cn = (size_t)(W / 2) * (H / 2);
        std::vector<uint8_t> cb(cn), cr(cn);
        if (std::fread(cb.data(), 1, cn, f) != cn) return false;
        if (std::fread(cr.data(), 1, cn, f) != cn) return false;
        for (size_t i = 0; i < cn; ++i) {
            uv[i * 2 + 0] = cb[i];
            uv[i * 2 + 1] = cr[i];
        }
        return true;
    }

    // `stale` and `patch` are the encoder's or the decoder's own entry points,
    // passed in so this file does not care which side it is running on.
    template <typename StaleFn, typename PatchFn>
    bool step(uint32_t frame, StaleFn stale, PatchFn patch) {
        if (!f) return true;
        uint32_t n_stale = 0;
        if (stale(margin, tiles.data(), ntiles, &n_stale) != NXVC_OK)
            return false;
        stale_total += n_stale;
        if (!n_stale) return true;
        if (!load(frame)) return false;
        nxvc_base_patch p{};
        p.plane[0] = y.data();
        p.stride[0] = (int)W;
        p.plane[1] = uv.data();
        p.stride[1] = (int)(W / 2) * 2;
        p.width = W;
        p.height = H;
        p.eye = 0;
        p.src_frame = frame;
        p.chroma_order = NXVC_BASE_CHROMA_CB_CR;
        p.tiles = tiles.data();
        p.tile_bytes = ntiles;
        uint32_t ap = 0, sup = 0;
        if (patch(&p, &ap, &sup) != NXVC_OK) return false;
        applied_total += ap;
        superseded_total += sup;
        return true;
    }

    void close() {
        if (f) std::fclose(f);
        f = nullptr;
    }
};

#endif
