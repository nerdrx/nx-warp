// Reed-Solomon GF(256): field laws, erasure recovery up to the parity count,
// failure beyond it, and the whole-datagram group codec.  TRANSPORT.md 6.
#include <algorithm>
#include <cstring>

#include "nxvc/transport/fec.h"
#include "test_util.h"

using namespace nxt;

// Named helpers: `auto v = vec<T>(size_t(k));` is a vexing parse.
template <class T>
static std::vector<T> vec(size_t n, T v) { return std::vector<T>(n, v); }
template <class T>
static std::vector<T> vec(size_t n) { return std::vector<T>(n); }

static void field_laws() {
    tt::begin("GF(256) field laws");
    for (int a = 1; a < 256; ++a) {
        TT_EQ(int(gf::mul(uint8_t(a), gf::inv(uint8_t(a)))), 1);
        TT_EQ(int(gf::mul(uint8_t(a), 1)), a);
        TT_EQ(int(gf::mul(uint8_t(a), 0)), 0);
    }
    tt::Rng r(5);
    for (int i = 0; i < 5000; ++i) {
        uint8_t a = uint8_t(r.u32(256)), b = uint8_t(1 + r.u32(255)),
                c = uint8_t(r.u32(256));
        TT_EQ(int(gf::mul(a, b)), int(gf::mul(b, a)));
        TT_EQ(int(gf::mul(gf::mul(a, b), c)), int(gf::mul(a, gf::mul(b, c))));
        TT_EQ(int(gf::div(gf::mul(a, b), b)), int(a));
    }
    tt::end();
}

// Every k-subset of the systematic Cauchy matrix must be invertible: with m
// parity blocks, any m erasures are recoverable, exhaustively for small k.
static void erasure_recovery_exhaustive() {
    tt::begin("RS: every erasure pattern up to m is recovered");
    tt::Rng r(11);
    const size_t len = 64;
    for (int k = 1; k <= 10; ++k) {
        for (int m = 1; m <= 4; ++m) {
            auto data = vec<ByteVec>(size_t(k), ByteVec(len));
            for (auto& d : data)
                for (auto& b : d) b = uint8_t(r.u32(256));
            auto par = vec<ByteVec>(size_t(m), ByteVec(len));
            auto dp = vec<const uint8_t*>(size_t(k));
            auto pp = vec<uint8_t*>(size_t(m));
            for (int i = 0; i < k; ++i) dp[size_t(i)] = data[size_t(i)].data();
            for (int j = 0; j < m; ++j) pp[size_t(j)] = par[size_t(j)].data();
            rs_encode(k, m, len, dp.data(), pp.data());

            // All subsets of erased data blocks of size <= m.
            for (uint32_t mask = 0; mask < (1u << k); ++mask) {
                int lost = 0;
                for (int i = 0; i < k; ++i) lost += (mask >> i) & 1;
                if (lost == 0 || lost > m) continue;
                auto have = vec<const uint8_t*>(size_t(k));
                for (int i = 0; i < k; ++i)
                    have[size_t(i)] = ((mask >> i) & 1) ? nullptr : data[size_t(i)].data();
                auto hp = vec<const uint8_t*>(size_t(m));
                for (int j = 0; j < m; ++j) hp[size_t(j)] = par[size_t(j)].data();
                auto out = vec<ByteVec>(size_t(k), ByteVec(len));
                auto op = vec<uint8_t*>(size_t(k));
                for (int i = 0; i < k; ++i) op[size_t(i)] = out[size_t(i)].data();
                TT_CHECK(rs_decode(k, m, len, have.data(), hp.data(), op.data()));
                for (int i = 0; i < k; ++i)
                    if ((mask >> i) & 1)
                        TT_CHECK(std::memcmp(out[size_t(i)].data(), data[size_t(i)].data(),
                                             len) == 0);
            }
            if (k > 6) break;  // 2^k subsets: keep the exhaustive sweep bounded
        }
    }
    tt::end();
}

static void erasure_beyond_parity_fails() {
    tt::begin("RS: m+1 erasures are not recoverable");
    const int k = 10, m = 2;
    const size_t len = 32;
    tt::Rng r(3);
    auto data = vec<ByteVec>(size_t(k), ByteVec(len));
    for (auto& d : data)
        for (auto& b : d) b = uint8_t(r.u32(256));
    auto par = vec<ByteVec>(size_t(m), ByteVec(len));
    auto dp = vec<const uint8_t*>(size_t(k));
    auto pp = vec<uint8_t*>(size_t(m));
    for (int i = 0; i < k; ++i) dp[size_t(i)] = data[size_t(i)].data();
    for (int j = 0; j < m; ++j) pp[size_t(j)] = par[size_t(j)].data();
    rs_encode(k, m, len, dp.data(), pp.data());
    std::vector<const uint8_t*> have = dp;
    have[0] = have[1] = have[2] = nullptr;
    std::vector<const uint8_t*> hp{par[0].data(), par[1].data()};
    auto out = vec<ByteVec>(size_t(k), ByteVec(len));
    auto op = vec<uint8_t*>(size_t(k));
    for (int i = 0; i < k; ++i) op[size_t(i)] = out[size_t(i)].data();
    TT_CHECK(!rs_decode(k, m, len, have.data(), hp.data(), op.data()));
    tt::end();
}

// The group codec protects whole datagrams of differing lengths.
static void group_codec() {
    tt::begin("FEC group: whole datagrams, ragged lengths");
    tt::Rng r(2026);
    for (int iter = 0; iter < 300; ++iter) {
        int k = 1 + int(r.u32(10));
        int m = 1 + int(r.u32(4));
        std::vector<ByteVec> dgs;
        for (int i = 0; i < k; ++i) {
            size_t n = kHeaderBytes + kTagBytes + r.u32(1200);
            ByteVec d(n);
            for (auto& b : d) b = uint8_t(r.u32(256));
            dgs.push_back(std::move(d));
        }
        FecGroupEncoder enc;
        enc.reset(k, m);
        for (auto& d : dgs) enc.add(std::span<const uint8_t>(d.data(), d.size()));
        std::vector<ByteVec> parity = enc.finish();
        TT_EQ(int(parity.size()), m);
        // Every parity datagram fits the MTU (TRANSPORT.md 5.1).
        for (auto& p : parity) TT_CHECK(kHeaderBytes + p.size() + kTagBytes <= 1400);

        int lose = std::min(k, int(r.u32(uint32_t(m) + 1)));
        std::vector<int> victims;
        for (int i = 0; i < k; ++i) victims.push_back(i);
        for (int i = 0; i < lose; ++i) {
            uint32_t j = r.u32(uint32_t(victims.size()));
            victims.erase(victims.begin() + j);
        }
        FecGroupDecoder dec;
        dec.reset(k, m);
        for (int i : victims)
            dec.add_data(i, std::span<const uint8_t>(dgs[size_t(i)].data(),
                                                     dgs[size_t(i)].size()));
        for (int j = 0; j < m; ++j)
            dec.add_parity(k + j, std::span<const uint8_t>(parity[size_t(j)].data(),
                                                           parity[size_t(j)].size()));
        std::vector<ByteVec> rec;
        TT_CHECK(dec.recover(&rec));
        TT_EQ(int(rec.size()), lose);
        // Every recovered datagram must be byte-identical to the original.
        for (const ByteVec& got : rec) {
            bool found = false;
            for (const ByteVec& want : dgs)
                if (want == got) { found = true; break; }
            TT_CHECK(found);
        }
    }
    tt::end();
}

static void group_unrecoverable() {
    tt::begin("FEC group: fewer than k blocks stays unrecoverable");
    tt::Rng r(1);
    const int k = 10, m = 1;
    std::vector<ByteVec> dgs;
    for (int i = 0; i < k; ++i) {
        ByteVec d(kHeaderBytes + kTagBytes + 100);
        for (auto& b : d) b = uint8_t(r.u32(256));
        dgs.push_back(std::move(d));
    }
    FecGroupEncoder enc;
    enc.reset(k, m);
    for (auto& d : dgs) enc.add(std::span<const uint8_t>(d.data(), d.size()));
    auto parity = enc.finish();
    FecGroupDecoder dec;
    dec.reset(k, m);
    for (int i = 2; i < k; ++i)
        dec.add_data(i, std::span<const uint8_t>(dgs[size_t(i)].data(), dgs[size_t(i)].size()));
    dec.add_parity(k, std::span<const uint8_t>(parity[0].data(), parity[0].size()));
    TT_CHECK(!dec.recoverable());
    std::vector<ByteVec> rec;
    TT_CHECK(!dec.recover(&rec));
    TT_EQ(int(rec.size()), 0);
    tt::end();
}

int main() {
    field_laws();
    erasure_recovery_exhaustive();
    erasure_beyond_parity_fails();
    group_codec();
    group_unrecoverable();
    return tt::report("transport.fec");
}
