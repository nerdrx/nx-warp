// Specialized kernel. The CPU selector proves every dispatched tile is
// R2/coarse PLANAR with zero slopes, CT_NONE 8-bit 4:2:0, no alpha, res_level 0.
// Uses the production image store and fuses the identical packed ring write
// into reconstruction; the ordinary second ring-store walk is disabled.
int flatDc(int cb, int byteIndex, int qp) {
    uint word = uPlanar.w[cb + (byteIndex >> 2)];
    int c = int(word << (24 - (byteIndex & 3) * 8)) >> 24;
    return clamp(128 + dequant(c, dequantStep(nxvw_dc_qp(qp), kFlatWeight)), 0, 255);
}
void main() {
    int tid = int(gl_LocalInvocationID.x);
    int tile = int(uOrder.i[gl_WorkGroupID.x]);
    NxvwTileRec rec = uTiles.rec[tile];
    int qp = clamp(pc.p.baseQp + nxvw_rec_qp_delta(rec.w1), 0, 63);
    int base = tile * kPlanarUintsPerTile;
    int cb = base + kPlanarHeaderUints + kPlanarMapUints;
    uint map0 = uPlanar.w[base + 1], map1 = uPlanar.w[base + 2];
    nxvwIsInterTile = false;
    for (int plane = 0; plane < 3; ++plane) {
        int lg = plane == 0 ? 6 : 5;
        int size = 1 << lg;
        int shift = lg - 3;
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
            int cell = (y >> shift) * 8 + (x >> shift);
            uint map = cell < 32 ? map0 : map1;
            int value = ((map >> (cell & 31)) & 1u) == 0u ? a : b;
            // A packed pair cannot cross a 4- or 8-pixel cell boundary.
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
