// NX Warp decoder, Pass B: the inter path's half of reconstruct.comp.
//
// reconstruct.comp belongs to the intra path.  The inter path touches it in
// exactly three places and every line of what those three places call lives
// here:
//
//   1. one `#include` of this file, immediately after the shared-memory
//      declarations (it uses `sPlane`, which cannot be forward declared);
//   2. the prediction hook, at the `INTER HOOK` marker in the prediction and
//      add step -- `nxvwWpredAt()` is the whole of it;
//   3. one call to `nxvwRefRingStore()` at the end of main(), after the
//      display stores.
//
// Everything else -- the predictor itself, the reference ring, the near-skip
// mean field, the parse and the host state -- is in this directory and in the
// host.  See vk/decoder/README.md, "The inter path".
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXVW_INTER_HOOK_GLSL
#define NXVW_INTER_HOOK_GLSL

#include "inter_layout.h"

// 7: 1 = this pipeline decodes inter tiles, so the prediction hook reads the
//    WPred buffer Pass W wrote.  A stream with no INTER tool bit compiles a
//    kernel in which the hook is `pred = planar` and nothing else, exactly the
//    kernel the intra decoder had before this file existed.
layout(constant_id = 7) const int kInterPred = 0;
// 8: 1 = write the tile's reconstruction into the frame's reference-ring slot
//    as well as into the display image.  It is a second store of the same
//    samples rather than a second reconstruction, which is the shape
//    `kOutSecond` already established (passB/README.md).
layout(constant_id = 8) const int kRefRingStore = 0;

// The predictor Pass W produced, i16 packed two per uint.
layout(set = 0, binding = 13, std430) readonly buffer WPredIn { uint w[]; } uWPredIn;
// The reference ring, u16 packed two per uint.  Pass B writes the slot this
// frame owns; Pass W reads the slots it does not.
layout(set = 0, binding = 14, std430) buffer RefRingOut { uint w[]; } uRingOut;
// The warp parameter buffer, for its ring-geometry header.  Pass B needs the
// geometry and nothing else out of it; the per-tile records are Pass W's.
layout(set = 0, binding = 15, std430) readonly buffer WarpHdr { uint w[]; } uWarpHdr;

// [inter] True while an INTER tile is being reconstructed.  It is set at the
// `bool intra` INTER HOOK in main() -- the same marker passB/README.md has
// always named -- and it exists for exactly one reason, below.
bool nxvwIsInterTile = false;

// [SYN] 13.3: an INTRA tile's block mean is a sample value and is clamped to
// the sample domain.  An INTER tile's is `dc_offset + a residual mean`, whose
// legal range is wider than the sample domain on BOTH sides; clamping it there
// would silently cap the DC correction the warp needs, and on a tile whose
// prediction is already near black or near white that is the correction that
// matters most.  `dequant()` has already bounded the term by 32767.
// [REF] codec.cpp reconstruct_dc_plane(), the `s.wpred.empty() ? clamp : no
// clamp` line.
int nxvwMeanClamp(int v, int lo, int hi) {
    return nxvwIsInterTile ? v : clamp(v, lo, hi);
}

// Defined below in reconstruct.comp.  A prototype rather than a copy: the
// coded -> full resample is the intra path's and the ring stores exactly what
// the display store would have stored.
int planeAtFull(int base, int size, int full, int px, int py);

// ------------------------------------------------------- the prediction hook
// One sample of the inter predictor of [SYN] 13.3: warp_tile() of the
// reference at this tile's homography and vector, box-averaged to the coded
// extent, with a near-skip tile's mean field already folded in (see
// warp_pred.comp).  Pass B adds the DC-plane correction and clamps:
//
//     pred = clamp(W + planar(M) - dc_offset, 0, maxval)
//
// which for a skipped tile, whose mean field is flat at dc_offset, is
// clamp(W) -- [REF] codec_impl.inc reconstruct_skip().
//
// Values are in [0, maxval], so the unpack needs no sign extension.
int nxvwWpredAt(int tile, int p, int size, int x, int y) {
    if (kInterPred == 0) return 0;
    int base = tile * nxvw_wpred_stride_i16(pc.p.chroma420, pc.p.alphaPresent) +
               nxvw_wpred_plane_off(p, pc.p.chroma420);
    uint e = uint(base + y * size + x);
    return int((uWPredIn.w[e >> 1u] >> ((e & 1u) * 16u)) & 0xffffu);
}

// ------------------------------------------------------ the reference ring
// [SYN] 13.2 and [REF] codec_impl.inc store_ref_tile(): the tile's
// reconstruction, in the CODED sample domain and at full tile extent, into
// the slot this frame's number names.  That is the domain and the resolution
// the predictor reads, and it is why the ring is not the display image: with
// the colour transform on, the display image is RGB and the predictor
// predicts Y/Co/Cg.
//
// Each thread writes a horizontally adjacent PAIR of samples, so every uint of
// the ring is written by exactly one thread and no two workgroups contend for
// one.  inter_layout.h's `nxvw_ring_stride()` carries the argument for why a
// tile's x origin is always even.
void nxvwRefRingStore(int tid, int tile, int tileX, int tileY, int res_level,
                      int chroma444, int alpha_mode, int alphaValue, int sb0,
                      int sb1, int sb2, int sb3) {
    if (kRefRingStore == 0) return;
    const int slotU16 = int(uWarpHdr.w[uint(NXVW_WARP_HDR_RING + 0)]);
    const int colsPerEye = int(uWarpHdr.w[uint(NXVW_WARP_HDR_RING + 2)]);
    const int curBase = int(uWarpHdr.w[uint(NXVW_WARP_HDR_RING + 3)]) * slotU16;
    const int eye = tileX / colsPerEye;
    const int cx = tileX - eye * colsPerEye;
    const int nplanes = (pc.p.alphaPresent != 0) ? 4 : 3;

    for (int p = 0; p < 4; ++p) {
        if (p >= nplanes) break;   // uniform across the workgroup
        const bool chroma = (p == 1 || p == 2);
        const int full = nxvw_inter_plane_full(p, pc.p.chroma420);
        const int size = nxvw_plane_size(p, res_level, chroma444);
        const int store = (p == 0) ? sb0 : (p == 1) ? sb1 : (p == 2) ? sb2 : sb3;
        const bool ctChroma = (pc.p.colorTransform == kCtYCoCgR) && chroma;
        const int maxval = ctChroma ? kMaxvalChromaCT : kMaxval8;
        const int pw = int(uWarpHdr.w[uint(NXVW_WARP_HDR_RING + 12 + p)]);
        const int ph = chroma ? (pc.p.chroma420 != 0 ? (pc.p.imageH + 1) >> 1
                                                     : pc.p.imageH)
                              : pc.p.imageH;
        const int planeOff = int(uWarpHdr.w[uint(NXVW_WARP_HDR_RING + 4 + p)]);
        const int stride = int(uWarpHdr.w[uint(NXVW_WARP_HDR_RING + 8 + p)]);
        // [REF] store_ref_tile(): an alpha plane the tile did not code is the
        // constant value, not a resampled plane slot.
        const bool constAlpha = (p == 3) && (alpha_mode != kAlphaCoded);
        const int cval = (alpha_mode == kAlphaConstant) ? alphaValue : 255;
        const int ox = eye * pw + cx * full;
        const int oy = tileY * full;
        const int npair = (full * full) >> 1;
        for (int i = tid; i < npair; i += 256) {
            const int e = 2 * i;
            const int y = e / full;
            const int x = e - y * full;
            const int gy = oy + y;
            const int gx = ox + x;
            if (gy >= ph) continue;
            if (gx >= eye * pw + pw) continue;
            const int v0 = constAlpha
                               ? cval
                               : clamp(planeAtFull(store, size, full, x, y), 0,
                                       maxval);
            const int v1 = constAlpha
                               ? cval
                               : clamp(planeAtFull(store, size, full, x + 1, y),
                                       0, maxval);
            const uint idx = uint(curBase + planeOff + gy * stride + gx);
            uRingOut.w[idx >> 1u] =
                (uint(v0) & 0xffffu) | (uint(v1) << 16);
        }
    }
}

#endif  // NXVW_INTER_HOOK_GLSL
