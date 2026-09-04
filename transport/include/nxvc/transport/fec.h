// nxvc_transport - Reed-Solomon over GF(256).  Normative: docs/TRANSPORT.md 6.
#ifndef NXVC_TRANSPORT_FEC_H
#define NXVC_TRANSPORT_FEC_H

#include "nxvc/transport/common.h"

namespace nxt {
namespace gf {

// GF(256) with 0x11D, generator 0x02.  Tables are built once, thread-safely.
uint8_t mul(uint8_t a, uint8_t b);
uint8_t div(uint8_t a, uint8_t b);
uint8_t inv(uint8_t a);

}  // namespace gf

// Systematic Cauchy generator: parity row j, data column i -> 1/((128+j) ^ i).
uint8_t rs_gen(int j, int i);

// Encode m parity blocks from k data blocks, each `len` bytes.
// data[i] must be non-null and `len` long; parity[j] receives `len` bytes.
void rs_encode(int k, int m, size_t len, const uint8_t* const* data,
               uint8_t* const* parity);

// Recover erased data blocks.
//   present_data[i]   : pointer to data block i, or nullptr if erased
//   present_parity[j] : pointer to parity block j, or nullptr if erased
//   out[i]            : buffer of `len` bytes for erased data block i (may be null
//                       for non-erased blocks)
// Returns true if every erasure was recovered.
bool rs_decode(int k, int m, size_t len, const uint8_t* const* present_data,
               const uint8_t* const* present_parity, uint8_t* const* out);

// ------------------------------------------------------------- group builder
// A group of complete datagrams protected as one unit (TRANSPORT.md 6.1).
class FecGroupEncoder {
  public:
    void reset(int k, int m);
    // `datagram` is the complete on-wire datagram (header || ciphertext || tag).
    void add(std::span<const uint8_t> datagram);
    int size() const { return int(blocks_.size()); }
    int k() const { return k_; }
    int m() const { return m_; }
    size_t max_len() const { return max_len_; }
    // Produces m parity payloads, each `u16 L || parity block (L+2 bytes)`.
    // Empty when m == 0 or no datagrams were added.
    std::vector<ByteVec> finish();

  private:
    int k_ = 0, m_ = 0;
    size_t max_len_ = 0;
    std::vector<ByteVec> blocks_;
};

// Receiver-side group.  Blocks are added as they arrive, by fec_idx.
class FecGroupDecoder {
  public:
    void reset(int k, int m);
    bool complete() const;              // all k data slots present
    bool recoverable() const;           // >= k blocks present
    int present() const { return present_; }
    void add_data(int idx, std::span<const uint8_t> datagram);
    // `payload` is the decrypted parity payload: u16 L || block(L+2).
    void add_parity(int idx, std::span<const uint8_t> payload);
    // Recovers missing data datagrams.  Appends complete datagrams to `out`.
    // Returns false if the group is unrecoverable.
    bool recover(std::vector<ByteVec>* out);
    bool has_data(int i) const { return i < int(have_data_.size()) && have_data_[i]; }
    // v2: `m` is on the wire, so recovery can wait for real evidence of loss
    // instead of firing whenever k blocks happen to be in hand.
    bool has_any_parity() const {
        for (uint8_t v : have_parity_)
            if (v) return true;
        return false;
    }
    bool all_blocks_seen() const { return present_ >= k_ + m_; }
    // Highest fec_idx seen.  Once the group's last parity block has arrived,
    // everything still missing was lost rather than reordered, because parity
    // is transmitted after its group's data on the same path.
    int highest_idx() const { return highest_idx_; }
    bool tail_seen() const { return highest_idx_ >= k_ + m_ - 1; }

  private:
    int k_ = 0, m_ = 0, present_ = 0, highest_idx_ = -1;
    size_t block_len_ = 0;  // L + 2
    std::vector<ByteVec> data_, parity_;
    std::vector<uint8_t> have_data_, have_parity_;
    bool recovered_ = false;
};

}  // namespace nxt

#endif
