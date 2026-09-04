#include "nxb_rans.h"

#include <cassert>
#include <cstring>

namespace nxb {
namespace {

struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    uint32_t next()
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return uint32_t(s >> 32);
    }
};

// Encoder for one lane. Words are appended in production order; the caller
// reverses the whole list at the end.
struct LaneEnc
{
    uint32_t x = kRansL;
};

inline void emit(std::vector<uint16_t>& out, uint16_t w) { out.push_back(w); }

// Encode one symbol of `ctx`. At most one renormalisation, by construction:
// x < 2^32 and after a 16-bit flush x < 2^16 <= freq * 2^22.
inline void encSymbol(LaneEnc& e, std::vector<uint16_t>& out,
                      const RansTables& t, int ctx, int sym)
{
    uint32_t f = t.freq[ctx][sym];
    uint32_t c = t.cum[ctx][sym];
    uint32_t maxX = f << (32 - kRansMBits);   // f * 2^22
    if (e.x >= maxX)
    {
        emit(out, uint16_t(e.x & 0xffffu));
        e.x >>= 16;
    }
    // x = ((x / f) << 10) + (x % f) + cum. Division is fine here: this is the
    // CPU side. PAPER 3.6 notes the GPU encoder needs a reciprocal table.
    e.x = ((e.x / f) << kRansMBits) + (e.x % f) + c;
}

// Encode `bits` of width k as a bypass symbol on the same state.
inline void encBypass(LaneEnc& e, std::vector<uint16_t>& out, uint32_t bits, int k)
{
    uint32_t maxX = 1u << (32 - k);
    if (e.x >= maxX)
    {
        emit(out, uint16_t(e.x & 0xffffu));
        e.x >>= 16;
    }
    e.x = (e.x << k) | bits;
}

// One recorded symbol, in decode order.
struct Sym
{
    uint8_t  ctx;
    uint8_t  sym;      // 0..15, 15 = escape
    uint8_t  esc;      // 8 raw bits when sym == 15
    uint8_t  sign;     // valid when sym != 0
    int16_t  value;
};

int sampleSymbol(const RansTables& t, int ctx, Rng& rng)
{
    uint32_t r = rng.next() & (kRansM - 1);
    return t.lut[ctx][r];
}

} // namespace

void RansTables::build()
{
    // Eight contexts with progressively flatter level distributions: the low
    // bands and the "previous level was nonzero" contexts carry more mass on
    // the larger levels. Shape only has to be plausible; K4 measures ops, and
    // the escape rate is kept near 1/256 so the escape ballot almost never
    // renormalises, which is the realistic case.
    for (int c = 0; c < kRansCtx; ++c)
    {
        double p[kRansSymbols];
        double decay = 0.30 + 0.10 * double(c & 3) + ((c >= 4) ? 0.18 : 0.0);
        double zeroMass = (c >= 4) ? 0.30 : 0.55 - 0.06 * double(c & 3);

        double rest = 1.0 - zeroMass;
        p[0] = zeroMass;
        double w = 1.0, sum = 0.0;
        for (int s = 1; s < kRansSymbols; ++s) { p[s] = w; sum += w; w *= decay; }
        for (int s = 1; s < kRansSymbols; ++s) p[s] = p[s] * rest / sum;

        // Quantise to 10-bit frequencies, every symbol at least 1.
        int total = 0;
        for (int s = 0; s < kRansSymbols; ++s)
        {
            int f = int(p[s] * double(kRansM) + 0.5);
            if (f < 1) f = 1;
            freq[c][s] = uint16_t(f);
            total += f;
        }
        // Fix the sum to exactly 1024 by adjusting the largest bucket.
        while (total != int(kRansM))
        {
            int best = 0;
            for (int s = 1; s < kRansSymbols; ++s)
                if (freq[c][s] > freq[c][best]) best = s;
            if (total > int(kRansM)) { freq[c][best]--; total--; }
            else                     { freq[c][best]++; total++; }
        }

        int acc = 0;
        for (int s = 0; s < kRansSymbols; ++s)
        {
            cum[c][s] = uint16_t(acc);
            for (int i = 0; i < freq[c][s]; ++i) lut[c][acc + i] = uint8_t(s);
            acc += freq[c][s];
        }
        assert(acc == int(kRansM));
    }
}

std::vector<uint32_t> RansTables::packed() const
{
    std::vector<uint32_t> out(kRansCtx * (kRansM / 4) + kRansCtx * kRansSymbols, 0u);
    size_t w = 0;
    for (int c = 0; c < kRansCtx; ++c)
        for (uint32_t slot = 0; slot < kRansM; slot += 4)
            out[w++] = uint32_t(lut[c][slot])
                     | (uint32_t(lut[c][slot + 1]) << 8)
                     | (uint32_t(lut[c][slot + 2]) << 16)
                     | (uint32_t(lut[c][slot + 3]) << 24);
    for (int c = 0; c < kRansCtx; ++c)
        for (int s = 0; s < kRansSymbols; ++s)
            out[w++] = uint32_t(freq[c][s]) | (uint32_t(cum[c][s]) << 16);
    assert(w == out.size());
    return out;
}

RansStream ransBuildStreams(const RansTables& t, int tiles, int symsPerLane,
                            uint64_t seed, bool keepExpect)
{
    RansStream out;
    out.offsets.resize(size_t(tiles));
    if (keepExpect) out.expect.resize(size_t(tiles));

    std::vector<uint8_t> bytes;
    bytes.reserve(size_t(tiles) * size_t(symsPerLane) * 6);

    std::vector<Sym> rec[kRansLanes];
    std::vector<uint16_t> words;

    for (int tile = 0; tile < tiles; ++tile)
    {
        Rng rng(seed * 0x2545f4914f6cdd1dull + uint64_t(tile) * 0x9e3779b97f4a7c15ull + 1);

        // ---- pass 1: draw the symbols forward, exactly as the decoder will
        // see them, so the contexts are causal and reproducible.
        for (int l = 0; l < kRansLanes; ++l)
        {
            rec[l].clear();
            rec[l].reserve(size_t(symsPerLane));
            int prevLevel = 0;
            int nPerBlock = symsPerLane / kRansLanes;
            for (int blk = 0; blk < kRansLanes; ++blk)
            {
                for (int k = 0; k < nPerBlock; ++k)
                {
                    int ctx = ransNextCtx(k, prevLevel);
                    int s = sampleSymbol(t, ctx, rng);
                    Sym sy{};
                    sy.ctx = uint8_t(ctx);
                    sy.sym = uint8_t(s);
                    int level = s;
                    if (s == 15) { sy.esc = uint8_t(rng.next() & 0xff); level = 15 + sy.esc; }
                    if (s != 0)  { sy.sign = uint8_t(rng.next() & 1u); }
                    sy.value = int16_t(sy.sign ? -level : level);
                    rec[l].push_back(sy);
                    prevLevel = level;
                }
            }
            if (keepExpect)
            {
                out.expect[size_t(tile)].value[l].resize(size_t(symsPerLane));
                for (int i = 0; i < symsPerLane; ++i)
                    out.expect[size_t(tile)].value[l][size_t(i)] = rec[l][size_t(i)].value;
            }
        }

        // ---- pass 2: encode backwards. Renormalisation points, in decode
        // order, are (i, main), (i, sign), (i, escape). The encoder walks them
        // in reverse and, within one point, walks lanes 7..0, so that after
        // the final reversal the decoder meets them as lanes 0..7.
        LaneEnc enc[kRansLanes];
        words.clear();

        for (int i = symsPerLane - 1; i >= 0; --i)
        {
            for (int l = kRansLanes - 1; l >= 0; --l)
            {
                const Sym& sy = rec[l][size_t(i)];
                if (sy.sym == 15) encBypass(enc[l], words, sy.esc, 8);
            }
            for (int l = kRansLanes - 1; l >= 0; --l)
            {
                const Sym& sy = rec[l][size_t(i)];
                if (sy.sym != 0) encBypass(enc[l], words, sy.sign, 1);
            }
            for (int l = kRansLanes - 1; l >= 0; --l)
            {
                const Sym& sy = rec[l][size_t(i)];
                encSymbol(enc[l], words, t, sy.ctx, sy.sym);
            }
        }

        // ---- emit the tile slot: 8 final states, then the reversed words.
        while (bytes.size() % 4u) bytes.push_back(0);
        out.offsets[size_t(tile)] = uint32_t(bytes.size());

        for (int l = 0; l < kRansLanes; ++l)
        {
            uint32_t v = enc[l].x;
            bytes.push_back(uint8_t(v));       bytes.push_back(uint8_t(v >> 8));
            bytes.push_back(uint8_t(v >> 16)); bytes.push_back(uint8_t(v >> 24));
        }
        for (size_t n = words.size(); n-- > 0; )
        {
            bytes.push_back(uint8_t(words[n] & 0xff));
            bytes.push_back(uint8_t(words[n] >> 8));
        }
    }

    out.payloadBytes = bytes.size();
    bytes.resize((bytes.size() + 7u) & ~size_t(7u), 0u);   // pad, readU16 may over-read
    out.words.resize(bytes.size() / 4u);
    std::memcpy(out.words.data(), bytes.data(), bytes.size());
    return out;
}

bool ransDecodeCheck(const RansTables& t, const RansStream& s,
                     int tiles, int symsPerLane, int* firstBadTile)
{
    const uint32_t* bw = s.words.data();
    size_t bwCount = s.words.size();

    auto readU16 = [&](uint32_t byteOff) -> uint32_t {
        size_t idx = byteOff >> 2;
        if (idx >= bwCount) idx = bwCount - 1;
        uint32_t w = bw[idx];
        return (w >> ((byteOff & 2u) * 8u)) & 0xffffu;
    };

    for (int tile = 0; tile < tiles; ++tile)
    {
        uint32_t slot = s.offsets[size_t(tile)];
        uint32_t x[kRansLanes];
        for (int l = 0; l < kRansLanes; ++l)
            x[l] = bw[(slot >> 2) + uint32_t(l)];
        uint32_t cursor = slot + 32u;

        int prevLevel[kRansLanes] = {0};
        int nPerBlock = symsPerLane / kRansLanes;
        int idx = 0;

        for (int blk = 0; blk < kRansLanes; ++blk)
        for (int k = 0; k < nPerBlock; ++k, ++idx)
        {
            int sym[kRansLanes], level[kRansLanes], sgn[kRansLanes] = {0};

            // main step, then the cluster-wide renormalisation in lane order
            bool need[kRansLanes];
            for (int l = 0; l < kRansLanes; ++l)
            {
                int ctx = ransNextCtx(k, prevLevel[l]);
                uint32_t sl = x[l] & (kRansM - 1);
                int sy = t.lut[ctx][sl];
                sym[l] = sy;
                x[l] = uint32_t(t.freq[ctx][sy]) * (x[l] >> kRansMBits) + sl - t.cum[ctx][sy];
                need[l] = x[l] < kRansL;
            }
            for (int l = 0; l < kRansLanes; ++l)
                if (need[l]) { x[l] = (x[l] << 16) | readU16(cursor); cursor += 2; }

            for (int l = 0; l < kRansLanes; ++l)
            {
                need[l] = false;
                if (sym[l] != 0) { sgn[l] = int(x[l] & 1u); x[l] >>= 1; need[l] = x[l] < kRansL; }
            }
            for (int l = 0; l < kRansLanes; ++l)
                if (need[l]) { x[l] = (x[l] << 16) | readU16(cursor); cursor += 2; }

            for (int l = 0; l < kRansLanes; ++l)
            {
                level[l] = sym[l];
                need[l] = false;
                if (sym[l] == 15)
                {
                    level[l] = 15 + int(x[l] & 255u);
                    x[l] >>= 8;
                    need[l] = x[l] < kRansL;
                }
            }
            for (int l = 0; l < kRansLanes; ++l)
                if (need[l]) { x[l] = (x[l] << 16) | readU16(cursor); cursor += 2; }

            for (int l = 0; l < kRansLanes; ++l)
            {
                int16_t v = int16_t(sgn[l] ? -level[l] : level[l]);
                if (!s.expect.empty() && v != s.expect[size_t(tile)].value[l][size_t(idx)])
                {
                    if (firstBadTile) *firstBadTile = tile;
                    return false;
                }
                prevLevel[l] = level[l];
            }
        }
    }
    return true;
}

} // namespace nxb
