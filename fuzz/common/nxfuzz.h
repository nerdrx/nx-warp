// nxfuzz.h -- shared scaffolding for every NX Warp fuzz target.
//
// Every target in fuzz/ is one translation unit that defines
// LLVMFuzzerTestOneInput and (usually) LLVMFuzzerCustomMutator.  The same
// source builds two ways:
//
//   * with -fsanitize=fuzzer  -> a real libFuzzer binary  (<target>)
//   * with NXFUZZ_STANDALONE  -> a plain regression runner (<target>_replay)
//     that replays a corpus directory and needs no clang and no libFuzzer.
//
// The standalone runner supplies its own main() from nxfuzz_main.cpp and a
// deterministic stand-in for LLVMFuzzerMutate, so a custom mutator is still
// exercised (and still compiled) in a non-clang CI.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXFUZZ_H
#define NXFUZZ_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// libFuzzer's built-in mutator.  Declared here for both build modes; the
// standalone runner defines a deterministic replacement in nxfuzz_main.cpp.
extern "C" size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);

namespace nxf {

// ---------------------------------------------------------------- randomness
// SplitMix64: tiny, seekable, and identical on every platform, so a mutator
// decision can be reproduced from a seed in a bug report.
class Rng {
  public:
    explicit Rng(uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return uint32_t(next() >> 32); }
    // Uniform in [0, n).  n == 0 returns 0.
    uint32_t below(uint32_t n) { return n ? uint32_t(next() % n) : 0u; }
    bool chance(uint32_t one_in) { return below(one_in ? one_in : 1) == 0; }
    uint8_t u8() { return uint8_t(next() >> 56); }
    // A byte biased towards the values that break parsers.
    uint8_t edge_u8() {
        static const uint8_t k[] = {0, 1, 2, 3, 0x7f, 0x80, 0xfe, 0xff, 0x55, 0xaa};
        return chance(2) ? k[below(sizeof(k))] : u8();
    }
    uint16_t edge_u16() {
        static const uint16_t k[] = {0,      1,      2,      0x7f,   0x80,
                                     0xff,   0x100,  0x7fff, 0x8000, 0xfffe,
                                     0xffff, 0x1000, 0x0fff};
        return chance(2) ? k[below(sizeof(k) / sizeof(k[0]))] : uint16_t(next() >> 40);
    }
    uint32_t edge_u32() {
        static const uint32_t k[] = {0u,          1u,          2u,
                                     0x7fffffffu, 0x80000000u, 0xffffffffu,
                                     0xffffu,     0x10000u,    0xfffffffeu};
        return chance(2) ? k[below(sizeof(k) / sizeof(k[0]))] : u32();
    }

  private:
    uint64_t s_;
};

// A mutator seed derived from libFuzzer's own seed keeps runs reproducible
// (`-seed=N` on the command line reaches the custom mutator through this).
inline uint64_t mix_seed(unsigned seed, size_t size) {
    return (uint64_t(seed) << 32) ^ (uint64_t(size) * 0x9E3779B97F4A7C15ull) ^ 0xD1B54A32D192ED03ull;
}

// ------------------------------------------------------------- little endian
inline void put8(std::vector<uint8_t> &b, uint8_t v) { b.push_back(v); }
inline void put16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
inline void put32(std::vector<uint8_t> &b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
inline void put64(std::vector<uint8_t> &b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
inline void putn(std::vector<uint8_t> &b, const uint8_t *p, size_t n) {
    b.insert(b.end(), p, p + n);
}
inline uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
inline uint64_t get64(const uint8_t *p) {
    return uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32);
}
inline void set16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
inline void set32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

// Replace bits [lo, lo+n) of `w`.
inline uint32_t set_bits(uint32_t w, int lo, int n, uint32_t v) {
    uint32_t mask = (n >= 32) ? 0xffffffffu : (((1u << n) - 1u) << lo);
    return (w & ~mask) | ((v << lo) & mask);
}
inline uint32_t get_bits(uint32_t w, int lo, int n) {
    uint32_t mask = (n >= 32) ? 0xffffffffu : ((1u << n) - 1u);
    return (w >> lo) & mask;
}

// ------------------------------------------------------------ byte surgery
// Run libFuzzer's mutator over a std::vector in place, bounded by `cap`.
// Under NXFUZZ_STANDALONE this calls the deterministic replacement, so the
// mutator code path is still covered by the regression runner.
inline void mutate_bytes(std::vector<uint8_t> &v, size_t cap, Rng &rng) {
    if (cap == 0) {
        v.clear();
        return;
    }
    size_t old = v.size();
    v.resize(cap);
    size_t n = LLVMFuzzerMutate(v.data(), old > cap ? cap : old, cap);
    if (n > cap) n = cap;
    v.resize(n);
    // A pure-random splash now and then; libFuzzer's mutator is conservative
    // about producing bytes it has never seen in a corpus.
    if (!v.empty() && rng.chance(8)) {
        size_t i = rng.below(uint32_t(v.size()));
        v[i] = rng.edge_u8();
    }
}

// ------------------------------------------------------- soft invariants
// Some properties a component documents are not memory-safety properties: the
// spec and the implementation disagree, but nothing is read out of bounds.
// Aborting on those would mask every real crash for the rest of a run, so they
// are counted instead.  Set NXFUZZ_VERBOSE=1 to have the first occurrence of
// each printed.
//
// (A plain `++` on a volatile is deprecated in C++20 and a plain static gets
// optimized away; a relaxed atomic is neither.)
inline void note_soft_violation(const char *what) {
    static std::atomic<unsigned long> count{0};
    unsigned long n = count.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) {
        static const bool verbose = std::getenv("NXFUZZ_VERBOSE") != nullptr;
        if (verbose) std::fprintf(stderr, "nxfuzz: soft invariant violated: %s\n", what);
    }
}

// Emit `out`, truncated to max_size.  Every custom mutator ends with this.
inline size_t emit(const std::vector<uint8_t> &out, uint8_t *data, size_t max_size) {
    size_t n = out.size() < max_size ? out.size() : max_size;
    if (n) std::memcpy(data, out.data(), n);
    return n;
}

}  // namespace nxf

// ------------------------------------------------------------ standalone mode
#ifdef NXFUZZ_STANDALONE
// The regression runner needs to know whether the target has a custom mutator
// so it can exercise it.  Targets that define one also define this.
#endif

#endif  // NXFUZZ_H
