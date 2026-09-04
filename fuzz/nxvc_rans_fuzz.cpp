// nxvc_rans_fuzz -- the interleaved rANS decoder and the per-lane coefficient
// syntax machine, on random payloads with random probability tables.
//
// Normative reference: docs/SYNTAX.md 9 (contexts, LAST classes, the escape,
// the table normalization of 9.4, and the rANS step of 9.5).
//
// This is the code a hostile stream reaches first and the code with the most
// arithmetic per byte, so it gets its own target rather than being reached
// only through a valid stream header.  Two things make it fuzzable at all:
//
//   1. the payload is fed directly, so no header has to be guessed;
//   2. the input carries a small parameter block, so the *shape* of the
//      decode (lane count, unit count, coefficient counts, scan order, table
//      set) is under the mutator's control instead of being fixed.
//
// Input layout -- the custom mutator keeps this prefix intact and in range:
//
//   [0]      nsub_log2      lane count is 1 << (b0 & 7); 6 and 7 are illegal
//                           and must be refused, so they are not masked away
//   [1]      table set      0..7 (SYNTAX.md 9.4)
//   [2]      flags          bit0: 120 bytes of custom table deltas follow
//                           bit1: transform skip (raster scan, not zigzag)
//                           bits 2..3: coding-unit shape selector
//   [3]      unit count     1..64
//   [4..123] table deltas   present iff flags bit0
//   [rest]   rANS payload   4 bytes of initial state per active lane, then the
//                           interleaved renormalization pairs
//
// Invariants: every decoded level fits int16 (SYNTAX.md 9.3 bounds |q| to
// 32767 through the capped Exp-Golomb prefix), the decoder never reads past
// the payload, and it never writes outside a unit's coefficient array.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <vector>

#include "common.h"   // ref/src/common.h
#include "entropy.h"  // ref/src/entropy.h

#include "common/nxfuzz.h"

namespace {

constexpr size_t kPrefixBytes = 4;
constexpr size_t kTableDeltaBytes = 120;
constexpr int kMaxUnits = 64;
constexpr int16_t kGuard = int16_t(0x5A5A);

// SYNTAX.md 9.4: a transmitted table set is 12 contexts x 16 symbols x 5 bits,
// MSB-first, each 5-bit value indexing kDeltaMul, applied to the built-in
// default of the same set index and then normalized to sum 1024.
bool apply_custom_tables(const uint8_t *bits, int set_index, nxvc::TableSet &ts) {
    size_t bitpos = 0;
    for (int c = 0; c < nxvc::kNumCtx; ++c) {
        nxvc::u16 f[nxvc::kNumSym];
        for (int s = 0; s < nxvc::kNumSym; ++s) {
            uint32_t d = 0;
            for (int b = 0; b < 5; ++b) {
                size_t byte = bitpos >> 3;
                int shift = 7 - int(bitpos & 7);
                d = (d << 1) | ((bits[byte] >> shift) & 1u);
                ++bitpos;
            }
            uint32_t def = nxvc::kDefaultFreq[set_index][c][s];
            int32_t v = int32_t((def * nxvc::kDeltaMul[d] + 128) >> 8);
            if (v < 1) v = 1;
            if (v > 32767) v = 32767;
            f[s] = nxvc::u16(v);
        }
        nxvc::normalize_freqs(f);
        std::memcpy(ts.ctx[c].freq, f, sizeof f);
        if (!nxvc::finalize_ctx(ts.ctx[c])) return false;
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < kPrefixBytes) return 0;

    const int nsub_log2 = data[0] & 7;
    const int set_index = data[1] & 7;
    const uint8_t flags = data[2];
    const bool custom = (flags & 1) != 0;
    const bool tskip = (flags & 2) != 0;
    const int shape = (flags >> 2) & 3;
    int nunits = 1 + (data[3] % kMaxUnits);

    // SYNTAX.md 4.1 constrains nsub_log2 to 0..5; 6 and 7 are illegal syntax
    // that the tile-header parser rejects before the entropy coder ever runs,
    // so this target does not feed them to a decoder that never sees them.
    if (nsub_log2 > 5) return 0;
    const int nlanes = 1 << nsub_log2;

    size_t off = kPrefixBytes;
    nxvc::TableSet ts;
    nxvc::build_default_set(ts, set_index);
    if (custom) {
        if (size < off + kTableDeltaBytes) return 0;
        if (!apply_custom_tables(data + off, set_index, ts)) return 0;
        off += kTableDeltaBytes;
    }

    // Coding units.  Each coefficient array is bracketed by guard values so an
    // out-of-bounds write is caught in a plain build too, not only under ASan.
    static const uint16_t kShapes[4][4] = {
        {64, 64, 64, 64}, {64, 16, 4, 1}, {16, 16, 4, 4}, {1, 4, 16, 64},
    };
    const size_t unit_count = static_cast<size_t>(nunits);
    std::vector<std::vector<int16_t>> storage(unit_count);
    std::vector<nxvc::Unit> units(unit_count);
    for (int i = 0; i < nunits; ++i) {
        uint16_t n = kShapes[shape][i & 3];
        storage[static_cast<size_t>(i)].assign(static_cast<size_t>(n) + 8, kGuard);
        nxvc::Unit &u = units[static_cast<size_t>(i)];
        u.coef = storage[static_cast<size_t>(i)].data() + 4;
        std::memset(u.coef, 0, sizeof(int16_t) * n);
        u.ncoef = n;
        u.scan = nxvc::scan_table(n, tskip && n == 64);
        bool chroma = (i & 1) != 0;
        u.ctx_cbf = uint8_t(chroma ? nxvc::kCtxCbfChroma : nxvc::kCtxCbfLuma);
        u.ctx_last = uint8_t(chroma ? nxvc::kCtxLastChroma : nxvc::kCtxLastLuma);
    }

    const uint8_t *payload = data + off;
    const size_t payload_len = size - off;
    bool ok = nxvc::decode_units(units.data(), nunits, nlanes, ts, payload, payload_len);

    // Guards must be untouched whether or not the decode succeeded.
    for (int i = 0; i < nunits; ++i) {
        const auto &s = storage[static_cast<size_t>(i)];
        for (int g = 0; g < 4; ++g)
            if (s[static_cast<size_t>(g)] != kGuard || s[s.size() - 1 - static_cast<size_t>(g)] != kGuard)
                __builtin_trap();
    }
    if (ok) {
        // SYNTAX.md 9.3: |q| is bounded by 32767, which is what makes the
        // int32 dequantizer bound provable.  int16 storage cannot hold more,
        // so this checks the value is not the int16 minimum, which the bound
        // excludes and which would break clamp16's symmetry downstream.
        for (int i = 0; i < nunits; ++i) {
            const nxvc::Unit &u = units[static_cast<size_t>(i)];
            for (uint16_t c = 0; c < u.ncoef; ++c)
                if (u.coef[c] == int16_t(-32768)) __builtin_trap();
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Structure-aware mutator: the 4-byte parameter prefix (and the 120-byte table
// blob when it is present) are mutated as fields, the payload as bytes.  It
// also knows the one fact that decides whether a payload decodes at all --
// the first 4 * active_lanes bytes are the initial rANS states and each must
// be >= 2^16 (SYNTAX.md 9.5) -- and repairs them most of the time, so the
// fuzzer spends its budget inside the symbol loop instead of bouncing off the
// first length and range check.
// ---------------------------------------------------------------------------
extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                          unsigned seed) {
    using namespace nxf;
    Rng rng(mix_seed(seed, size));

    uint8_t prefix[kPrefixBytes];
    std::vector<uint8_t> tabs, payload;
    if (size >= kPrefixBytes) {
        std::memcpy(prefix, data, kPrefixBytes);
        size_t off = kPrefixBytes;
        if ((prefix[2] & 1) && size >= off + kTableDeltaBytes) {
            tabs.assign(data + off, data + off + kTableDeltaBytes);
            off += kTableDeltaBytes;
        }
        payload.assign(data + off, data + size);
    } else {
        prefix[0] = uint8_t(rng.below(6));
        prefix[1] = uint8_t(rng.below(8));
        prefix[2] = uint8_t(rng.below(16));
        prefix[3] = uint8_t(rng.below(64));
    }

    switch (rng.below(8)) {
        case 0: prefix[0] = uint8_t(rng.chance(6) ? rng.below(8) : rng.below(6)); break;
        case 1: prefix[1] = uint8_t(rng.below(8)); break;
        case 2: prefix[2] ^= uint8_t(1u << rng.below(4)); break;
        case 3: prefix[3] = rng.edge_u8(); break;
        case 4:  // toggle the custom-table blob into or out of existence
            prefix[2] ^= 1u;
            if (prefix[2] & 1) {
                tabs.resize(kTableDeltaBytes);
                for (auto &b : tabs) b = rng.u8();
            } else {
                tabs.clear();
            }
            break;
        case 5:
            if (!tabs.empty()) tabs[rng.below(kTableDeltaBytes)] = rng.edge_u8();
            break;
        default: break;
    }
    if ((prefix[2] & 1) && tabs.size() != kTableDeltaBytes) {
        size_t old = tabs.size();
        tabs.resize(kTableDeltaBytes);
        for (size_t i = old; i < tabs.size(); ++i) tabs[i] = rng.u8();
    }
    if (!(prefix[2] & 1)) tabs.clear();

    mutate_bytes(payload, 4096, rng);

    // Repair the per-lane initial states so the payload is decodable.
    if (!rng.chance(4)) {
        int nsub = prefix[0] & 7;
        if (nsub <= 5) {
            int nlanes = 1 << nsub;
            int nunits = 1 + (prefix[3] % 64);
            int active = nlanes < nunits ? nlanes : nunits;
            size_t need = static_cast<size_t>(4 * active);
            if (payload.size() < need + 8) payload.resize(need + 8 + rng.below(256));
            for (int l = 0; l < active; ++l) {
                // Any value >= L == 2^16; keep the low bits from the mutated
                // bytes so the state itself stays under the mutator's control.
                uint32_t v = get32(&payload[static_cast<size_t>(4 * l)]);
                v |= 0x00010000u;
                set32(&payload[static_cast<size_t>(4 * l)], v);
            }
        }
    }

    std::vector<uint8_t> out;
    putn(out, prefix, kPrefixBytes);
    putn(out, tabs.data(), tabs.size());
    putn(out, payload.data(), payload.size());
    return emit(out, data, max_size);
}
