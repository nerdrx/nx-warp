// transport_rs_fuzz -- Reed-Solomon over GF(256) and the FEC group machinery.
//
// Normative reference: docs/TRANSPORT.md 6 (Cauchy generator over 0x11D,
// systematic, k <= 10, m <= 4, blocks are whole datagrams prefixed by their
// length and zero padded to the group maximum).
//
// Two surfaces, both in one target because they share the parameter block:
//
//   * rs_encode / rs_decode, checked for the *correctness* property, not only
//     for memory safety: with at least k of the k+m blocks present, every
//     erased data block must come back byte identical.  A Cauchy generator is
//     chosen precisely so that every k-subset is invertible; a k-subset that
//     fails to invert is a bug in the matrix, not in the input.
//
//   * FecGroupDecoder::add_parity, which is the one place where an attacker
//     hands the library a length (`u16 L`) that the library then uses to size
//     a block.  A parity payload whose declared L disagrees with its actual
//     size is the interesting input, and the mutator produces it on purpose.
//
// Input layout (the custom mutator keeps it in range):
//   [0]      k        1..10
//   [1]      m        0..4
//   [2..3]   len      block length, 1..1024
//   [4..5]   erasure bitmap: bit i of [4] erases data i, bit j of [5] parity j
//   [6]      flags    bit0: also drive the FecGroup{Encoder,Decoder} path
//   [7]      pad
//   [rest]   block bytes, cycled to fill k blocks
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <vector>

#include "nxvc/transport/fec.h"

#include "common/nxfuzz.h"

namespace {

constexpr size_t kPrefix = 8;
constexpr size_t kMaxLen = 1024;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < kPrefix + 1) return 0;

    int k = 1 + (data[0] % nxt::kFecMaxK);
    int m = data[1] % (nxt::kFecMaxM + 1);
    size_t len = 1 + (nxf::get16(data + 2) % kMaxLen);
    uint16_t erase_data = data[4];
    uint16_t erase_parity = data[5];
    bool group_path = (data[6] & 1) != 0;

    const uint8_t *src = data + kPrefix;
    size_t src_len = size - kPrefix;

    // ---- systematic encode -------------------------------------------------
    std::vector<std::vector<uint8_t>> blocks(static_cast<size_t>(k));
    std::vector<const uint8_t *> dptr(static_cast<size_t>(k));
    for (int i = 0; i < k; ++i) {
        blocks[static_cast<size_t>(i)].resize(len);
        for (size_t b = 0; b < len; ++b)
            blocks[static_cast<size_t>(i)][b] = src[(static_cast<size_t>(i) * len + b) % src_len];
        dptr[static_cast<size_t>(i)] = blocks[static_cast<size_t>(i)].data();
    }
    std::vector<std::vector<uint8_t>> parity(static_cast<size_t>(m));
    std::vector<uint8_t *> pptr(static_cast<size_t>(m));
    for (int j = 0; j < m; ++j) {
        parity[static_cast<size_t>(j)].assign(len, 0);
        pptr[static_cast<size_t>(j)] = parity[static_cast<size_t>(j)].data();
    }
    if (m > 0) nxt::rs_encode(k, m, len, dptr.data(), pptr.data());

    // ---- erase and recover -------------------------------------------------
    std::vector<const uint8_t *> have_d(static_cast<size_t>(k), nullptr);
    std::vector<const uint8_t *> have_p(static_cast<size_t>(m), nullptr);
    std::vector<std::vector<uint8_t>> out(static_cast<size_t>(k));
    std::vector<uint8_t *> optr(static_cast<size_t>(k), nullptr);
    int present = 0, erased = 0;
    for (int i = 0; i < k; ++i) {
        if (erase_data & (1u << i)) {
            out[static_cast<size_t>(i)].assign(len, 0);
            optr[static_cast<size_t>(i)] = out[static_cast<size_t>(i)].data();
            ++erased;
        } else {
            have_d[static_cast<size_t>(i)] = dptr[static_cast<size_t>(i)];
            ++present;
        }
    }
    for (int j = 0; j < m; ++j) {
        if (!(erase_parity & (1u << j))) {
            have_p[static_cast<size_t>(j)] = parity[static_cast<size_t>(j)].data();
            ++present;
        }
    }

    bool ok = nxt::rs_decode(k, m, len, have_d.data(), have_p.data(), optr.data());

    if (present >= k) {
        // TRANSPORT.md 6: with any k of the k+m blocks the group is
        // recoverable, because every k-subset of a Cauchy generator inverts.
        if (!ok) __builtin_trap();
        for (int i = 0; i < k; ++i) {
            if (!optr[static_cast<size_t>(i)]) continue;
            if (std::memcmp(optr[static_cast<size_t>(i)], dptr[static_cast<size_t>(i)], len) != 0) __builtin_trap();
        }
    } else if (ok && erased > 0) {
        // Claiming success with fewer than k blocks would mean the library
        // fabricated data from an underdetermined system.
        __builtin_trap();
    }

    // ---- the group builder, on attacker-shaped payloads --------------------
    if (group_path) {
        nxt::FecGroupEncoder enc;
        enc.reset(k, m);
        for (int i = 0; i < k; ++i)
            enc.add(std::span<const uint8_t>(blocks[static_cast<size_t>(i)].data(), blocks[static_cast<size_t>(i)].size()));
        std::vector<nxt::ByteVec> pars = enc.finish();

        nxt::FecGroupDecoder dec;
        dec.reset(k, m);
        for (int i = 0; i < k; ++i) {
            if (erase_data & (1u << i)) continue;
            dec.add_data(i, std::span<const uint8_t>(blocks[static_cast<size_t>(i)]));
        }
        for (int j = 0; j < m; ++j) {
            if (erase_parity & (1u << j)) continue;
            if (static_cast<size_t>(j) < pars.size()) {
                nxt::ByteVec p = pars[static_cast<size_t>(j)];
                // Corrupt the declared length half the time: `u16 L` at the
                // front of a parity payload is attacker controlled, and a
                // library that trusts it reads past the buffer.
                if ((data[6] & 2) && p.size() >= 2) nxf::set16(p.data(), nxf::get16(data + 2));
                if ((data[6] & 4) && p.size() > 3) p.resize(p.size() / 2);
                dec.add_parity(k + j, std::span<const uint8_t>(p));
            } else {
                // No parity was produced (m == 0 or nothing added): hand it a
                // payload built straight from the fuzz input.
                dec.add_parity(k + j, std::span<const uint8_t>(src, src_len));
            }
        }
        std::vector<nxt::ByteVec> recovered;
        bool rec = dec.recover(&recovered);
        if (rec) {
            for (const auto &r : recovered) {
                volatile uint8_t acc = 0;
                for (uint8_t b : r) acc = uint8_t(acc + b);
                (void)acc;
            }
        }
        volatile int sink = dec.present() + (dec.complete() ? 1 : 0) +
                            (dec.recoverable() ? 2 : 0) + int(enc.max_len());
        (void)sink;
    }

    // ---- the field itself --------------------------------------------------
    // gf::div by zero and gf::inv(0) are the classic table-index-underflow
    // sites; every input pokes them once.
    volatile uint8_t g = nxt::gf::mul(data[0], data[1]);
    g = uint8_t(g + nxt::gf::inv(data[0]));
    if (data[1] != 0) g = uint8_t(g + nxt::gf::div(data[0], data[1]));
    g = uint8_t(g + nxt::rs_gen(int(data[1] % 4), int(data[0] % 10)));
    (void)g;
    return 0;
}

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                          unsigned seed) {
    using namespace nxf;
    Rng rng(mix_seed(seed, size));

    uint8_t p[kPrefix];
    std::vector<uint8_t> body;
    if (size >= kPrefix) {
        std::memcpy(p, data, kPrefix);
        body.assign(data + kPrefix, data + size);
    } else {
        for (auto &b : p) b = rng.u8();
    }

    switch (rng.below(6)) {
        case 0: p[0] = uint8_t(1 + rng.below(uint32_t(nxt::kFecMaxK))); break;
        case 1: p[1] = uint8_t(rng.below(uint32_t(nxt::kFecMaxM) + 1)); break;
        case 2: {  // block length: 1, tiny, a whole MTU, and the cap
            static const uint16_t v[] = {0, 1, 2, 3, 23, 24, 40, 1316, 1360, 1023, 1024, 0xffff};
            set16(p + 2, rng.chance(2) ? v[rng.below(sizeof(v) / sizeof(v[0]))] : rng.edge_u16());
            break;
        }
        case 3: p[4] = rng.edge_u8(); break;  // data erasures
        case 4: p[5] = rng.edge_u8(); break;  // parity erasures
        default: p[6] = uint8_t(rng.below(8)); break;
    }

    mutate_bytes(body, 4096, rng);
    if (body.empty()) body.push_back(rng.u8());

    std::vector<uint8_t> out;
    putn(out, p, kPrefix);
    putn(out, body.data(), body.size());
    return emit(out, data, max_size);
}
