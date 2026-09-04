// Test-only rANS encoder - see passA_test_encoder.h.
// Transcribed from ref/src/entropy.cpp.
#include "passA_test_encoder.h"

#include <algorithm>

namespace nxwarp_passA {
namespace test {
namespace {

// ref/src/entropy.cpp eg3_encode(): Exp-Golomb order 3.
void eg3_encode(uint32_t v, int &j, uint32_t &suffix, int &bits) {
    uint32_t n = v + (1u << kEscOrder);
    int b = 0;
    while ((n >> (b + 1)) != 0) ++b;
    j = b - kEscOrder;
    bits = b;
    suffix = n - (1u << b);
}

enum : int {
    kPhCbf = 0, kPhLast, kPhLastRaw, kPhLevel,
    kPhEscPrefix, kPhEscSuffix, kPhSign, kPhDone
};

struct Op {
    int kind;   // 0 = symbol (arg is context), 1 = bypass (arg is bit count)
    int arg;
    uint32_t value;
    int lane;
};

// Encoding-side LaneMachine.  next() derives the value from the
// coefficients; feed() advances exactly as the decoder does.
struct LaneMachine {
    const UnitInfo *units = nullptr;
    const int16_t *coef = nullptr;
    int nunits = 0, ui = 0, stride = 1;
    int phase = kPhDone;
    const UnitInfo *u = nullptr;
    int last = 0, pos = 0, prev_class = 0, last_cls = 0;
    int32_t mag = 0;
    int esc_j = 0, esc_bits = 0, esc_done = 0;

    void begin_unit() { u = &units[ui]; phase = kPhCbf; }
    void begin_levels() { pos = last; prev_class = 0; phase = kPhLevel; }

    void init(const UnitInfo *un, const int16_t *cf, int n, int lane, int nl) {
        units = un; coef = cf; nunits = n; stride = nl; ui = lane;
        if (ui >= nunits) { phase = kPhDone; return; }
        begin_unit();
    }

    int32_t coef_at(int p) const {
        return coef[u->coef_base + scan_index(u->scan_id, p)];
    }

    void advance_pos() {
        prev_class = nxs_level_class(mag < 0 ? -mag : mag);
        if (pos == 0) {
            ui += stride;
            if (ui >= nunits) phase = kPhDone; else begin_unit();
        } else {
            --pos;
            phase = kPhLevel;
        }
    }

    bool next(Op &op) {
        if (phase == kPhDone) return false;
        switch (phase) {
            case kPhCbf: {
                op.kind = 0; op.arg = u->ctx_cbf; op.value = 0;
                for (int i = 0; i < u->ncoef; ++i)
                    if (coef[u->coef_base + i] != 0) { op.value = 1; break; }
                return true;
            }
            case kPhLast: {
                op.kind = 0; op.arg = u->ctx_last;
                int lastpos = 0;
                for (int p = u->ncoef - 1; p >= 0; --p)
                    if (coef_at(p) != 0) { lastpos = p; break; }
                last = lastpos;
                op.value = uint32_t(nxs_last_class_of(lastpos));
                return true;
            }
            case kPhLastRaw: {
                op.kind = 1; op.arg = kLastRawBits[last_cls];
                op.value = uint32_t(last - kLastBase[last_cls]);
                return true;
            }
            case kPhLevel: {
                op.kind = 0; op.arg = nxs_level_ctx(pos, prev_class);
                int32_t q = coef_at(pos);
                int32_t m = q < 0 ? -q : q;
                op.value = uint32_t(m > kLevelMaxDirect ? kEscSym : m);
                return true;
            }
            case kPhEscPrefix: {
                int32_t q = coef_at(pos);
                int32_t m = q < 0 ? -q : q;
                int j, bits; uint32_t suf;
                eg3_encode(uint32_t(m - (kLevelMaxDirect + 1)), j, suf, bits);
                op.kind = 1; op.arg = 1; op.value = uint32_t(esc_j < j ? 1 : 0);
                return true;
            }
            case kPhEscSuffix: {
                int nchunks = (esc_bits + kEscChunkBits - 1) / kEscChunkBits;
                int chunk = esc_done == 0
                                ? esc_bits - kEscChunkBits * (nchunks - 1)
                                : kEscChunkBits;
                int32_t q = coef_at(pos);
                int32_t m = q < 0 ? -q : q;
                int j, bits; uint32_t suf;
                eg3_encode(uint32_t(m - (kLevelMaxDirect + 1)), j, suf, bits);
                int shift = esc_bits - esc_done - chunk;
                op.kind = 1; op.arg = chunk;
                op.value = (suf >> shift) & ((1u << chunk) - 1u);
                return true;
            }
            default: {  // kPhSign
                op.kind = 1; op.arg = 1;
                op.value = uint32_t(coef_at(pos) < 0 ? 1 : 0);
                return true;
            }
        }
    }

    bool feed(uint32_t v) {
        switch (phase) {
            case kPhCbf:
                if (v == 0) {
                    ui += stride;
                    if (ui >= nunits) phase = kPhDone; else begin_unit();
                    return true;
                }
                if (u->ncoef == 1) { last = 0; begin_levels(); return true; }
                phase = kPhLast;
                return true;
            case kPhLast: {
                if (v > uint32_t(kLastMaxClass)) return false;
                last_cls = int(v);
                int base = kLastBase[last_cls];
                if (base >= u->ncoef) return false;
                if (kLastRawBits[last_cls] > 0) { phase = kPhLastRaw; return true; }
                last = base;
                begin_levels();
                return true;
            }
            case kPhLastRaw:
                last = kLastBase[last_cls] + int(v);
                if (last >= u->ncoef) return false;
                begin_levels();
                return true;
            case kPhLevel:
                if (v == uint32_t(kEscSym)) { esc_j = 0; phase = kPhEscPrefix; return true; }
                mag = int32_t(v);
                if (mag == 0) {
                    if (pos == last) return false;
                    advance_pos();
                    return true;
                }
                phase = kPhSign;
                return true;
            case kPhEscPrefix:
                if (v == 1) { if (++esc_j > kEscMaxPrefix) return false; return true; }
                esc_bits = esc_j + kEscOrder;
                esc_done = 0;
                phase = kPhEscSuffix;
                return true;
            case kPhEscSuffix: {
                int nchunks = (esc_bits + kEscChunkBits - 1) / kEscChunkBits;
                int chunk = esc_done == 0
                                ? esc_bits - kEscChunkBits * (nchunks - 1)
                                : kEscChunkBits;
                esc_done += chunk;
                if (esc_done < esc_bits) return true;
                int32_t q = coef_at(pos);
                mag = q < 0 ? -q : q;
                phase = kPhSign;
                return true;
            }
            default:  // kPhSign
                advance_pos();
                return true;
        }
    }
};

// ref/src/entropy.cpp put16(): bytes go into a reversed buffer.
void put16(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(uint8_t(v & 0xff));
    b.push_back(uint8_t(v >> 8));
}

}  // namespace

int scan_index(int scan_id, int pos) {
    if (scan_id == kScanZigzag8) return kZigzag8[pos];
    if (scan_id == kScanZigzag4) return kZigzag4[pos];
    return pos;  // kScanRaster8 and kScanSmall are the identity
}

int build_units(const TileShape &shape, std::vector<UnitInfo> &units) {
    units.clear();
    int np = nxs_coded_planes(shape.frame_nplanes, shape.alpha_mode);
    int off = 0;
    for (int p = 0; p < np; ++p) {
        int nb = nxs_plane_size(p, shape.res_level, shape.chroma444) / kBlockSize;
        int ndc = nb * nb;
        bool chroma = (p == 1 || p == 2);
        int ccbf = chroma ? kCtxCbfChroma : kCtxCbfLuma;
        int clast = chroma ? kCtxLastChroma : kCtxLastLuma;
        units.push_back({ndc, nxs_scan_id(ndc, 0), off, ccbf, clast});
        off += ndc;
        for (int b = 0; b < ndc; ++b) {
            units.push_back({kCoefPerBlock,
                             nxs_scan_id(kCoefPerBlock, shape.tskip), off,
                             ccbf, clast});
            off += kCoefPerBlock;
        }
    }
    return off;
}

bool finalize(Tables &t) {
    for (int k = 0; k < kNumTableSets; ++k)
        for (int c = 0; c < kNumCtx; ++c) {
            uint32_t acc = 0;
            for (int s = 0; s < kNumSym; ++s) {
                if (t.freq[k][c][s] == 0) return false;
                t.cum[k][c][s] = acc;
                acc += t.freq[k][c][s];
            }
            if (acc != kProbScale) return false;
        }
    return true;
}

bool encode_tile(const TileShape &shape, const std::vector<UnitInfo> &units,
                 const int16_t *coef, const Tables &tabs,
                 std::vector<uint8_t> &out, uint64_t *op_count) {
    const int nunits = int(units.size());
    const int nlanes = int(kLanes);
    const int active = std::min(nlanes, nunits);

    // --- collect the interleaved op schedule -------------------------------
    std::vector<LaneMachine> lanes(active);
    for (int l = 0; l < active; ++l)
        lanes[l].init(units.data(), coef, nunits, l, nlanes);

    std::vector<Op> ops;
    ops.reserve(4096);
    bool any = true;
    while (any) {
        any = false;
        for (int l = 0; l < active; ++l) {
            Op op{};
            if (!lanes[l].next(op)) continue;
            any = true;
            op.lane = l;
            ops.push_back(op);
            if (!lanes[l].feed(op.value)) return false;
        }
    }

    if (op_count) *op_count = ops.size();

    // --- encode in reverse -------------------------------------------------
    std::vector<uint8_t> rev;
    rev.reserve(ops.size() * 2 + 64);
    std::vector<uint32_t> state(nlanes, kRansL);

    for (size_t i = ops.size(); i-- > 0;) {
        const Op &op = ops[i];
        uint32_t f, c;
        if (op.kind == 0) {
            f = tabs.freq[shape.table_set][op.arg][op.value];
            c = tabs.cum[shape.table_set][op.arg][op.value];
            if (f == 0) return false;
        } else {
            uint32_t k = uint32_t(op.arg);
            f = 1u << (kProbBits - k);
            c = op.value << (kProbBits - k);
        }
        uint32_t x = state[op.lane];
        if (x >= (f << 22)) {  // renormalise: keep the encoded state < 2^32
            put16(rev, x & 0xffff);
            x >>= 16;
        }
        state[op.lane] = ((x / f) << kProbBits) + (x % f) + c;
    }

    // --- flush: lane 0 first in the final stream ---------------------------
    for (int l = active - 1; l >= 0; --l) {
        uint32_t x = state[l];
        rev.push_back(uint8_t((x >> 24) & 0xff));
        rev.push_back(uint8_t((x >> 16) & 0xff));
        rev.push_back(uint8_t((x >> 8) & 0xff));
        rev.push_back(uint8_t(x & 0xff));
    }

    std::vector<uint8_t> payload(rev.rbegin(), rev.rend());
    if (payload.size() + kTileHeaderBytes > kThPayloadLenMask) return false;

    // --- 8-byte tile header ------------------------------------------------
    uint32_t w0 = (uint32_t(shape.tile_index) & kThTileIndexMask) << kThTileIndexShift;
    w0 |= (uint32_t(payload.size()) & kThPayloadLenMask) << kThPayloadLenShift;
    uint32_t w1 = 0;
    w1 |= (uint32_t(shape.res_level) & kThResLevelMask) << kThResLevelShift;
    w1 |= (uint32_t(shape.chroma444) & kThChroma444Mask) << kThChroma444Shift;
    w1 |= (uint32_t(shape.alpha_mode) & kThAlphaModeMask) << kThAlphaModeShift;
    w1 |= (uint32_t(shape.table_set) & kThTableSetMask) << kThTableSetShift;
    w1 |= (kLanesLog2 & kThNsubLog2Mask) << kThNsubLog2Shift;
    w1 |= (uint32_t(shape.tskip) & kThTskipMask) << kThTskipShift;

    out.clear();
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t((w0 >> (8 * i)) & 0xff));
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t((w1 >> (8 * i)) & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

}  // namespace test
}  // namespace nxwarp_passA
