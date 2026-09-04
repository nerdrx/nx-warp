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
enum UnitKind : u8 { UNIT_COEF = 0, UNIT_MODE = 1 };

// The intra mode of block `b` is coded relative to a most-probable mode
// derived from the already-coded left and above modes of the same plane.
// Both are inside the same unit, so the derivation only ever looks at data the
// lane has already produced, whatever the interleaved lane schedule does.
int mpm_of(const u8 *modes, int nbx, int b);
// The 8 modes other than `mpm`, ascending: index -> mode.
int nonmpm_mode(int mpm, int idx);
int nonmpm_index(int mpm, int mode);

struct Unit {
    i16 *coef;        // UNIT_COEF: ncoef quantized levels, block-local order
    const u8 *scan;   // scan_pos -> block-local index
    u16 ncoef;
    u8 ctx_cbf;       // kCtxCbfLuma / kCtxCbfChroma / kCtxCbfDc
    u8 ctx_last;      // kCtxLastLuma / kCtxLastChroma / kCtxLastDc
    u8 ctx_level;     // kCtxNone = the banded LEVEL contexts; else a context
    u8 kind;          // UnitKind
    u8 *modes;        // UNIT_MODE: nbx*nbx intra modes, raster order
    u8 nbx;           // UNIT_MODE: blocks per edge
    u8 ctx_mode;      // UNIT_MODE: kCtxNone = bypass coded, else a context
    u8 sdh;           // UNIT_COEF: 1 = sign data hiding applies
    u8 band_min;      // UNIT_COEF: LEVEL band floor (SYNTAX.md 9.3); 0 in v1
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
        kCbf, kLast, kLastRaw, kLevel, kEscPrefix, kEscSuffix, kSign,
        kModeSym, kModeFlag, kModeIdx, kHidden, kDone
    };
    void begin_next_unit();
    void store_magnitude();
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
    int mb_ = 0;             // UNIT_MODE: block being coded
    int mpm_ = 0;
    i32 sum_abs_ = 0;        // sign data hiding: sum of |level| in the unit
    bool hide_ = false;      // ... and whether this unit hides a sign
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
