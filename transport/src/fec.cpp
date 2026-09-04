#include "nxvc/transport/fec.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace nxt {
namespace gf {
namespace {

struct Tables {
    std::array<uint8_t, 512> exp{};
    std::array<uint8_t, 256> log{};
    Tables() {
        uint32_t x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[size_t(i)] = uint8_t(x);
            log[x] = uint8_t(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp[size_t(i)] = exp[size_t(i - 255)];
        log[0] = 0;
    }
};
const Tables& tab() {
    static const Tables t;
    return t;
}

}  // namespace

uint8_t mul(uint8_t a, uint8_t b) {
    if (!a || !b) return 0;
    const Tables& t = tab();
    return t.exp[size_t(t.log[a]) + size_t(t.log[b])];
}

uint8_t inv(uint8_t a) {
    const Tables& t = tab();
    return t.exp[255 - size_t(t.log[a])];
}

uint8_t div(uint8_t a, uint8_t b) {
    if (!a) return 0;
    const Tables& t = tab();
    int e = int(t.log[a]) - int(t.log[b]);
    if (e < 0) e += 255;
    return t.exp[size_t(e)];
}

}  // namespace gf

uint8_t rs_gen(int j, int i) {
    // Cauchy: 1 / (x_j ^ y_i) with x in [128,131], y in [0,9] - disjoint sets, so
    // every k x k submatrix of [I;G] is invertible.
    return gf::inv(uint8_t(uint8_t(128 + j) ^ uint8_t(i)));
}

void rs_encode(int k, int m, size_t len, const uint8_t* const* data,
               uint8_t* const* parity) {
    for (int j = 0; j < m; ++j) {
        std::memset(parity[j], 0, len);
        for (int i = 0; i < k; ++i) {
            uint8_t c = rs_gen(j, i);
            if (!c) continue;
            const uint8_t* s = data[i];
            uint8_t* d = parity[j];
            for (size_t b = 0; b < len; ++b) d[b] ^= gf::mul(c, s[b]);
        }
    }
}

bool rs_decode(int k, int m, size_t len, const uint8_t* const* present_data,
               const uint8_t* const* present_parity, uint8_t* const* out) {
    std::vector<int> erased;
    for (int i = 0; i < k; ++i)
        if (!present_data[i]) erased.push_back(i);
    if (erased.empty()) return true;

    // Rows: identity rows for surviving data, generator rows for present parity.
    std::vector<int> rows;  // >=0: data index; -1-j: parity j
    std::vector<const uint8_t*> src;
    for (int i = 0; i < k; ++i)
        if (present_data[i]) { rows.push_back(i); src.push_back(present_data[i]); }
    for (int j = 0; j < m && int(rows.size()) < k; ++j)
        if (present_parity[j]) { rows.push_back(-1 - j); src.push_back(present_parity[j]); }
    if (int(rows.size()) < k) return false;

    // Build the k x k matrix mapping the k original data blocks to the chosen rows.
    std::vector<uint8_t> A(size_t(k) * k, 0);
    for (int r = 0; r < k; ++r) {
        if (rows[size_t(r)] >= 0) {
            A[size_t(r) * k + size_t(rows[size_t(r)])] = 1;
        } else {
            int j = -1 - rows[size_t(r)];
            for (int i = 0; i < k; ++i) A[size_t(r) * k + size_t(i)] = rs_gen(j, i);
        }
    }
    // Invert A by Gauss-Jordan in GF(256).
    std::vector<uint8_t> I(size_t(k) * k, 0);
    for (int i = 0; i < k; ++i) I[size_t(i) * k + size_t(i)] = 1;
    for (int c = 0; c < k; ++c) {
        int piv = -1;
        for (int r = c; r < k; ++r)
            if (A[size_t(r) * k + size_t(c)]) { piv = r; break; }
        if (piv < 0) return false;
        if (piv != c) {
            for (int t = 0; t < k; ++t) {
                std::swap(A[size_t(c) * k + size_t(t)], A[size_t(piv) * k + size_t(t)]);
                std::swap(I[size_t(c) * k + size_t(t)], I[size_t(piv) * k + size_t(t)]);
            }
            // src is deliberately NOT permuted: Gauss-Jordan on [A | I] yields
            // the inverse of the matrix as it stood when the augmentation
            // started, so row r still corresponds to the original src[r].
        }
        uint8_t iv = gf::inv(A[size_t(c) * k + size_t(c)]);
        for (int t = 0; t < k; ++t) {
            A[size_t(c) * k + size_t(t)] = gf::mul(A[size_t(c) * k + size_t(t)], iv);
            I[size_t(c) * k + size_t(t)] = gf::mul(I[size_t(c) * k + size_t(t)], iv);
        }
        for (int r = 0; r < k; ++r) {
            if (r == c) continue;
            uint8_t f = A[size_t(r) * k + size_t(c)];
            if (!f) continue;
            for (int t = 0; t < k; ++t) {
                A[size_t(r) * k + size_t(t)] ^= gf::mul(f, A[size_t(c) * k + size_t(t)]);
                I[size_t(r) * k + size_t(t)] ^= gf::mul(f, I[size_t(c) * k + size_t(t)]);
            }
        }
    }
    // data[e] = sum_r I[e][r] * src[r]
    for (int e : erased) {
        uint8_t* d = out[size_t(e)];
        if (!d) continue;
        std::memset(d, 0, len);
        for (int r = 0; r < k; ++r) {
            uint8_t c = I[size_t(e) * k + size_t(r)];
            if (!c) continue;
            const uint8_t* s = src[size_t(r)];
            for (size_t b = 0; b < len; ++b) d[b] ^= gf::mul(c, s[b]);
        }
    }
    return true;
}

// --------------------------------------------------------------- group encoder
void FecGroupEncoder::reset(int k, int m) {
    k_ = k;
    m_ = m;
    max_len_ = 0;
    blocks_.clear();
}

void FecGroupEncoder::add(std::span<const uint8_t> dg) {
    ByteVec b(dg.size() + 2);
    wr16(b.data(), uint16_t(dg.size()));
    std::memcpy(b.data() + 2, dg.data(), dg.size());
    max_len_ = std::max(max_len_, dg.size());
    blocks_.push_back(std::move(b));
}

std::vector<ByteVec> FecGroupEncoder::finish() {
    std::vector<ByteVec> out;
    if (m_ <= 0 || blocks_.empty()) return out;
    const int k = int(blocks_.size());
    const size_t ks = blocks_.size();
    size_t len = max_len_ + 2;
    std::vector<const uint8_t*> data{ks, nullptr};
    for (int i = 0; i < k; ++i) {
        blocks_[size_t(i)].resize(len, 0);
        data[size_t(i)] = blocks_[size_t(i)].data();
    }
    const size_t ms = size_t(m_);
    std::vector<ByteVec> par{ms, ByteVec(len)};
    std::vector<uint8_t*> pp{ms, nullptr};
    for (int j = 0; j < m_; ++j) pp[size_t(j)] = par[size_t(j)].data();
    rs_encode(k, m_, len, data.data(), pp.data());
    k_ = k;
    for (int j = 0; j < m_; ++j) {
        ByteVec payload(2 + len);
        wr16(payload.data(), uint16_t(max_len_));
        std::memcpy(payload.data() + 2, par[size_t(j)].data(), len);
        out.push_back(std::move(payload));
    }
    return out;
}

// --------------------------------------------------------------- group decoder
void FecGroupDecoder::reset(int k, int m) {
    k_ = k;
    m_ = m;
    present_ = 0;
    block_len_ = 0;
    recovered_ = false;
    data_.assign(size_t(k), {});
    parity_.assign(size_t(m > 0 ? m : 0), {});
    have_data_.assign(size_t(k), 0);
    have_parity_.assign(size_t(m > 0 ? m : 0), 0);
}

bool FecGroupDecoder::complete() const {
    for (uint8_t v : have_data_)
        if (!v) return false;
    return true;
}

bool FecGroupDecoder::recoverable() const { return present_ >= k_; }

void FecGroupDecoder::add_data(int idx, std::span<const uint8_t> dg) {
    if (idx < 0 || idx >= k_ || have_data_[size_t(idx)]) return;
    ByteVec b(dg.size() + 2);
    wr16(b.data(), uint16_t(dg.size()));
    std::memcpy(b.data() + 2, dg.data(), dg.size());
    data_[size_t(idx)] = std::move(b);
    have_data_[size_t(idx)] = 1;
    ++present_;
    block_len_ = std::max(block_len_, dg.size() + 2);
}

void FecGroupDecoder::add_parity(int idx, std::span<const uint8_t> payload) {
    int j = idx - k_;
    if (j < 0 || j >= m_ || payload.size() < 4 || have_parity_[size_t(j)]) return;
    size_t L = rd16(payload.data());
    if (payload.size() != 2 + L + 2) return;
    parity_[size_t(j)].assign(payload.begin() + 2, payload.end());
    have_parity_[size_t(j)] = 1;
    ++present_;
    block_len_ = std::max(block_len_, L + 2);
}

bool FecGroupDecoder::recover(std::vector<ByteVec>* out) {
    if (recovered_ || complete()) return true;
    if (!recoverable() || block_len_ == 0) return false;
    size_t len = block_len_;
    for (int i = 0; i < k_; ++i)
        if (have_data_[size_t(i)]) data_[size_t(i)].resize(len, 0);
    for (int j = 0; j < m_; ++j)
        if (have_parity_[size_t(j)]) parity_[size_t(j)].resize(len, 0);

    std::vector<const uint8_t*> pd(size_t(k_), nullptr);
    for (int i = 0; i < k_; ++i)
        if (have_data_[size_t(i)]) pd[size_t(i)] = data_[size_t(i)].data();
    std::vector<const uint8_t*> pp(size_t(m_ > 0 ? m_ : 1), nullptr);
    for (int j = 0; j < m_; ++j)
        if (have_parity_[size_t(j)]) pp[size_t(j)] = parity_[size_t(j)].data();

    std::vector<ByteVec> rec{size_t(k_), ByteVec{}};
    std::vector<uint8_t*> op(size_t(k_), nullptr);
    for (int i = 0; i < k_; ++i)
        if (!have_data_[size_t(i)]) {
            rec[size_t(i)].assign(len, 0);
            op[size_t(i)] = rec[size_t(i)].data();
        }
    if (!rs_decode(k_, m_, len, pd.data(), pp.data(), op.data())) return false;

    for (int i = 0; i < k_; ++i) {
        if (have_data_[size_t(i)]) continue;
        const ByteVec& b = rec[size_t(i)];
        size_t n = rd16(b.data());
        if (n < kHeaderBytes + kTagBytes || n + 2 > b.size()) continue;
        out->emplace_back(b.begin() + 2, b.begin() + 2 + long(n));
        data_[size_t(i)] = b;
        have_data_[size_t(i)] = 1;
    }
    recovered_ = true;
    return true;
}

}  // namespace nxt
