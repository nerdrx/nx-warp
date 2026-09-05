// NX Warp decoder, Pass B: the output stores.
//
// Everything that turns a finished tile in `sPlane` into pixels: the display
// conversion and its formats, the optional second store (`kOutSecond`), and
// the reference-ring slot.  It was the tail of reconstruct.comp's main() and
// it is a file of its own so that a SECOND kernel can call it.
//
// The second kernel is the warp predictor.  A WARP_SKIP tile's reconstruction
// is exactly the predictor's output, so Pass W can store it and Pass B need
// never run on that tile at all -- which takes a 12.3 KB WPred write and a
// 12.3 KB WPred read per tile out of the frame.  That is the change this file
// exists for; extracting it first, and proving Pass B byte-identical across
// the whole suite before anything calls it twice, is what keeps the store --
// the part the conformance vectors pin hardest -- a single implementation.
//
// It closes over nothing: `sPlane`, the push constants, the specialization
// constants and the image bindings are file scope in both kernels, and every
// value that was a local of main() is a parameter.  Scalars rather than a
// struct, per docs/ADRENO-RULES.md rule 2.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXVW_PASSB_STORE_GLSL
#define NXVW_PASSB_STORE_GLSL

void nxvwStoreTile(int tid, int tile, int tileX, int tileY, int res_level,
                   int chroma444, int alpha_mode, int alphaValue,
                   int sb0, int sb1, int sb2, int sb3) {
    // ------------------------------------------------- display conversion
    // Scalars for the same reason as planeStoreBase above.
    // [REF] plane_full_extent(): a chroma plane is half size only when the
    // *stream* is 4:2:0; inside a 4:4:4 stream a 4:2:0 tile is upsampled
    // straight to 64 in one step.
    int sizeP0 = nxvw_plane_size(0, res_level, chroma444);
    int sizeP1 = nxvw_plane_size(1, res_level, chroma444);
    int sizeP2 = nxvw_plane_size(2, res_level, chroma444);
    int sizeP3 = nxvw_plane_size(3, res_level, chroma444);
    int chromaFull = pc.p.chroma420 != 0 ? 32 : 64;
    int fullP0 = 64, fullP1 = chromaFull, fullP2 = chromaFull, fullP3 = 64;

    int ox = tileX * 64, oy = tileY * 64;

    // Both stores run off the same reconstruction; `kOutSecond` is a
    // specialization constant, so a pipeline that does not want one compiles
    // to exactly the single-store kernel it did before.
    for (int store_pass = 0; store_pass < 2; ++store_pass) {
    int fmt = store_pass == 0 ? kOutFormat : kOutSecond;
    if (fmt == kOutNone) break;

    // ---------------------------------- two-plane 4:2:0 YCbCr passthrough
    // No colour transform, no chroma upsampling: the planes go out exactly as
    // the stream carries them.  Luma is written at full resolution, Cb/Cr
    // interleaved at half.  Requires a 4:2:0 stream and colour_transform none.
    if (fmt == kOutYcbcr420) {
        for (int k = 0; k < 16; ++k) {
            int idx = k * 256 + tid;
            int x = idx & 63, y = idx >> 6;
            int gx = ox + x, gy = oy + y;
            if (gx >= pc.p.imageW || gy >= pc.p.imageH) continue;
            int Y = clamp(planeAtFull(sb0, sizeP0, 64, x, y), 0, 255);
            if (kUnormStore != 0)
                imageStore(uOutLumaN, ivec2(gx, gy),
                           vec4(nxvw_unorm8(Y), 0.0, 0.0, 0.0));
            else
                imageStore(uOutLuma, ivec2(gx, gy), uvec4(uint(Y), 0u, 0u, 0u));
        }
        int cw = (pc.p.imageW + 1) >> 1, ch = (pc.p.imageH + 1) >> 1;
        int cox = tileX * 32, coy = tileY * 32;
        for (int k = 0; k < 4; ++k) {
            int idx = k * 256 + tid;
            int x = idx & 31, y = idx >> 5;
            int gx = cox + x, gy = coy + y;
            if (gx >= cw || gy >= ch) continue;
            int Cb = clamp(planeAtFull(sb1, sizeP1, 32, x, y), 0, 255);
            int Cr = clamp(planeAtFull(sb2, sizeP2, 32, x, y), 0, 255);
            if (kUnormStore != 0)
                imageStore(uOutCbCrN, ivec2(gx, gy),
                           vec4(nxvw_unorm8(Cb), nxvw_unorm8(Cr), 0.0, 0.0));
            else
                imageStore(uOutCbCr, ivec2(gx, gy), uvec4(uint(Cb), uint(Cr), 0u, 0u));
        }
        continue;
    }

    for (int k = 0; k < 16; ++k) {
        int idx = k * 256 + tid;
        int x = idx & 63, y = idx >> 6;
        int gx = ox + x, gy = oy + y;
        if (gx >= pc.p.imageW || gy >= pc.p.imageH) continue;

        int P0 = planeAtDisplay(sb0, sizeP0, fullP0, x, y);
        int P1 = planeAtDisplay(sb1, sizeP1, fullP1, x, y);
        int P2 = planeAtDisplay(sb2, sizeP2, fullP2, x, y);

        int R, G, B;
        if (pc.p.colorTransform == kCtYCoCgR) {
            // [REF] nxvc_ycocgr_inverse.
            int Y = P0;
            int Co = P1 - kDcOffsetChromaCT;
            int Cg = P2 - kDcOffsetChromaCT;
            int t = Y - (Cg >> 1);
            G = Cg + t;
            B = t - (Co >> 1);
            R = B + Co;
        } else {
            R = P0; G = P1; B = P2;
        }
        R = clamp(R, 0, 255);
        G = clamp(G, 0, 255);
        B = clamp(B, 0, 255);

        int A = 255;
        if (alpha_mode == kAlphaConstant) {
            A = alphaValue;
        } else if (alpha_mode == kAlphaCoded) {
            A = clamp(planeAtDisplay(sb3, sizeP3, fullP3, x, y),
                      0, 255);
        }

        if (fmt == kOutRgb10A2) {
            // 8-bit samples replicated into 10 bits; see syntax_constants.h.
            uvec4 v = uvec4(uint((R << 2) | (R >> 6)), uint((G << 2) | (G >> 6)),
                            uint((B << 2) | (B >> 6)), uint(A >> 6));
            imageStore(uOutRgb10a2, ivec2(gx, gy), v);
        } else if (kUnormStore != 0) {
            imageStore(uOutRgba8N, ivec2(gx, gy),
                       vec4(nxvw_unorm8(R), nxvw_unorm8(G), nxvw_unorm8(B),
                            nxvw_unorm8(A)));
        } else {
            imageStore(uOutRgba8, ivec2(gx, gy),
                       uvec4(uint(R), uint(G), uint(B), uint(A)));
        }
    }
    }  // store_pass

    // [inter] The reference-ring slot is a second store of the same samples,
    // in the coded domain rather than the display one.
    // vk/decoder/inter/inter_hook.glsl.
    nxvwRefRingStore(tid, tile, tileX, tileY, res_level, chroma444, alpha_mode,
                     alphaValue, sb0,
                     sb1, sb2, sb3);
}

#endif  // NXVW_PASSB_STORE_GLSL
