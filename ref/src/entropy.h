// Interleaved rANS (Duda/Giesen) and the per-lane coefficient syntax machine.
// The machine is shared by the encoder and the decoder so the two can never
// disagree; the GPU Pass A shader runs the identical state machine.
#pragma once
#include "common.h"

namespace nxvc {

constexpr u32 kRansL = 1u << 16;      // lower bound of the state interval
constexpr u32 kProbBits = 10;         // M = 2^10
constexpr u32 kRansMaxLanes = 32;

// ------------------------------------------------------------- coding units
struct Unit {
    i16 *coef;        // ncoef quantized levels, block-local order
    const u8 *scan;   // scan_pos -> block-local index
    u16 ncoef;
    u8 ctx_cbf;       // kCtxCbfLuma / kCtxCbfChroma
    u8 ctx_last;      // kCtxLastLuma / kCtxLastChroma
};

// --------------------------------------------------------------- lane ops
enum OpKind : u8 { OP_SYM = 0, OP_BYPASS = 1 };
struct Op {
    u8 kind;
    u8 arg;    // OP_SYM: context index.  OP_BYPASS: bit count (1..8).
    u16 value; // encoder side value
};

class LaneMachine {
  public:
    void init(const Unit *units, int nunits, int lane, int nlanes,
              bool encoding);
    bool done() const { return phase_ == kDone; }
    // Produces the next operation.  Returns false when the lane is finished.
    bool next(Op &op);
    // Applies the coded/decoded value.  Returns false on an illegal value.
    bool feed(u32 v);

  private:
    enum Phase : u8 {
        kCbf, kLast, kLastRaw, kLevel, kEscPrefix, kEscSuffix, kSign, kDone
    };
    void begin_unit();
    void begin_levels();
    void advance_pos();

    const Unit *units_ = nullptr;
    int nunits_ = 0, ui_ = 0, stride_ = 1;
    bool encoding_ = false;
    Phase phase_ = kDone;

    const Unit *u_ = nullptr;
    int last_ = 0, pos_ = 0, prev_class_ = 0;
    int last_cls_ = 0;
    i32 mag_ = 0;
    int esc_j_ = 0;          // escape prefix length so far
    int esc_bits_ = 0;       // total suffix bits
    int esc_done_ = 0;       // suffix bits consumed
    u32 esc_acc_ = 0;
};

// ------------------------------------------------------------ rANS decoder
class RansDecoder {
  public:
    bool init(const u8 *buf, size_t len, int nlanes);
    // Returns false on truncation.
    bool decode_sym(int lane, const CtxTable &t, u32 &sym);
    bool decode_bypass(int lane, int nbits, u32 &val);
    size_t consumed() const { return pos_; }

  private:
    bool renorm(u32 &x);
    const u8 *buf_ = nullptr;
    size_t len_ = 0, pos_ = 0;
    int nlanes_ = 8;
    u32 state_[kRansMaxLanes] = {};
};

// ------------------------------------------------------- tile payload glue
// Walks the interleaved schedule over `nlanes` lane machines.  Encoding
// records the global op order; decoding drives the rANS decoder.
bool encode_units(const Unit *units, int nunits, int nlanes,
                  const TableSet &tabs, std::vector<u8> &out);
bool decode_units(const Unit *units, int nunits, int nlanes,
                  const TableSet &tabs, const u8 *buf, size_t len);
// Histograms the symbols a tile would emit, for custom table derivation.
bool count_units(const Unit *units, int nunits, int nlanes,
                 u32 hist[kNumCtx][kNumSym], u32 *op_count);

}  // namespace nxvc
