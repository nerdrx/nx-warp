// Specialized kernel. The CPU selector proves every dispatched tile is
// R2 coarse/fine PLANAR with zero slopes, CT_NONE 8-bit 4:2:0, no alpha, res_level 0.
// Uses the production image store and fuses the identical packed ring write
// into reconstruction; the ordinary second ring-store walk is disabled.
int flatDc(int cb, int byteIndex, int qp) {
    uint word = uPlanar.w[cb + (byteIndex >> 2)];
    int c = int(word << (24 - (byteIndex & 3) * 8)) >> 24;
    return clamp(128 + dequant(c, dequantStep(nxvw_dc_qp(qp), kFlatWeight)), 0, 255);
}

int flatSample(int mapBase, int cellShift, int grid,
               int x, int y, int a, int b) {
    int cell = (y >> cellShift) * grid + (x >> cellShift);
    uint map = uPlanar.w[mapBase + (cell >> 5)];
    return ((map >> (cell & 31)) & 1u) == 0u ? a : b;
}

void main() {
    int tid = int(gl_LocalInvocationID.x);
    int tile = int(uOrder.i[gl_WorkGroupID.x]);
    NxvwTileRec rec = uTiles.rec[tile];
    int qp = clamp(pc.p.baseQp + nxvw_rec_qp_delta(rec.w1), 0, 63);
    int base = tile * kPlanarUintsPerTile;
    int cb = base + kPlanarHeaderUints + kPlanarMapUints;
    bool fine = (uPlanar.w[base] & 8u) != 0u;
    nxvwIsInterTile = false;

    // Compact center output only needs the retained native samples.  Keep
    // this before the shared reconstruction and store walk: flat PLANAR
    // tiles are already represented by two DC values and a cell map, so the
    // discarded 15/16 (or 3/4) samples need never touch sPlane.
    if (kCompactCentre != 0) {
        int tileX = tile % pc.p.tilesX;
        int tileY = tile / pc.p.tilesX;
        int cols = kCompactCentre == 2 ? 42 : 34, centreCols = kCompactCentre == 2 ? 10 : 8;
        int centre0 = (cols - centreCols) / 2, packedCentre = centreCols * 64;
        int packedEye = packedCentre + (cols * 64 - packedCentre) / 4;
        int eye = tileX / cols, localX = tileX - eye * cols;
        int stepX = localX >= centre0 && localX < centre0 + centreCols ? 1 : 4;
        int stepY = tileY >= centre0 && tileY < centre0 + centreCols ? 1 : 4;
        int packedX = eye * packedEye + (localX < centre0 ? localX * 16 :
                      localX < centre0 + centreCols ? centre0 * 16 + (localX - centre0) * 64 :
                      centre0 * 16 + packedCentre + (localX - centre0 - centreCols) * 16);
        int packedY = tileY < centre0 ? tileY * 16 :
                      tileY < centre0 + centreCols ? centre0 * 16 + (tileY - centre0) * 64 :
                      centre0 * 16 + packedCentre + (tileY - centre0 - centreCols) * 16;
        int grid = 1 << (fine ? 4 : 3);
        int cellShiftY = 6 - (fine ? 4 : 3);
        int cellShiftC = 5 - (fine ? 4 : 3);
        int mapBase = base + 1;
        int ay = flatDc(cb, 0, qp), by = flatDc(cb, 9, qp);
        int ac = flatDc(cb, 3, clamp(qp + pc.p.chromaQpOff, 0, 63));
        int bc = flatDc(cb, 12, clamp(qp + pc.p.chromaQpOff, 0, 63));
        int ar = flatDc(cb, 6, clamp(qp + pc.p.chromaQpOff, 0, 63));
        int br = flatDc(cb, 15, clamp(qp + pc.p.chromaQpOff, 0, 63));

        int widthY = 64 / stepX, heightY = 64 / stepY;
        for (int idx = tid; idx < widthY * heightY; idx +=
#ifdef NXVW_COMPACT_FLAT
             64
#else
             256
#endif
             ) {
            int px = idx & (widthY - 1), py = idx >> (stepX == 1 ? 6 : 4);
            int x = px * stepX + (stepX == 4 ? 1 : 0);
            int y = py * stepY + (stepY == 4 ? 1 : 0);
            int value = clamp(flatSample(mapBase, cellShiftY, grid,
                                         x, y, ay, by), 0, 255);
            ivec2 dst = ivec2(packedX + px, packedY + py);
            if (kUnormStore != 0) imageStore(uOutLumaN, dst,
                                              vec4(nxvw_unorm8(value), 0, 0, 0));
            else imageStore(uOutLuma, dst, uvec4(uint(value), 0, 0, 0));
        }
        int widthC = 32 / stepX, heightC = 32 / stepY;
        for (int idx = tid; idx < widthC * heightC; idx +=
#ifdef NXVW_COMPACT_FLAT
             64
#else
             256
#endif
             ) {
            int px = idx & (widthC - 1), py = idx >> (stepX == 1 ? 5 : 3);
            int x = px * stepX + (stepX == 4 ? 1 : 0);
            int y = py * stepY + (stepY == 4 ? 1 : 0);
            int cbv = clamp(flatSample(mapBase, cellShiftC, grid,
                                       x, y, ac, bc), 0, 255);
            int crv = clamp(flatSample(mapBase,
                                       cellShiftC, grid, x, y, ar, br), 0, 255);
            ivec2 dst = ivec2(packedX / 2 + px, packedY / 2 + py);
            if (kUnormStore != 0) imageStore(uOutCbCrN, dst,
                                              vec4(nxvw_unorm8(cbv), nxvw_unorm8(crv), 0, 0));
            else imageStore(uOutCbCr, dst, uvec4(uint(cbv), uint(crv), 0, 0));
        }
        return;
    }

    for (int plane = 0; plane < 3; ++plane) {
        int lg = plane == 0 ? 6 : 5;
        int size = 1 << lg;
        int shift = lg - (fine ? 4 : 3);
        int q = plane == 0 ? qp : clamp(qp + pc.p.chromaQpOff, 0, 63);
        int a = flatDc(cb, plane * 3, q);
        int b = flatDc(cb, 9 + plane * 3, q);
        int store = plane == 0 ? 0 : plane == 1 ? pc.p.planeWords0
                            : pc.p.planeWords0 + pc.p.planeWords1;
        int cols = int(uWarpHdr.w[NXVW_WARP_HDR_RING + 2]);
        int tileX = tile % pc.p.tilesX;
        int eye = tileX / cols;
        int cx = tileX - eye * cols;
        int pw = int(uWarpHdr.w[NXVW_WARP_HDR_RING + 12 + plane]);
        int stride = int(uWarpHdr.w[NXVW_WARP_HDR_RING + 8 + plane]);
        int ringBase = int(uWarpHdr.w[NXVW_WARP_HDR_RING + 3]) *
                       int(uWarpHdr.w[NXVW_WARP_HDR_RING]) +
                       int(uWarpHdr.w[NXVW_WARP_HDR_RING + 4 + plane]);
        int ox = eye * pw + cx * size;
        int oy = (tile / pc.p.tilesX) * size;
        for (int w = tid; w < (size * size / 2); w += 256) {
            int x = (w * 2) & (size - 1), y = (w * 2) >> lg;
            int grid = 1 << (fine ? 4 : 3);
            int cell = (y >> shift) * grid + (x >> shift);
            uint map = uPlanar.w[base + 1 + (cell >> 5)];
            int value = ((map >> (cell & 31)) & 1u) == 0u ? a : b;
            // A packed pair cannot cross an aligned 2-, 4- or 8-pixel cell boundary.
            uint packed = pack16x2(value, value);
            sPlane[store + w] = packed;
            if (kRefRingStore != 0) {
                uint dest = uint(ringBase + (oy + y) * stride + ox + x);
                uRingOut.w[dest >> 1u] = packed;
            }
        }
    }
    barrier();
    nxvwStoreTile(tid, tile, tile % pc.p.tilesX, tile / pc.p.tilesX,
                  0, 0, 0, 0, 0, pc.p.planeWords0,
                  pc.p.planeWords0 + pc.p.planeWords1,
                  pc.p.planeWords0 + pc.p.planeWords1 + pc.p.planeWords2);
}
