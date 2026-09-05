#include "entropy_lite.h"

#include <cstdio>
#include <cstdlib>

namespace nxvc {

LiteStats g_lite_stats{};

// Development instrumentation: set NXVC_LITE_STATS=1 and the process prints
// the section breakdown of every Lite payload it produced, once, at exit.
// It is a measurement hook for ref/RESULTS-entropy-lite.md, nothing else.
namespace {
struct LiteStatsDump {
    ~LiteStatsDump() {
        const char *e = std::getenv("NXVC_LITE_STATS");
        if (!e || !*e || *e == '0') return;
        const LiteStats &s = g_lite_stats;
        u64 tot = s.cbf + s.param + s.sig + s.mode + s.mag + s.sign + s.pad;
        if (!tot) return;
        std::fprintf(stderr, "lite payload bits: total %llu\n",
                     (unsigned long long)tot);
        const char *n[7] = {"cbf", "param", "sig", "mode", "mag", "sign", "pad"};
        const u64 v[7] = {s.cbf, s.param, s.sig, s.mode, s.mag, s.sign, s.pad};
        for (int i = 0; i < 7; ++i)
            std::fprintf(stderr, "  %-6s %12llu  %6.2f%%\n", n[i],
                         (unsigned long long)v[i],
                         100.0 * (double)v[i] / (double)tot);
    }
} g_lite_stats_dump;
}  // namespace

namespace {

// ------------------------------------------------------------ bit plumbing
// MSB-first inside a byte, which is the packing docs/SYNTAX.md 9.4 already
// uses for transmitted tables, and the one a GPU reads with a single funnel
// shift over two 32-bit words.
struct BitW {
    std::vector<u8> &b;
    u32 acc = 0;
    int n = 0;
    explicit BitW(std::vector<u8> &out) : b(out) {}
    void put(u32 v, int k) {
        while (k > 0) {
            int take = k > 8 ? 8 : k;
            u32 chunk = (v >> (k - take)) & ((1u << take) - 1u);
            acc = (acc << take) | chunk;
            n += take;
            k -= take;
            while (n >= 8) {
                b.push_back((u8)((acc >> (n - 8)) & 0xff));
                n -= 8;
            }
        }
        acc &= (1u << n) - 1u;
    }
    void align() {
        if (n) {
            b.push_back((u8)((acc << (8 - n)) & 0xff));
            n = 0;
            acc = 0;
        }
    }
};

struct BitR {
    const u8 *b;
    size_t len;      // bytes
    size_t pos = 0;  // bit position
    bool bad = false;
    BitR(const u8 *p, size_t l) : b(p), len(l) {}
    void seek_bit(size_t bit) { pos = bit; }
    u32 get(int k) {
        u32 v = 0;
        for (int i = 0; i < k; ++i) {
            if ((pos >> 3) >= len) { bad = true; return v; }
            u32 bit = (b[pos >> 3] >> (7 - (pos & 7))) & 1u;
            v = (v << 1) | bit;
            ++pos;
        }
        return v;
    }
};

inline size_t align8(size_t bits) { return (bits + 7) & ~(size_t)7; }

// Exp-Golomb of order k, as in 9.5's escape but with the order a parameter.
// n = v + 2^k, b = floor(log2 n), j = b - k; j ones, a zero, the low b bits.
inline int eg_len(u32 v, int k) {
    u64 n = (u64)v + (1u << k);
    int b = 0;
    while ((n >> (b + 1)) != 0) ++b;
    return 2 * (b - k) + k + 1;
}
inline void eg_put(BitW &w, u32 v, int k) {
    u64 n = (u64)v + (1u << k);
    int b = 0;
    while ((n >> (b + 1)) != 0) ++b;
    for (int i = 0; i < b - k; ++i) w.put(1, 1);
    w.put(0, 1);
    if (b) w.put((u32)(n - ((u64)1 << b)), b);
}
inline bool eg_get(BitR &r, int k, u32 &v) {
    int j = 0;
    while (r.get(1) == 1) {
        if (r.bad) return false;
        if (++j > 20) return false;
    }
    if (r.bad) return false;
    int b = j + k;
    u32 suf = b ? r.get(b) : 0;
    if (r.bad) return false;
    u64 n = ((u64)1 << b) + suf;
    u64 val = n - (1u << k);
    if (val > 32766u) return false;   // codes |q| - 1, and |q| <= 32767
    v = (u32)val;
    return true;
}

inline int mode_count(const Unit &u) { return (int)u.nbx * (int)u.nbx; }

// Section-P width of one coded coefficient unit.
inline int unit_param_bits(const Unit &u, int variant) {
    if (u.kind == UNIT_MODE) return 0;
    return lite_last_bits((int)u.ncoef) + 3 +
           (variant == kLiteRice ? kLiteLenBits : 0);
}

struct UnitFacts {
    int coded = 0;
    int last = 0;
    int nnz = 0;
    int param = 0;      // FIXED: magnitude class.  RICE: Exp-Golomb order.
    int body_bits = 0;
};

}  // namespace

// ---------------------------------------------------------------- encoder
bool lite_encode_units(const Unit *units, int nunits, int variant,
                       std::vector<u8> &out) {
    if (variant != kLiteFixed && variant != kLiteRice) return false;
    if (nunits <= 0) return false;
    std::vector<UnitFacts> f((size_t)nunits);

    for (int i = 0; i < nunits; ++i) {
        const Unit &u = units[i];
        UnitFacts &uf = f[(size_t)i];
        if (u.kind == UNIT_MODE) {
            uf.coded = mode_count(u) != 0;
            continue;
        }
        int last = -1;
        for (int p = (int)u.ncoef - 1; p >= 0; --p)
            if (u.coef[u.scan[p]] != 0) { last = p; break; }
        if (last < 0) { uf.coded = 0; continue; }
        uf.coded = 1;
        uf.last = last;
        i32 maxa = 0;
        int nnz = 0;
        for (int p = 0; p <= last; ++p) {
            i32 q = u.coef[u.scan[p]];
            i32 a = q < 0 ? -q : q;
            if (a) { ++nnz; if (a > maxa) maxa = a; }
        }
        uf.nnz = nnz;
        if (variant == kLiteFixed) {
            int cls = 7;
            for (int c = 0; c < 8; ++c)
                if ((i64)maxa <= ((i64)1 << kLiteMagBits[c])) { cls = c; break; }
            uf.param = cls;
            uf.body_bits = nnz * (kLiteMagBits[cls] + 1);
        } else {
            int bestk = 0, bestbits = 0;
            for (int k = 0; k < 8; ++k) {
                int bits = 0;
                for (int p = 0; p <= last; ++p) {
                    i32 q = u.coef[u.scan[p]];
                    i32 a = q < 0 ? -q : q;
                    if (a) bits += eg_len((u32)(a - 1), k) + 1;
                }
                if (k == 0 || bits < bestbits) { bestbits = bits; bestk = k; }
            }
            uf.param = bestk;
            uf.body_bits = bestbits;
            if (uf.body_bits >= (1 << kLiteLenBits)) return false;
        }
    }

    const int ngroups = (nunits + kLiteCbfGroup - 1) / kLiteCbfGroup;
    std::vector<u8> gflag((size_t)ngroups, 0);
    size_t h1bits = 0;
    for (int g = 0; g < ngroups; ++g) {
        int lo = g * kLiteCbfGroup;
        int hi = lo + kLiteCbfGroup < nunits ? lo + kLiteCbfGroup : nunits;
        for (int i = lo; i < hi; ++i)
            if (f[(size_t)i].coded) { gflag[(size_t)g] = 1; break; }
        if (gflag[(size_t)g]) h1bits += (size_t)(hi - lo);
    }

    out.clear();
    BitW w(out);
    for (int g = 0; g < ngroups; ++g) w.put(gflag[(size_t)g], 1);
    g_lite_stats.cbf += (u64)ngroups;
    g_lite_stats.pad += align8((size_t)ngroups) - (size_t)ngroups;
    w.align();
    for (int g = 0; g < ngroups; ++g) {
        if (!gflag[(size_t)g]) continue;
        int lo = g * kLiteCbfGroup;
        int hi = lo + kLiteCbfGroup < nunits ? lo + kLiteCbfGroup : nunits;
        for (int i = lo; i < hi; ++i) w.put((u32)f[(size_t)i].coded, 1);
    }
    g_lite_stats.cbf += (u64)h1bits;
    g_lite_stats.pad += align8(h1bits) - h1bits;
    w.align();

    size_t pbits = 0, sbits = 0;
    for (int i = 0; i < nunits; ++i) {
        const Unit &u = units[i];
        const UnitFacts &uf = f[(size_t)i];
        if (!uf.coded || u.kind == UNIT_MODE) continue;
        w.put((u32)uf.last, lite_last_bits((int)u.ncoef));
        w.put((u32)uf.param, 3);
        if (variant == kLiteRice) w.put((u32)uf.body_bits, kLiteLenBits);
        pbits += (size_t)unit_param_bits(u, variant);
    }
    g_lite_stats.param += (u64)pbits;
    g_lite_stats.pad += align8(pbits) - pbits;
    w.align();

    // Section S, and the mode indices that section B will carry for it.
    std::vector<u8> mode_idx;
    for (int i = 0; i < nunits; ++i) {
        const Unit &u = units[i];
        const UnitFacts &uf = f[(size_t)i];
        if (!uf.coded) continue;
        if (u.kind == UNIT_MODE) {
            int n = mode_count(u);
            // The mode alphabet is the unit's own: nine, or ten on a chroma
            // plane whose tile enables chroma-from-luma (SYNTAX.md 7.7).  The
            // non-MPM index is still three bypass bits either way -- CfL is
            // the tenth VALUE, and one of the ten is always the MPM -- so the
            // Lite path's field widths are unchanged by it.
            const int nmodes = u.nmodes ? u.nmodes : kNumIntraModes;
            for (int b = 0; b < n; ++b) {
                if (u.modes[b] >= nmodes) return false;
                int mpm = mpm_of(u.modes, u.nbx, b);
                int hit = u.modes[b] == mpm;
                w.put((u32)hit, 1);
                if (!hit)
                    mode_idx.push_back(
                        (u8)nonmpm_index(mpm, u.modes[b], nmodes));
            }
            g_lite_stats.mode += (u64)n;
            sbits += (size_t)n;
        } else {
            // Position `last` is nonzero by construction and is not coded.
            for (int p = 0; p < uf.last; ++p)
                w.put(u.coef[u.scan[p]] != 0 ? 1u : 0u, 1);
            g_lite_stats.sig += (u64)uf.last;
            sbits += (size_t)uf.last;
        }
    }
    g_lite_stats.pad += align8(sbits) - sbits;
    w.align();

    size_t bbits = 0, midx = 0;
    for (int i = 0; i < nunits; ++i) {
        const Unit &u = units[i];
        const UnitFacts &uf = f[(size_t)i];
        if (!uf.coded) continue;
        if (u.kind == UNIT_MODE) {
            int n = mode_count(u);
            for (int b = 0; b < n; ++b) {
                int mpm = mpm_of(u.modes, u.nbx, b);
                if (u.modes[b] == mpm) continue;
                w.put((u32)mode_idx[midx++], 3);
                bbits += 3;
                g_lite_stats.mode += 3;
            }
            continue;
        }
        for (int p = 0; p <= uf.last; ++p) {
            i32 q = u.coef[u.scan[p]];
            if (!q) continue;
            i32 a = q < 0 ? -q : q;
            if (variant == kLiteFixed) {
                if (kLiteMagBits[uf.param])
                    w.put((u32)(a - 1), kLiteMagBits[uf.param]);
            } else {
                eg_put(w, (u32)(a - 1), uf.param);
            }
            w.put(q < 0 ? 1u : 0u, 1);
        }
        bbits += (size_t)uf.body_bits;
        g_lite_stats.sign += (u64)uf.nnz;
        g_lite_stats.mag += (u64)(uf.body_bits - uf.nnz);
    }
    g_lite_stats.pad += align8(bbits) - bbits;
    w.align();
    if (out.empty()) out.push_back(0);
    return true;
}

// ---------------------------------------------------------------- decoder
// Every section start follows from the section before it by a quantity that
// is either a constant of the unit list or a popcount over an already-read
// fixed-width section.  That is what makes the layout parallel: a lane finds
// its own unit with two prefix sums and no arithmetic-coder state at all.
bool lite_decode_units(const Unit *units, int nunits, int variant,
                       const u8 *buf, size_t len) {
    if (variant != kLiteFixed && variant != kLiteRice) return false;
    if (nunits <= 0) return false;
    const int ngroups = (nunits + kLiteCbfGroup - 1) / kLiteCbfGroup;
    if (len * 8 < align8((size_t)ngroups)) return false;

    BitR gr(buf, len);
    std::vector<u8> gflag((size_t)ngroups, 0);
    size_t h1bits = 0;
    for (int g = 0; g < ngroups; ++g) {
        gflag[(size_t)g] = (u8)gr.get(1);
        if (gflag[(size_t)g]) {
            int lo = g * kLiteCbfGroup;
            int hi = lo + kLiteCbfGroup < nunits ? lo + kLiteCbfGroup : nunits;
            h1bits += (size_t)(hi - lo);
        }
    }
    if (gr.bad) return false;

    const size_t h1_off = align8((size_t)ngroups);
    if (h1_off + h1bits > len * 8) return false;
    BitR hr(buf, len);
    hr.seek_bit(h1_off);
    std::vector<u8> coded((size_t)nunits, 0);
    for (int g = 0; g < ngroups; ++g) {
        if (!gflag[(size_t)g]) continue;
        int lo = g * kLiteCbfGroup;
        int hi = lo + kLiteCbfGroup < nunits ? lo + kLiteCbfGroup : nunits;
        for (int i = lo; i < hi; ++i) coded[(size_t)i] = (u8)hr.get(1);
    }
    if (hr.bad) return false;

    size_t pbits = 0;
    for (int i = 0; i < nunits; ++i)
        if (coded[(size_t)i]) pbits += (size_t)unit_param_bits(units[i], variant);
    const size_t p_off = h1_off + align8(h1bits);
    const size_t s_off = p_off + align8(pbits);
    if (s_off > len * 8) return false;

    BitR pr(buf, len);
    pr.seek_bit(p_off);
    std::vector<i32> last((size_t)nunits, -1), param((size_t)nunits, 0),
        blen((size_t)nunits, -1);
    size_t sbits = 0;
    for (int i = 0; i < nunits; ++i) {
        if (!coded[(size_t)i]) continue;
        const Unit &u = units[i];
        if (u.kind == UNIT_MODE) { sbits += (size_t)mode_count(u); continue; }
        i32 L = (i32)pr.get(lite_last_bits((int)u.ncoef));
        param[(size_t)i] = (i32)pr.get(3);
        if (variant == kLiteRice) blen[(size_t)i] = (i32)pr.get(kLiteLenBits);
        if (pr.bad || L >= (i32)u.ncoef) return false;
        last[(size_t)i] = L;
        sbits += (size_t)L;
    }
    const size_t b_off = s_off + align8(sbits);
    if (b_off > len * 8) return false;

    BitR sr(buf, len), br(buf, len);
    sr.seek_bit(s_off);
    br.seek_bit(b_off);

    // Section S is read ahead of section B for the whole tile, because a mode
    // unit's B length is the count of its zero flags in S.  The reference
    // walks the units in order; a lane finds the same offsets with a prefix
    // sum over the same quantities.
    std::vector<u8> sflags;
    for (int i = 0; i < nunits; ++i) {
        const Unit &u = units[i];
        if (!coded[(size_t)i]) {
            if (u.kind == UNIT_MODE && mode_count(u) != 0) return false;
            continue;
        }
        if (u.kind == UNIT_MODE) {
            int n = mode_count(u);
            sflags.assign((size_t)n, 0);
            for (int b = 0; b < n; ++b) {
                sflags[(size_t)b] = (u8)sr.get(1);
                if (sr.bad) return false;
            }
            // The MPM of block b reads the modes of b-1 and b-nbx, which this
            // loop has already resolved: a raster wavefront of 2*nbx-1 steps,
            // not an arithmetic-coder chain.
            for (int b = 0; b < n; ++b) {
                int mpm = mpm_of(u.modes, u.nbx, b);
                if (sflags[(size_t)b]) {
                    u.modes[b] = (u8)mpm;
                } else {
                    u32 idx = br.get(3);
                    if (br.bad) return false;
                    u.modes[b] = (u8)nonmpm_mode(
                        mpm, (int)idx, u.nmodes ? u.nmodes : kNumIntraModes);
                }
            }
            continue;
        }
        const int L = last[(size_t)i];
        const int k = param[(size_t)i];
        const size_t body_start = br.pos;
        for (int p = 0; p <= L; ++p) {
            u32 sig = 1;
            if (p < L) {
                sig = sr.get(1);
                if (sr.bad) return false;
            }
            if (!sig) continue;
            u32 mag = 0;
            if (variant == kLiteFixed) {
                if (kLiteMagBits[k]) {
                    mag = br.get(kLiteMagBits[k]);
                    if (br.bad) return false;
                }
                if (mag > 32766u) return false;
            } else if (!eg_get(br, k, mag)) {
                return false;
            }
            u32 sgn = br.get(1);
            if (br.bad) return false;
            i32 a = (i32)mag + 1;
            u.coef[u.scan[p]] = (i16)(sgn ? -a : a);
        }
        if (variant == kLiteRice &&
            (i32)(br.pos - body_start) != blen[(size_t)i])
            return false;
    }
    return true;
}

}  // namespace nxvc
