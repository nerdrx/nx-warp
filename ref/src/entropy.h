// Interleaved rANS (Duda/Giesen) and the per-lane coefficient syntax machine.
// The machine is shared by the encoder and the decoder so the two can never
// disagree; the GPU Pass A shader runs the identical state machine.
#pragma once
#include "common.h"

namespace nxvc {

constexpr u32 kRansL = 1u << 16;      // lower bound of the state interval
constexpr u32 kRansMaxLanes = 32;
// The renormalization guard: x < freq << kRansShift keeps the encoded state
// below 2^32 for every legal freq <= kProbTotal - 15.
constexpr u32 kRansShift = 32 - kProbBits;

// ------------------------------------------------------------- coding units
enum UnitKind : u8 { UNIT_COEF = 0, UNIT_MODE = 1 };

// The intra mode of block `b` is coded relative to a most-probable mode
// derived from the already-coded left and above modes of the same plane.
// Both are inside the same unit, so the derivation only ever looks at data the
// lane has already produced, whatever the interleaved lane schedule does.
int mpm_of(const u8 *modes, int nbx, int b);
// The `nmodes - 1` modes other than `mpm`, ascending: index -> mode.
int nonmpm_mode(int mpm, int idx, int nmodes);
int nonmpm_index(int mpm, int mode, int nmodes);

struct Unit {
    i16 *coef;        // UNIT_COEF: ncoef quantized levels, block-local order
    const u16 *scan;  // scan_pos -> block-local index
    u16 ncoef;        // up to 1024 (a 32x32 block)
    u8 ctx_cbf;       // kCtxCbfLuma / kCtxCbfChroma / kCtxCbfDc
    u8 ctx_last;      // kCtxLastLuma / kCtxLastChroma / kCtxLastDc
    u8 ctx_level;     // kCtxNone = the banded LEVEL contexts; else a context
    u8 kind;          // UnitKind
    u8 *modes;        // UNIT_MODE: nbx*nbx intra modes, raster order
    u8 nbx;           // UNIT_MODE: blocks per edge
    u8 nmodes;        // UNIT_MODE: mode alphabet, kNumIntraModes or +1 (CFL)
    u8 ctx_mode;      // UNIT_MODE: kCtxNone = bypass coded, else a context
    u8 sdh;           // UNIT_COEF: 1 = sign data hiding applies
    // Tool bit 19.  `split_present` means the unit codes a 4x4-split flag
    // after a nonzero CBF; `split_out` is where that flag is read from (the
    // encoder) and written to (the decoder).  The flag lives in the *block's
    // own* unit rather than in the mode unit because the two can fall in
    // different rANS lanes, and a unit's syntax may only depend on values its
    // own lane has already produced (SYNTAX.md 9.1).
    u8 split_present;
    u8 *split_out;
    // Tool bit 25 (CTX_V3).  `ucls` is the unit's statistical class, derived
    // by both sides from the unit's position and never transmitted; `ctx_v3`
    // says to derive CBF/LAST from it and the lane's own previous unit.
    // These live beside the split fields on purpose: one coding unit carries
    // both the detail package's per-block split flag and the entropy
    // package's lane conditioning state, and neither reads the other.
    u8 ucls;          // kUclsLuma / kUclsChroma / kUclsDc
    u8 ctx_v3;        // 1 = derive CBF/LAST from ucls and the lane state
    u8 ngrp;          // v3: neighbour group (plane + 1) for block units, 0
                      // for DC-plane and mode units.  The lane's neighbour
                      // class is carried within a group and reset between
                      // groups, so it never leaks across a plane boundary.
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
        kCbf, kSplit, kLast, kLastRaw, kLevel, kEscPrefix, kEscSuffix, kSign,
        kModeSym, kModeFlag, kModeIdx, kHidden, kDone
    };
    void begin_next_unit();
    void mode_commit(int mode);
    void after_cbf();
    void store_magnitude();
    void begin_unit();
    void begin_levels();
    void advance_pos();
    void finish_coef_unit(int cbf);
    int ctx_cbf() const;
    int ctx_last() const;
    int ctx_level(int scan_pos, int band_scan_pos, int prev_class) const;

    const Unit *units_ = nullptr;
    int nunits_ = 0, ui_ = 0, stride_ = 1;
    bool encoding_ = false;
    Phase phase_ = kDone;

    const Unit *u_ = nullptr;
    const u16 *scan_ = nullptr;  // u_->scan, or the split scan when split_
    bool split_ = false;
    int last_ = 0, pos_ = 0, prev_class_ = 0;
    // last_shift_of(ncoef): 0 for every unit of at most 64 coefficients, 2
    // for a 16x16 block and 4 for a 32x32 one.  It scales the LAST class
    // table and the LEVEL bands to the unit's size (common.h).
    int lshift_ = 0;
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
    // v3 neighbour state: the class of the previous coefficient unit this
    // lane finished in the current group, and which group that was.  Two
    // registers, written once per unit and read once per unit; no cross-lane
    // traffic and no barrier.  SYNTAX.md 9.9.
    u8 nbr_ = 0;
    u8 ngrp_ = 0;
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
