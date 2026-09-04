// nxvc_transport - pluggable AEAD.  Normative: docs/TRANSPORT.md section 4.
//
// The library never generates or exchanges keys.  The integration supplies a
// session key and salt from the WiVRn NX handshake and picks a backend.
#ifndef NXVC_TRANSPORT_AEAD_H
#define NXVC_TRANSPORT_AEAD_H

#include <array>
#include <memory>

#include "nxvc/transport/common.h"

namespace nxt {

using Key = std::array<uint8_t, kKeyBytes>;
using Nonce = std::array<uint8_t, kNonceBytes>;

enum class Direction : uint8_t { kDownstream = 0, kUpstream = 1 };

// TRANSPORT.md 4.2.
Nonce derive_nonce(uint8_t stream_id, uint8_t path_id, uint16_t epoch,
                   uint64_t path_seq_ext);

// HKDF-SHA256, always available (a small self-contained SHA-256 lives in aead.cpp).
Key derive_subkey(const Key& session_key, const Key& session_salt, uint8_t path_id,
                  Direction dir);

class Aead {
  public:
    virtual ~Aead() = default;
    virtual const char* name() const = 0;
    // out must have room for plaintext.size() + kTagBytes.  Returns bytes written.
    virtual size_t seal(const Key& key, const Nonce& nonce,
                        std::span<const uint8_t> aad,
                        std::span<const uint8_t> plaintext, uint8_t* out) const = 0;
    // Returns the plaintext length, or SIZE_MAX on tag failure.
    virtual size_t open(const Key& key, const Nonce& nonce,
                        std::span<const uint8_t> aad,
                        std::span<const uint8_t> ciphertext_and_tag,
                        uint8_t* out) const = 0;
};

// Test / simulator backend.  Keyed but NOT cryptographic; refuses nothing but
// detects corruption.  Never use it on a real link.
std::unique_ptr<Aead> make_null_aead();

// Real backends, present only when OpenSSL or libsodium was found at configure
// time.  Return nullptr otherwise.
std::unique_ptr<Aead> make_aes256gcm();
std::unique_ptr<Aead> make_chacha20poly1305();

// Best available: AES-256-GCM, else ChaCha20-Poly1305, else nullptr.
std::unique_ptr<Aead> make_default_aead();

bool has_real_aead();

}  // namespace nxt

#endif
