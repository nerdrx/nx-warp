#include "nxvc/transport/aead.h"

#include <cstring>

#if defined(NXT_HAVE_OPENSSL)
#include <openssl/evp.h>
#endif
#if defined(NXT_HAVE_SODIUM)
#include <sodium.h>
#endif

namespace nxt {
namespace {

// ------------------------------------------------------------------ SHA-256
struct Sha256 {
    uint32_t h[8];
    uint64_t len = 0;
    uint8_t buf[64];
    size_t buflen = 0;

    Sha256() {
        static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                       0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                       0x1f83d9abu, 0x5be0cd19u};
        std::memcpy(h, iv, sizeof(h));
    }
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
            0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
            0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
            0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
            0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
            0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
            0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
            0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
            0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
                   (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            size_t take = 64 - buflen;
            if (take > n) take = n;
            std::memcpy(buf + buflen, p, take);
            buflen += take; p += take; n -= take;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }
    void final(uint8_t out[32]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (buflen != 56) update(&z, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = uint8_t(bits >> (56 - 8 * i));
        update(lb, 8);
        for (int i = 0; i < 8; ++i) {
            out[4 * i + 0] = uint8_t(h[i] >> 24);
            out[4 * i + 1] = uint8_t(h[i] >> 16);
            out[4 * i + 2] = uint8_t(h[i] >> 8);
            out[4 * i + 3] = uint8_t(h[i]);
        }
    }
};

void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* const* msgs,
                 const size_t* lens, size_t nmsg, uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (klen > 64) {
        Sha256 s; s.update(key, klen); s.final(k);
    } else {
        std::memcpy(k, key, klen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = uint8_t(k[i] ^ 0x36); opad[i] = uint8_t(k[i] ^ 0x5c); }
    Sha256 inner;
    inner.update(ipad, 64);
    for (size_t i = 0; i < nmsg; ++i) inner.update(msgs[i], lens[i]);
    uint8_t ih[32];
    inner.final(ih);
    Sha256 outer;
    outer.update(opad, 64);
    outer.update(ih, 32);
    outer.final(out);
}

// ------------------------------------------------------------- NullAead
// Encrypt-then-MAC: SHA-256 counter-mode keystream, HMAC-SHA256 tag truncated
// to 16 bytes.  Self-contained so the library and its tests have zero deps.
class NullAead final : public Aead {
  public:
    const char* name() const override { return "null-sha256ctr"; }

    static void keystream(const Key& key, const Nonce& nonce, uint8_t* out, size_t n) {
        uint32_t ctr = 0;
        while (n) {
            Sha256 s;
            s.update(key.data(), key.size());
            s.update(nonce.data(), nonce.size());
            uint8_t cb[4] = {uint8_t(ctr), uint8_t(ctr >> 8), uint8_t(ctr >> 16),
                             uint8_t(ctr >> 24)};
            s.update(cb, 4);
            uint8_t blk[32];
            s.final(blk);
            size_t take = n < 32 ? n : 32;
            std::memcpy(out, blk, take);
            out += take; n -= take; ++ctr;
        }
    }

    static void tag(const Key& key, const Nonce& nonce, std::span<const uint8_t> aad,
                    const uint8_t* ct, size_t ctlen, uint8_t out[kTagBytes]) {
        uint8_t lenblk[16];
        wr64(lenblk, uint64_t(aad.size()));
        wr64(lenblk + 8, uint64_t(ctlen));
        const uint8_t* msgs[4] = {nonce.data(), aad.data(), ct, lenblk};
        const size_t lens[4] = {nonce.size(), aad.size(), ctlen, 16};
        uint8_t full[32];
        hmac_sha256(key.data(), key.size(), msgs, lens, 4, full);
        std::memcpy(out, full, kTagBytes);
    }

    size_t seal(const Key& key, const Nonce& nonce, std::span<const uint8_t> aad,
                std::span<const uint8_t> pt, uint8_t* out) const override {
        std::vector<uint8_t> ks(pt.size());
        keystream(key, nonce, ks.data(), ks.size());
        for (size_t i = 0; i < pt.size(); ++i) out[i] = uint8_t(pt[i] ^ ks[i]);
        tag(key, nonce, aad, out, pt.size(), out + pt.size());
        return pt.size() + kTagBytes;
    }

    size_t open(const Key& key, const Nonce& nonce, std::span<const uint8_t> aad,
                std::span<const uint8_t> ct, uint8_t* out) const override {
        if (ct.size() < kTagBytes) return SIZE_MAX;
        size_t n = ct.size() - kTagBytes;
        uint8_t want[kTagBytes];
        tag(key, nonce, aad, ct.data(), n, want);
        uint8_t diff = 0;
        for (size_t i = 0; i < kTagBytes; ++i) diff |= uint8_t(want[i] ^ ct[n + i]);
        if (diff) return SIZE_MAX;
        std::vector<uint8_t> ks(n);
        keystream(key, nonce, ks.data(), n);
        for (size_t i = 0; i < n; ++i) out[i] = uint8_t(ct[i] ^ ks[i]);
        return n;
    }
};

#if defined(NXT_HAVE_OPENSSL)
class EvpAead final : public Aead {
  public:
    explicit EvpAead(const EVP_CIPHER* c, const char* n) : c_(c), n_(n) {}
    const char* name() const override { return n_; }

    size_t seal(const Key& key, const Nonce& nonce, std::span<const uint8_t> aad,
                std::span<const uint8_t> pt, uint8_t* out) const override {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        int len = 0;
        EVP_EncryptInit_ex(ctx, c_, nullptr, nullptr, nullptr);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, int(kNonceBytes), nullptr);
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data());
        if (!aad.empty())
            EVP_EncryptUpdate(ctx, nullptr, &len, aad.data(), int(aad.size()));
        int total = 0;
        if (!pt.empty()) {
            EVP_EncryptUpdate(ctx, out, &len, pt.data(), int(pt.size()));
            total = len;
        }
        EVP_EncryptFinal_ex(ctx, out + total, &len);
        total += len;
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, int(kTagBytes), out + total);
        EVP_CIPHER_CTX_free(ctx);
        return size_t(total) + kTagBytes;
    }

    size_t open(const Key& key, const Nonce& nonce, std::span<const uint8_t> aad,
                std::span<const uint8_t> ct, uint8_t* out) const override {
        if (ct.size() < kTagBytes) return SIZE_MAX;
        size_t n = ct.size() - kTagBytes;
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        int len = 0;
        EVP_DecryptInit_ex(ctx, c_, nullptr, nullptr, nullptr);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, int(kNonceBytes), nullptr);
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data());
        if (!aad.empty())
            EVP_DecryptUpdate(ctx, nullptr, &len, aad.data(), int(aad.size()));
        int total = 0;
        if (n) {
            EVP_DecryptUpdate(ctx, out, &len, ct.data(), int(n));
            total = len;
        }
        uint8_t tagcopy[kTagBytes];
        std::memcpy(tagcopy, ct.data() + n, kTagBytes);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, int(kTagBytes), tagcopy);
        int rc = EVP_DecryptFinal_ex(ctx, out + total, &len);
        EVP_CIPHER_CTX_free(ctx);
        if (rc <= 0) return SIZE_MAX;
        return size_t(total) + size_t(len);
    }

  private:
    const EVP_CIPHER* c_;
    const char* n_;
};
#endif

#if !defined(NXT_HAVE_OPENSSL) && defined(NXT_HAVE_SODIUM)
// libsodium fallback.  Both of its AEADs use a 12-byte nonce and a 16-byte tag,
// matching our wire format exactly.
class SodiumAead final : public Aead {
  public:
    enum class Kind { kAesGcm, kChaCha };
    explicit SodiumAead(Kind k) : k_(k) { sodium_init(); }
    const char* name() const override {
        return k_ == Kind::kAesGcm ? "aes-256-gcm(sodium)"
                                   : "chacha20-poly1305-ietf(sodium)";
    }
    size_t seal(const Key& key, const Nonce& nonce, std::span<const uint8_t> aad,
                std::span<const uint8_t> pt, uint8_t* out) const override {
        unsigned long long clen = 0;
        if (k_ == Kind::kAesGcm)
            crypto_aead_aes256gcm_encrypt(out, &clen, pt.data(), pt.size(), aad.data(),
                                          aad.size(), nullptr, nonce.data(), key.data());
        else
            crypto_aead_chacha20poly1305_ietf_encrypt(
                out, &clen, pt.data(), pt.size(), aad.data(), aad.size(), nullptr,
                nonce.data(), key.data());
        return size_t(clen);
    }
    size_t open(const Key& key, const Nonce& nonce, std::span<const uint8_t> aad,
                std::span<const uint8_t> ct, uint8_t* out) const override {
        unsigned long long mlen = 0;
        int rc;
        if (k_ == Kind::kAesGcm)
            rc = crypto_aead_aes256gcm_decrypt(out, &mlen, nullptr, ct.data(), ct.size(),
                                               aad.data(), aad.size(), nonce.data(),
                                               key.data());
        else
            rc = crypto_aead_chacha20poly1305_ietf_decrypt(
                out, &mlen, nullptr, ct.data(), ct.size(), aad.data(), aad.size(),
                nonce.data(), key.data());
        return rc == 0 ? size_t(mlen) : SIZE_MAX;
    }

  private:
    Kind k_;
};
#endif

}  // namespace

Nonce derive_nonce(uint8_t stream_id, uint8_t path_id, uint16_t epoch,
                   uint64_t path_seq_ext) {
    Nonce n{};
    n[0] = stream_id;
    n[1] = uint8_t(path_id & 0x03);
    wr16(n.data() + 2, epoch);
    wr64(n.data() + 4, path_seq_ext);
    return n;
}

Key derive_subkey(const Key& session_key, const Key& session_salt, uint8_t path_id,
                  Direction dir) {
    // HKDF-SHA256 extract + one expand block (32 bytes out).
    uint8_t prk[32];
    {
        const uint8_t* msgs[1] = {session_key.data()};
        const size_t lens[1] = {session_key.size()};
        hmac_sha256(session_salt.data(), session_salt.size(), msgs, lens, 1, prk);
    }
    static const char kInfo[] = "nxvc-transport/v1";
    uint8_t info[sizeof(kInfo) - 1 + 2];
    std::memcpy(info, kInfo, sizeof(kInfo) - 1);
    info[sizeof(kInfo) - 1] = uint8_t(path_id & 0x03);
    info[sizeof(kInfo)] = uint8_t(dir);
    uint8_t one = 1;
    const uint8_t* msgs[2] = {info, &one};
    const size_t lens[2] = {sizeof(info), 1};
    Key out{};
    hmac_sha256(prk, 32, msgs, lens, 2, out.data());
    return out;
}

std::unique_ptr<Aead> make_null_aead() { return std::make_unique<NullAead>(); }

std::unique_ptr<Aead> make_aes256gcm() {
#if defined(NXT_HAVE_OPENSSL)
    return std::make_unique<EvpAead>(EVP_aes_256_gcm(), "aes-256-gcm");
#else
    return nullptr;
#endif
}

std::unique_ptr<Aead> make_chacha20poly1305() {
#if defined(NXT_HAVE_OPENSSL)
    return std::make_unique<EvpAead>(EVP_chacha20_poly1305(), "chacha20-poly1305");
#else
    return nullptr;
#endif
}

std::unique_ptr<Aead> make_default_aead() {
    if (auto a = make_aes256gcm()) return a;
    if (auto a = make_chacha20poly1305()) return a;
    return nullptr;
}

bool has_real_aead() {
#if defined(NXT_HAVE_OPENSSL)
    return true;
#else
    return false;
#endif
}

}  // namespace nxt
