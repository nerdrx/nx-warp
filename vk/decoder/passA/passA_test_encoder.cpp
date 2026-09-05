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

// ------------------------------------------------------------ entropy-lite
// [REF] ref/src/entropy_lite.cpp BitW: MSB-first inside a byte, and every
// section padded to a byte boundary by align().
struct LiteBitW {
    std::vector<uint8_t> &b;
    uint32_t acc = 0;
    int n = 0;
    explicit LiteBitW(std::vector<uint8_t> &out) : b(out) {}
    void put(uint32_t v, int k) {
        while (k > 0) {
            int take = k > 8 ? 8 : k;
            uint32_t chunk = (v >> (k - take)) & ((1u << take) - 1u);
            acc = (acc << take) | chunk;
            n += take;
            k -= take;
            while (n >= 8) {
                b.push_back(uint8_t((acc >> (n - 8)) & 0xff));
                n -= 8;
            }
        }
        acc &= (1u << n) - 1u;
    }
    void align() {
        if (n) {
            b.push_back(uint8_t((acc << (8 - n)) & 0xff));
            n = 0;
            acc = 0;
        }
    }
};

// [REF] entropy_lite.cpp UnitFacts.
struct LiteFacts {
    int coded = 0;
    int last = 0;
    int nnz = 0;
    int param = 0;      // FIXED: the magnitude class
    int body_bits = 0;
};

// [REF] entropy.cpp mpm_of() / nonmpm_index(), over a plane's mode array.
int lite_mpm_of(const uint8_t *m, int nbx, int b) {
    int bx = b % nbx, by = b / nbx;
    int left = bx > 0 ? int(m[b - 1]) : kIntraDcPlane;
    int above = by > 0 ? int(m[b - nbx]) : kIntraDcPlane;
    return nxs_mpm(left, above);
}

int lite_nonmpm_index(int mpm, int mode) {
    int n = 0;
    for (int m = 0; m < kNumIntraModes; ++m) {
        if (m == mpm) continue;
        if (m == mode) return n;
        ++n;
    }
    return 0;
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
    // [entropy-lite] INTRA_DIR puts one mode unit between a plane's DC unit
    // and its block units ([SYN] 9.1); without it the list is v1's.
    const bool dir = (uint32_t(shape.tools) & kToolFlagIntraDir) != 0u;
    int off = 0;
    for (int p = 0; p < np; ++p) {
        int nb = nxs_plane_size(p, shape.res_level, shape.chroma444) / kBlockSize;
        int ndc = nb * nb;
        bool chroma = (p == 1 || p == 2);
        int ccbf = chroma ? kCtxCbfChroma : kCtxCbfLuma;
        int clast = chroma ? kCtxLastChroma : kCtxLastLuma;
        units.push_back({ndc, nxs_scan_id(ndc, 0), off, ccbf, clast});
        off += ndc;
        if (dir) {
            UnitInfo mu{0, kScanSmall, off, ccbf, clast};
            mu.kind = 1;
            mu.nbx = nb;
            mu.mode_base = p * int(kModesPerPlane);
            units.push_back(mu);
        }
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

// ---------------------------------------------------------------------------
// [entropy-lite] ENTROPY_LITE / FIXED, byte-identical to
// ref/src/entropy_lite.cpp lite_encode_units() for the same unit list.
// ---------------------------------------------------------------------------
bool encode_tile_lite(const TileShape &shape, const std::vector<UnitInfo> &units,
                      const int16_t *coef, const uint8_t *modes,
                      std::vector<uint8_t> &out, uint64_t *op_count) {
    const int nunits = int(units.size());
    if (nunits <= 0) return false;
    std::vector<LiteFacts> f(static_cast<size_t>(nunits));
    uint64_t ops = 0;

    // --- per-unit facts ----------------------------------------------------
    for (int i = 0; i < nunits; ++i) {
        const UnitInfo &u = units[i];
        LiteFacts &uf = f[size_t(i)];
        if (u.kind == 1) {
            uf.coded = (u.nbx * u.nbx) != 0 ? 1 : 0;
            continue;
        }
        int last = -1;
        for (int p = u.ncoef - 1; p >= 0; --p)
            if (coef[u.coef_base + scan_index(u.scan_id, p)] != 0) {
                last = p;
                break;
            }
        if (last < 0) { uf.coded = 0; continue; }
        uf.coded = 1;
        uf.last = last;
        int32_t maxa = 0;
        int nnz = 0;
        for (int p = 0; p <= last; ++p) {
            int32_t q = coef[u.coef_base + scan_index(u.scan_id, p)];
            int32_t a = q < 0 ? -q : q;
            if (a) { ++nnz; if (a > maxa) maxa = a; }
        }
        uf.nnz = nnz;
        int cls = 7;
        for (int c = 0; c < 8; ++c)
            if (int64_t(maxa) <= (int64_t(1) << kLiteMagBits[c])) { cls = c; break; }
        uf.param = cls;
        uf.body_bits = nnz * (kLiteMagBits[cls] + 1);
    }

    // --- H0: one bit per group of kLiteCbfGroup units ----------------------
    const int ngroups = (nunits + kLiteCbfGroup - 1) / kLiteCbfGroup;
    std::vector<uint8_t> gflag(size_t(ngroups), 0);
    for (int g = 0; g < ngroups; ++g) {
        int lo = g * kLiteCbfGroup;
        int hi = lo + kLiteCbfGroup < nunits ? lo + kLiteCbfGroup : nunits;
        for (int i = lo; i < hi; ++i)
            if (f[size_t(i)].coded) { gflag[size_t(g)] = 1; break; }
    }

    std::vector<uint8_t> payload;
    LiteBitW w(payload);
    for (int g = 0; g < ngroups; ++g) w.put(gflag[size_t(g)], 1);
    ops += uint64_t(ngroups);
    w.align();

    // --- H1: one bit per unit of every flagged group -----------------------
    for (int g = 0; g < ngroups; ++g) {
        if (!gflag[size_t(g)]) continue;
        int lo = g * kLiteCbfGroup;
        int hi = lo + kLiteCbfGroup < nunits ? lo + kLiteCbfGroup : nunits;
        for (int i = lo; i < hi; ++i) {
            w.put(uint32_t(f[size_t(i)].coded), 1);
            ++ops;
        }
    }
    w.align();

    // --- P: LAST and the magnitude class of every coded coefficient unit ---
    for (int i = 0; i < nunits; ++i) {
        const UnitInfo &u = units[i];
        const LiteFacts &uf = f[size_t(i)];
        if (!uf.coded || u.kind == 1) continue;
        w.put(uint32_t(uf.last), nxs_lite_last_bits(u.ncoef));
        w.put(uint32_t(uf.param), kLiteParamBits);
        ops += 2;
    }
    w.align();

    // --- S, and the mode indices section B will carry for it ---------------
    std::vector<uint8_t> mode_idx;
    for (int i = 0; i < nunits; ++i) {
        const UnitInfo &u = units[i];
        const LiteFacts &uf = f[size_t(i)];
        if (!uf.coded) continue;
        if (u.kind == 1) {
            if (!modes) return false;
            const uint8_t *m = modes + u.mode_base;
            int n = u.nbx * u.nbx;
            for (int b = 0; b < n; ++b) {
                if (int(m[b]) >= kNumIntraModes) return false;
                int mpm = lite_mpm_of(m, u.nbx, b);
                int hit = int(m[b]) == mpm ? 1 : 0;
                w.put(uint32_t(hit), 1);
                ++ops;
                if (!hit)
                    mode_idx.push_back(uint8_t(lite_nonmpm_index(mpm, int(m[b]))));
            }
        } else {
            // Position `last` is nonzero by construction and is not coded.
            for (int p = 0; p < uf.last; ++p) {
                w.put(coef[u.coef_base + scan_index(u.scan_id, p)] != 0 ? 1u : 0u,
                      1);
                ++ops;
            }
        }
    }
    w.align();

    // --- B: the bodies -----------------------------------------------------
    size_t midx = 0;
    for (int i = 0; i < nunits; ++i) {
        const UnitInfo &u = units[i];
        const LiteFacts &uf = f[size_t(i)];
        if (!uf.coded) continue;
        if (u.kind == 1) {
            const uint8_t *m = modes + u.mode_base;
            int n = u.nbx * u.nbx;
            for (int b = 0; b < n; ++b) {
                int mpm = lite_mpm_of(m, u.nbx, b);
                if (int(m[b]) == mpm) continue;
                w.put(uint32_t(mode_idx[midx++]), kModeIdxBits);
                ++ops;
            }
            continue;
        }
        for (int p = 0; p <= uf.last; ++p) {
            int32_t q = coef[u.coef_base + scan_index(u.scan_id, p)];
            if (!q) continue;
            int32_t a = q < 0 ? -q : q;
            if (kLiteMagBits[uf.param])
                w.put(uint32_t(a - 1), kLiteMagBits[uf.param]);
            w.put(q < 0 ? 1u : 0u, 1);
            ops += 2;
        }
    }
    w.align();
    if (payload.empty()) payload.push_back(0);

    if (payload.size() + kTileHeaderBytes > kThPayloadLenMask) return false;
    if (op_count) *op_count = ops;

    // --- 8-byte tile header ------------------------------------------------
    // `table_set` names the VARIANT under ENTROPY_LITE, and Pass A implements
    // only kLiteFixed; shape.table_set is a probability-table selector and
    // has no meaning here.
    uint32_t w0 = (uint32_t(shape.tile_index) & kThTileIndexMask) << kThTileIndexShift;
    w0 |= (uint32_t(payload.size()) & kThPayloadLenMask) << kThPayloadLenShift;
    uint32_t w1 = 0;
    w1 |= (uint32_t(shape.res_level) & kThResLevelMask) << kThResLevelShift;
    w1 |= (uint32_t(shape.chroma444) & kThChroma444Mask) << kThChroma444Shift;
    w1 |= (uint32_t(shape.alpha_mode) & kThAlphaModeMask) << kThAlphaModeShift;
    w1 |= (uint32_t(kLiteFixed) & kThTableSetMask) << kThTableSetShift;
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
