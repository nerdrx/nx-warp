// CPU model of rans_decode.comp - see passA_model.h.
//
// The layout below mirrors the shader one section at a time:
//   shared memory   -> struct Shared
//   per-thread vars -> struct Thread (the shader's g_* registers)
//   main()          -> run_group(), with an explicit loop over lid where the
//                      shader relies on 64 hardware threads.
//
// Barriers are no-ops here because run_group() executes each phase for all
// 64 threads before moving to the next; that is exactly what barrier() buys.
#include "passA_model.h"

#include <cstring>

namespace nxwarp_passA {
namespace {

// ---------------------------------------------------------------------------
// Shared memory
// ---------------------------------------------------------------------------
struct Shared {
    uint32_t cum[kNumTableSets * kNumCtx * kNumSym];
    uint32_t scan[kNumScans * kScanStride];
    int np[8];
    int nb[8 * kMaxPlanes];
    int unit_base[8 * (kMaxPlanes + 1)];
    int coef_base[8 * kMaxPlanes];
    int nunits[8];
    int tskip[8];
    uint32_t tabbase[8];
    uint32_t pay[8];
    uint32_t end[8];
    uint32_t ok[8];
    uint32_t renorm[8];
    uint32_t any;
};

// ---------------------------------------------------------------------------
// Per-thread registers (the shader's g_* globals)
// ---------------------------------------------------------------------------
struct Thread {
    uint32_t state;
    uint32_t pos;
    uint32_t end;

    int phase;
    int ui;
    int stride;
    int nunits;
    int last, pos_sp, prev_class, last_cls, mag;
    int esc_j, esc_bits, esc_done;
    uint32_t esc_acc;
    int u_ncoef, u_scan, u_ctx_cbf, u_ctx_last, u_coef;
    int slot;
    uint32_t coef_off;
    uint32_t cbf_off;

    // main()-local values that survive across the round loop
    uint32_t hdr_off, bs_len;
    bool valid, live;
    uint32_t tile;
};

constexpr int kPhCbf = 0, kPhLast = 1, kPhLastRaw = 2, kPhLevel = 3;
constexpr int kPhEscPrefix = 4, kPhEscSuffix = 5, kPhSign = 6, kPhDone = 7;

// ---------------------------------------------------------------------------
// Bitstream access
// ---------------------------------------------------------------------------
struct Ctx {
    const Inputs *in;
    const Outputs *out;
    Shared sh;
    Thread th[kWorkgroupSize];
};

uint32_t load_byte(const Ctx &c, uint32_t byte_off) {
    // Matches the shader's uint-addressed load: out-of-range words read 0.
    uint32_t w = 0;
    size_t wi = byte_off >> 2u;
    if ((wi + 1) * 4 <= c.in->bits_size)
        std::memcpy(&w, c.in->bits + wi * 4, 4);
    else if (wi * 4 < c.in->bits_size)
        std::memcpy(&w, c.in->bits + wi * 4, c.in->bits_size - wi * 4);
    return (w >> ((byte_off & 3u) * 8u)) & 0xffu;
}

uint32_t load_u32le(const Ctx &c, uint32_t o) {
    return load_byte(c, o) | (load_byte(c, o + 1) << 8) |
           (load_byte(c, o + 2) << 16) | (load_byte(c, o + 3) << 24);
}

uint32_t load_u16be(const Ctx &c, uint32_t o) {
    return (load_byte(c, o) << 8) | load_byte(c, o + 1);
}

int scan_index(const Ctx &c, int scan_id, int p) {
    return int(c.sh.scan[uint32_t(scan_id * kScanStride + p)]);
}

void fail(Ctx &c, Thread &g, uint32_t code) {
    if (c.sh.ok[g.slot] == kStatusOk) c.sh.ok[g.slot] = code;
    g.phase = kPhDone;
}

// ---------------------------------------------------------------------------
// Unit lookup - TileCoder::build_units() order
// ---------------------------------------------------------------------------
void begin_unit(Ctx &c, Thread &g) {
    int base = g.slot * (kMaxPlanes + 1);
    int p = 0;
    for (int q = 1; q < c.sh.np[g.slot]; ++q)
        if (c.sh.unit_base[base + q] <= g.ui) p = q;

    int k = g.ui - c.sh.unit_base[base + p];
    int nb = c.sh.nb[g.slot * kMaxPlanes + p];
    int ndc = nb * nb;
    int chroma = (p == 1 || p == 2) ? 1 : 0;
    g.u_ctx_cbf = chroma != 0 ? kCtxCbfChroma : kCtxCbfLuma;
    g.u_ctx_last = chroma != 0 ? kCtxLastChroma : kCtxLastLuma;
    if (k == 0) {
        g.u_ncoef = ndc;
        g.u_scan = nxs_scan_id(ndc, 0);
        g.u_coef = c.sh.coef_base[g.slot * kMaxPlanes + p];
    } else {
        g.u_ncoef = kCoefPerBlock;
        g.u_scan = nxs_scan_id(kCoefPerBlock, c.sh.tskip[g.slot]);
        g.u_coef = c.sh.coef_base[g.slot * kMaxPlanes + p] + ndc +
                   (k - 1) * kCoefPerBlock;
    }
    g.phase = kPhCbf;
}

void begin_levels(Thread &g) {
    g.pos_sp = g.last;
    g.prev_class = 0;
    g.phase = kPhLevel;
}

void advance_pos(Ctx &c, Thread &g) {
    g.prev_class = nxs_level_class(g.mag < 0 ? -g.mag : g.mag);
    if (g.pos_sp == 0) {
        g.ui += g.stride;
        if (g.ui >= g.nunits) g.phase = kPhDone; else begin_unit(c, g);
    } else {
        --g.pos_sp;
        g.phase = kPhLevel;
    }
}

void lane_init(Ctx &c, Thread &g, int lane) {
    g.stride = int(kLanes);
    g.ui = lane;
    g.last = 0; g.pos_sp = 0; g.prev_class = 0; g.last_cls = 0; g.mag = 0;
    g.esc_j = 0; g.esc_bits = 0; g.esc_done = 0; g.esc_acc = 0u;
    if (g.ui >= g.nunits) { g.phase = kPhDone; return; }
    begin_unit(c, g);
}

bool lane_next(const Thread &g, int &out_kind, int &out_arg) {
    out_kind = 0; out_arg = 0;
    if (g.phase == kPhDone) return false;
    if (g.phase == kPhCbf)  { out_kind = 0; out_arg = g.u_ctx_cbf;  return true; }
    if (g.phase == kPhLast) { out_kind = 0; out_arg = g.u_ctx_last; return true; }
    if (g.phase == kPhLastRaw) {
        out_kind = 1; out_arg = kLastRawBits[g.last_cls]; return true;
    }
    if (g.phase == kPhLevel) {
        out_kind = 0; out_arg = nxs_level_ctx(g.pos_sp, g.prev_class);
        return true;
    }
    if (g.phase == kPhEscPrefix) { out_kind = 1; out_arg = 1; return true; }
    if (g.phase == kPhEscSuffix) {
        int nchunks = (g.esc_bits + kEscChunkBits - 1) / kEscChunkBits;
        int chunk = g.esc_done == 0
                        ? g.esc_bits - kEscChunkBits * (nchunks - 1)
                        : kEscChunkBits;
        out_kind = 1; out_arg = chunk; return true;
    }
    out_kind = 1; out_arg = 1; return true;  // kPhSign
}

void store_coef(Ctx &c, Thread &g, int value) {
    c.out->coef[g.coef_off + uint32_t(g.u_coef + scan_index(c, g.u_scan, g.pos_sp))] =
        int16_t(value);
}

void lane_feed(Ctx &c, Thread &g, uint32_t v) {
    if (g.phase == kPhCbf) {
        if (v > 1u) { fail(c, g, kStatusBadSymbol); return; }
        if (v == 0u) {
            g.ui += g.stride;
            if (g.ui >= g.nunits) g.phase = kPhDone; else begin_unit(c, g);
            return;
        }
        c.out->cbf[g.cbf_off + uint32_t(g.ui) / 32u] |= 1u << (uint32_t(g.ui) & 31u);
        if (g.u_ncoef == 1) { g.last = 0; begin_levels(g); return; }
        g.phase = kPhLast;
        return;
    }
    if (g.phase == kPhLast) {
        if (v > uint32_t(kLastMaxClass)) { fail(c, g, kStatusBadSymbol); return; }
        g.last_cls = int(v);
        int base = kLastBase[g.last_cls];
        if (base >= g.u_ncoef) { fail(c, g, kStatusBadSymbol); return; }
        if (kLastRawBits[g.last_cls] > 0) { g.phase = kPhLastRaw; return; }
        g.last = base;
        begin_levels(g);
        return;
    }
    if (g.phase == kPhLastRaw) {
        g.last = kLastBase[g.last_cls] + int(v);
        if (g.last >= g.u_ncoef) { fail(c, g, kStatusBadSymbol); return; }
        begin_levels(g);
        return;
    }
    if (g.phase == kPhLevel) {
        if (v == uint32_t(kEscSym)) { g.esc_j = 0; g.phase = kPhEscPrefix; return; }
        g.mag = int(v);
        if (g.mag == 0) {
            if (g.pos_sp == g.last) { fail(c, g, kStatusBadSymbol); return; }
            store_coef(c, g, 0);
            g.mag = 0;
            advance_pos(c, g);
            return;
        }
        g.phase = kPhSign;
        return;
    }
    if (g.phase == kPhEscPrefix) {
        if (v > 1u) { fail(c, g, kStatusBadSymbol); return; }
        if (v == 1u) {
            if (++g.esc_j > kEscMaxPrefix) { fail(c, g, kStatusBadSymbol); return; }
            return;
        }
        g.esc_bits = g.esc_j + kEscOrder;
        g.esc_done = 0;
        g.esc_acc = 0u;
        g.phase = kPhEscSuffix;
        return;
    }
    if (g.phase == kPhEscSuffix) {
        int nchunks = (g.esc_bits + kEscChunkBits - 1) / kEscChunkBits;
        int chunk = g.esc_done == 0
                        ? g.esc_bits - kEscChunkBits * (nchunks - 1)
                        : kEscChunkBits;
        g.esc_acc = (g.esc_acc << uint32_t(chunk)) | v;
        g.esc_done += chunk;
        if (g.esc_done < g.esc_bits) return;
        uint32_t n = (1u << uint32_t(g.esc_bits)) + g.esc_acc;
        uint32_t val = n - (1u << uint32_t(kEscOrder));
        if (val > uint32_t(kEscMaxValue)) { fail(c, g, kStatusBadSymbol); return; }
        g.mag = int(val) + kEscSym;
        g.phase = kPhSign;
        return;
    }
    // kPhSign
    if (v > 1u) { fail(c, g, kStatusBadSymbol); return; }
    store_coef(c, g, v != 0u ? -g.mag : g.mag);
    advance_pos(c, g);
}

// ---------------------------------------------------------------------------
// Symbol lookup - same branchless search as the shader
// ---------------------------------------------------------------------------
uint32_t cum_at(const Ctx &c, uint32_t tabbase, int ctx, int s) {
    return c.sh.cum[tabbase + uint32_t(ctx * kNumSym + s)];
}

void decode_symbol(const Ctx &c, uint32_t tabbase, int ctx, uint32_t slot,
                   uint32_t &sym, uint32_t &f, uint32_t &cu) {
    int s = 0;
    if (cum_at(c, tabbase, ctx, 8) <= slot) s = 8;
    if (cum_at(c, tabbase, ctx, s + 4) <= slot) s += 4;
    if (cum_at(c, tabbase, ctx, s + 2) <= slot) s += 2;
    if (cum_at(c, tabbase, ctx, s + 1) <= slot) s += 1;
    cu = cum_at(c, tabbase, ctx, s);
    uint32_t hi = (s == kNumSym - 1) ? kProbScale : cum_at(c, tabbase, ctx, s + 1);
    f = hi - cu;
    sym = uint32_t(s);
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------
void run_group(Ctx &c, uint32_t workgroup_id) {
    Shared &sh = c.sh;

    // --- shared table load -------------------------------------------------
    for (uint32_t i = 0; i < uint32_t(kNumTableSets * kNumCtx * kNumSym); ++i)
        sh.cum[i] = c.in->tables[i];
    for (uint32_t i = 0; i < uint32_t(kNumScans * kScanStride); ++i) {
        uint32_t t = i / uint32_t(kScanStride);
        uint32_t p = i % uint32_t(kScanStride);
        uint32_t v = p;
        if (t == uint32_t(kScanZigzag8)) v = uint32_t(kZigzag8[p]);
        else if (t == uint32_t(kScanZigzag4)) v = (p < 16u) ? uint32_t(kZigzag4[p]) : p;
        sh.scan[i] = v;
    }
    for (int s = 0; s < 8; ++s) { sh.ok[s] = kStatusOk; sh.renorm[s] = 0u; }
    sh.any = 0u;

    for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid) {
        Thread &g = c.th[lid];
        g = Thread{};
        int slot = int(lid >> 3u);
        g.slot = slot;
        g.tile = workgroup_id * kTilesPerGroup + uint32_t(slot);
        g.valid = g.tile < c.in->num_tiles;
        g.hdr_off = 0; g.bs_len = 0;
        if (g.valid) {
            const TileDesc &d = c.in->tiles[g.tile];
            g.hdr_off = d.bits_offset;
            g.bs_len = d.bits_length;
            g.coef_off = g.tile * c.in->coef_stride;
            g.cbf_off = g.tile * c.in->cbf_words;
        }
    }

    // --- tile header (lane 0 of each slot) ---------------------------------
    for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid) {
        Thread &g = c.th[lid];
        int slot = int(lid >> 3u), lane = int(lid & 7u);
        if (!(g.valid && lane == 0)) continue;
        uint32_t w0 = load_u32le(c, g.hdr_off);
        uint32_t w1 = load_u32le(c, g.hdr_off + 4u);
        int res_level = int((w1 >> kThResLevelShift) & kThResLevelMask);
        int chroma444 = int((w1 >> kThChroma444Shift) & kThChroma444Mask);
        int alpha_mode = int((w1 >> kThAlphaModeShift) & kThAlphaModeMask);
        uint32_t table_set = (w1 >> kThTableSetShift) & kThTableSetMask;
        uint32_t nsub_log2 = (w1 >> kThNsubLog2Shift) & kThNsubLog2Mask;
        sh.tskip[slot] = int((w1 >> kThTskipShift) & kThTskipMask);
        sh.tabbase[slot] = table_set * uint32_t(kNumCtx * kNumSym);

        uint32_t pay = nxs_tile_payload_offset(g.hdr_off, w1);
        uint32_t paylen = (w0 >> kThPayloadLenShift) & kThPayloadLenMask;
        sh.pay[slot] = pay;
        uint32_t tile_end = g.hdr_off + g.bs_len;
        uint32_t pay_end = pay + paylen;
        sh.end[slot] = pay_end < tile_end ? pay_end : tile_end;

        if (nxs_tile_header_reserved_bad(w0, w1) != 0 ||
            nsub_log2 != kLanesLog2 ||
            pay + kLanes * kInitBytesPerLane > sh.end[slot]) {
            sh.ok[slot] = kStatusBadHeader;
        }

        int np = nxs_coded_planes(int(c.in->frame_nplanes), alpha_mode);
        sh.np[slot] = np;
        int ub = 0, cb = 0;
        for (int p = 0; p < kMaxPlanes; ++p) {
            sh.unit_base[slot * (kMaxPlanes + 1) + p] = ub;
            sh.coef_base[slot * kMaxPlanes + p] = cb;
            if (p < np) {
                int nb = nxs_plane_size(p, res_level, chroma444) / kBlockSize;
                sh.nb[slot * kMaxPlanes + p] = nb;
                int ndc = nb * nb;
                ub += kUnitsPerPlaneExtra + ndc;
                cb += ndc + ndc * kCoefPerBlock;
            } else {
                sh.nb[slot * kMaxPlanes + p] = 0;
            }
        }
        sh.unit_base[slot * (kMaxPlanes + 1) + kMaxPlanes] = ub;
        sh.nunits[slot] = ub;
        if (ub < int(kLanes)) sh.ok[slot] = kStatusBadHeader;
    }

    // --- zero the coefficient region and CBF words -------------------------
    for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid) {
        Thread &g = c.th[lid];
        int lane = int(lid & 7u);
        if (!g.valid) continue;
        for (uint32_t i = uint32_t(lane); i < c.in->coef_stride; i += kLanes)
            c.out->coef[g.coef_off + i] = 0;
        for (uint32_t i = uint32_t(lane); i < c.in->cbf_words; i += kLanes)
            c.out->cbf[g.cbf_off + i] = 0u;
    }

    // --- rANS init ---------------------------------------------------------
    for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid) {
        Thread &g = c.th[lid];
        int slot = int(lid >> 3u), lane = int(lid & 7u);
        g.live = g.valid && sh.ok[slot] == kStatusOk;
        g.end = g.valid ? sh.end[g.slot] : 0u;
        g.pos = (g.valid ? sh.pay[g.slot] : 0u) + kLanes * kInitBytesPerLane;
        g.nunits = g.valid ? sh.nunits[slot] : 0;
        g.phase = kPhDone;
        if (g.live) {
            g.state = load_u32le(
                c, sh.pay[g.slot] + uint32_t(lane) * kInitBytesPerLane);
            if (g.state < kRansL) sh.ok[slot] = kStatusBadHeader;
            lane_init(c, g, lane);
        }
    }
    for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid) {
        Thread &g = c.th[lid];
        g.live = g.live && sh.ok[g.slot] == kStatusOk;
        if (!g.live) g.phase = kPhDone;
    }

    // --- scheduling rounds --------------------------------------------------
    uint32_t round = 0;
    for (;;) {
        int kind[kWorkgroupSize], arg[kWorkgroupSize];
        bool has_op[kWorkgroupSize], needs[kWorkgroupSize];
        uint32_t value[kWorkgroupSize], xs[kWorkgroupSize];

        for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid) {
            Thread &g = c.th[lid];
            kind[lid] = 0; arg[lid] = 0;
            has_op[lid] = g.live && lane_next(g, kind[lid], arg[lid]);
            value[lid] = 0;
            xs[lid] = g.state;
            needs[lid] = false;
            if (has_op[lid]) {
                uint32_t slot1024 = xs[lid] & kProbMask;
                uint32_t f, cu;
                if (kind[lid] == 0) {
                    uint32_t sym;
                    decode_symbol(c, sh.tabbase[g.slot], arg[lid], slot1024, sym, f, cu);
                    value[lid] = sym;
                } else {
                    uint32_t shb = kProbBits - uint32_t(arg[lid]);
                    value[lid] = slot1024 >> shb;
                    f = 1u << shb;
                    cu = value[lid] << shb;
                }
                xs[lid] = f * (xs[lid] >> kProbBits) + slot1024 - cu;
                needs[lid] = xs[lid] < kRansL;
            }
        }

        // --- shared read pointer -------------------------------------------
        for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid) {
            Thread &g = c.th[lid];
            if (c.in->read_ptr_mode != kReadPtrBallot && needs[lid])
                sh.renorm[g.slot] |= 1u << (lid & 7u);
            if (has_op[lid]) sh.any = 1u;
        }

        for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid) {
            Thread &g = c.th[lid];
            int slot = int(lid >> 3u), lane = int(lid & 7u);
            uint32_t prefix = 0, total = 0;
            if (c.in->read_ptr_mode == kReadPtrBallot) {
                // Emulated 8-lane cluster ballot: the shader's
                // subgroupBallot(needs) & cluster_mask.
                uint32_t b = 0;
                for (int l = 0; l < int(kLanes); ++l)
                    if (needs[uint32_t(slot * 8 + l)]) b |= 1u << l;
                prefix = uint32_t(__builtin_popcount(b & ((1u << lane) - 1u)));
                total = uint32_t(__builtin_popcount(b));
            } else {
                uint32_t m = sh.renorm[slot];
                prefix = uint32_t(__builtin_popcount(m & ((1u << lane) - 1u)));
                total = uint32_t(__builtin_popcount(m));
            }

            if (needs[lid]) {
                uint32_t at = g.pos + prefix * kRenormBytes;
                if (at + kRenormBytes > g.end) {
                    fail(c, g, kStatusTruncated);
                    has_op[lid] = false;
                } else {
                    xs[lid] = (xs[lid] << 16u) | load_u16be(c, at);
                }
            }
            g.pos += total * kRenormBytes;
        }

        for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid) {
            Thread &g = c.th[lid];
            if (has_op[lid]) {
                g.state = xs[lid];
                lane_feed(c, g, value[lid]);
            }
        }

        bool cont = (sh.any != 0u);
        for (int s = 0; s < 8; ++s) sh.renorm[s] = 0u;
        sh.any = 0u;
        if (!cont) break;
        if (++round >= kMaxRounds) {
            for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid)
                fail(c, c.th[lid], kStatusRoundOverflow);
            break;
        }
    }

    for (uint32_t lid = 0; lid < kWorkgroupSize; ++lid) {
        Thread &g = c.th[lid];
        if (g.valid && (lid & 7u) == 0) c.out->status[g.tile] = sh.ok[g.slot];
    }
}

}  // namespace

void decode(const Inputs &in, const Outputs &out) {
    Ctx c;
    c.in = &in;
    c.out = &out;
    uint32_t groups = group_count(in.num_tiles);
    for (uint32_t wg = 0; wg < groups; ++wg) run_group(c, wg);
}

}  // namespace nxwarp_passA
