#include "entropy.h"

namespace nxvc {

// ------------------------------------------------------------ escape codes
// Exp-Golomb order 3: n = v + 8, b = floor(log2(n)), j = b - 3.
// prefix = j ones then a zero, suffix = the low b bits of n.
static void eg3_encode(u32 v, int &j, u32 &suffix, int &bits) {
    u32 n = v + (1u << kEscOrder);
    int b = 0;
    while ((n >> (b + 1)) != 0) ++b;
    j = b - kEscOrder;
    bits = b;
    suffix = n - (1u << b);
}

// ------------------------------------------------------------ intra modes
int mpm_of(const u8 *modes, int nbx, int b) {
    int bx = b % nbx, by = b / nbx;
    int left = bx > 0 ? modes[b - 1] : kIntraDcPlane;
    int above = by > 0 ? modes[b - nbx] : kIntraDcPlane;
    if (left == above) return left;
    return left < above ? left : above;
}

int nonmpm_mode(int mpm, int idx) {
    int n = 0;
    for (int m = 0; m < kNumIntraModes; ++m) {
        if (m == mpm) continue;
        if (n == idx) return m;
        ++n;
    }
    return kIntraDcPlane;
}

int nonmpm_index(int mpm, int mode) {
    int n = 0;
    for (int m = 0; m < kNumIntraModes; ++m) {
        if (m == mpm) continue;
        if (m == mode) return n;
        ++n;
    }
    return 0;
}

// ------------------------------------------------------------ LaneMachine
void LaneMachine::init(const Unit *units, int nunits, int lane, int nlanes,
                       bool encoding) {
    units_ = units;
    nunits_ = nunits;
    stride_ = nlanes;
    ui_ = lane;
    encoding_ = encoding;
    if (ui_ >= nunits_) {
        phase_ = kDone;
        return;
    }
    begin_unit();
}

void LaneMachine::begin_unit() {
    u_ = &units_[ui_];
    if (u_->kind == UNIT_MODE) {
        mb_ = 0;
        if (u_->nbx == 0) { begin_next_unit(); return; }
        mpm_ = mpm_of(u_->modes, u_->nbx, 0);
        phase_ = u_->ctx_mode != kCtxNone ? kModeSym : kModeFlag;
        return;
    }
    phase_ = kCbf;
}

void LaneMachine::begin_next_unit() {
    ui_ += stride_;
    if (ui_ >= nunits_) phase_ = kDone;
    else begin_unit();
}

// ------------------------------------------------- v3 context derivation
// The three accessors are the only place a coding context is chosen.  Under
// the v1/v2 models the unit carries its contexts; under v3 they are derived
// from the unit's class and this lane's neighbour class, both of which the
// decoder already holds.  SYNTAX.md 9.8.
int LaneMachine::ctx_cbf() const {
    return u_->ctx_v3 ? v3_ctx_cbf(u_->ucls, prev_cbf_[u_->ucls]) : u_->ctx_cbf;
}
int LaneMachine::ctx_last() const {
    return u_->ctx_v3 ? v3_ctx_last(u_->ucls, prev_cbf_[u_->ucls])
                      : u_->ctx_last;
}
int LaneMachine::ctx_level(int scan_pos, int prev_class) const {
    if (u_->ctx_v3) return v3_ctx_level(u_->ucls, scan_pos, prev_class);
    return u_->ctx_level != kCtxNone ? u_->ctx_level
                                     : level_ctx(scan_pos, prev_class);
}

// A coefficient unit is over: record its CBF for the next unit this lane
// reaches in the same class, and move on.  `cbf` is a value this lane has just
// decoded, so nothing here reads another lane's state.
void LaneMachine::finish_coef_unit(int cbf) {
    prev_cbf_[u_->ucls] = (u8)cbf;
    begin_next_unit();
}

void LaneMachine::begin_levels() {
    pos_ = last_;
    prev_class_ = 0;
    sum_abs_ = 0;
    // The hidden sign is the one at scan position `last`, which is known as
    // soon as LAST is decoded and is always nonzero.  Its value is settled at
    // the end of the unit, when the parity of the whole unit is known.
    hide_ = u_->sdh != 0 && last_ >= kSdhMinLast;
    phase_ = kLevel;
}

void LaneMachine::advance_pos() {
    i32 m = mag_ < 0 ? -mag_ : mag_;
    prev_class_ = level_class((int)m);
    sum_abs_ += m;
    if (pos_ == 0) {
        if (hide_ && (sum_abs_ & 1))
            u_->coef[u_->scan[last_]] = (i16)(-u_->coef[u_->scan[last_]]);
        finish_coef_unit(1);
    } else {
        --pos_;
        phase_ = kLevel;
    }
}

bool LaneMachine::next(Op &op) {
    if (phase_ == kDone) return false;
    switch (phase_) {
        case kCbf: {
            op.kind = OP_SYM;
            op.arg = (u8)ctx_cbf();
            op.value = 0;
            if (encoding_) {
                u32 cbf = 0;
                for (int i = 0; i < u_->ncoef; ++i)
                    if (u_->coef[i] != 0) { cbf = 1; break; }
                op.value = (u16)cbf;
            }
            return true;
        }
        case kLast: {
            op.kind = OP_SYM;
            op.arg = (u8)ctx_last();
            op.value = 0;
            if (encoding_) {
                int lastpos = 0;
                for (int p = u_->ncoef - 1; p >= 0; --p)
                    if (u_->coef[u_->scan[p]] != 0) { lastpos = p; break; }
                last_ = lastpos;
                op.value = (u16)last_class_of(lastpos);
            }
            return true;
        }
        case kLastRaw: {
            op.kind = OP_BYPASS;
            op.arg = kLastRawBits[last_cls_];
            op.value = encoding_ ? (u16)(last_ - kLastBase[last_cls_]) : 0;
            return true;
        }
        case kModeSym: {
            op.kind = OP_SYM;
            op.arg = (u8)u_->ctx_mode;
            op.value = 0;
            if (encoding_) {
                int m = u_->modes[mb_];
                op.value = (u16)(m == mpm_ ? 0 : 1 + nonmpm_index(mpm_, m));
            }
            return true;
        }
        case kModeFlag: {
            op.kind = OP_BYPASS;
            op.arg = 1;
            op.value = encoding_ ? (u16)(u_->modes[mb_] == mpm_) : 0;
            return true;
        }
        case kModeIdx: {
            op.kind = OP_BYPASS;
            op.arg = 3;
            op.value =
                encoding_ ? (u16)nonmpm_index(mpm_, u_->modes[mb_]) : 0;
            return true;
        }
        case kLevel: {
            op.kind = OP_SYM;
            op.arg = (u8)ctx_level(pos_, prev_class_);
            op.value = 0;
            if (encoding_) {
                i32 q = u_->coef[u_->scan[pos_]];
                i32 m = q < 0 ? -q : q;
                op.value = (u16)(m > 14 ? kEscSym : m);
            }
            return true;
        }
        case kEscPrefix: {
            op.kind = OP_BYPASS;
            op.arg = 1;
            op.value = 0;
            if (encoding_) {
                i32 q = u_->coef[u_->scan[pos_]];
                i32 m = q < 0 ? -q : q;
                int j; u32 suf; int bits;
                eg3_encode((u32)(m - 15), j, suf, bits);
                op.value = (u16)(esc_j_ < j ? 1 : 0);
            }
            return true;
        }
        case kEscSuffix: {
            int nchunks = (esc_bits_ + 7) / 8;
            int chunk = esc_done_ == 0 ? esc_bits_ - 8 * (nchunks - 1) : 8;
            op.kind = OP_BYPASS;
            op.arg = (u8)chunk;
            op.value = 0;
            if (encoding_) {
                i32 q = u_->coef[u_->scan[pos_]];
                i32 m = q < 0 ? -q : q;
                int j; u32 suf; int bits;
                eg3_encode((u32)(m - 15), j, suf, bits);
                int shift = esc_bits_ - esc_done_ - chunk;
                op.value = (u16)((suf >> shift) & ((1u << chunk) - 1));
            }
            return true;
        }
        case kSign: {
            op.kind = OP_BYPASS;
            op.arg = 1;
            op.value = 0;
            if (encoding_) op.value = (u16)(u_->coef[u_->scan[pos_]] < 0);
            return true;
        }
        case kHidden:
            return false;  // unreachable: kHidden never asks for an operation
        default:
            return false;
    }
}

// A magnitude has been decoded at pos_.  Normally its sign follows; for the
// one hidden position the coefficient is stored provisionally positive and its
// sign is settled from the unit's parity in advance_pos().
void LaneMachine::store_magnitude() {
    if (hide_ && pos_ == last_) {
        u_->coef[u_->scan[pos_]] = (i16)mag_;
        advance_pos();
        return;
    }
    phase_ = kSign;
}

// Commit the mode just decoded for block mb_ and move on.
static inline void mode_step(u8 *modes, int nbx, int &mb, int &mpm, int mode,
                             bool &done) {
    modes[mb] = (u8)mode;
    ++mb;
    done = (mb >= nbx * nbx);
    if (!done) mpm = mpm_of(modes, nbx, mb);
}

bool LaneMachine::feed(u32 v) {
    switch (phase_) {
        case kModeSym: {
            if (v >= (u32)kNumIntraModes) return false;
            int m = v == 0 ? mpm_ : nonmpm_mode(mpm_, (int)v - 1);
            bool done = false;
            mode_step(u_->modes, u_->nbx, mb_, mpm_, m, done);
            if (done) begin_next_unit();
            return true;
        }
        case kModeFlag: {
            if (v > 1) return false;
            if (v == 0) { phase_ = kModeIdx; return true; }
            bool done = false;
            mode_step(u_->modes, u_->nbx, mb_, mpm_, mpm_, done);
            if (done) begin_next_unit(); else phase_ = kModeFlag;
            return true;
        }
        case kModeIdx: {
            if (v > 7) return false;
            int m = nonmpm_mode(mpm_, (int)v);
            bool done = false;
            mode_step(u_->modes, u_->nbx, mb_, mpm_, m, done);
            if (done) begin_next_unit(); else phase_ = kModeFlag;
            return true;
        }
        case kCbf: {
            if (v > 1) return false;
            if (v == 0) {
                finish_coef_unit(0);
                return true;
            }
            if (u_->ncoef == 1) {
                last_ = 0;
                begin_levels();
                return true;
            }
            phase_ = kLast;
            return true;
        }
        case kLast: {
            if (v > 14) return false;
            last_cls_ = (int)v;
            int base = kLastBase[last_cls_];
            if (base >= u_->ncoef) return false;
            if (kLastRawBits[last_cls_] > 0) {
                phase_ = kLastRaw;
                return true;
            }
            last_ = base;
            begin_levels();
            return true;
        }
        case kLastRaw: {
            last_ = kLastBase[last_cls_] + (int)v;
            if (last_ >= u_->ncoef) return false;
            begin_levels();
            return true;
        }
        case kLevel: {
            if (v == (u32)kEscSym) {
                esc_j_ = 0;
                phase_ = kEscPrefix;
                return true;
            }
            mag_ = (i32)v;
            if (mag_ == 0) {
                if (pos_ == last_) return false;  // LAST must be nonzero
                u_->coef[u_->scan[pos_]] = 0;
                mag_ = 0;
                advance_pos();
                return true;
            }
            store_magnitude();
            return true;
        }
        case kEscPrefix: {
            if (v > 1) return false;
            if (v == 1) {
                if (++esc_j_ > kEscMaxPrefix) return false;
                return true;
            }
            esc_bits_ = esc_j_ + kEscOrder;
            esc_done_ = 0;
            esc_acc_ = 0;
            phase_ = kEscSuffix;
            return true;
        }
        case kEscSuffix: {
            int nchunks = (esc_bits_ + 7) / 8;
            int chunk = esc_done_ == 0 ? esc_bits_ - 8 * (nchunks - 1) : 8;
            esc_acc_ = (esc_acc_ << chunk) | v;
            esc_done_ += chunk;
            if (esc_done_ < esc_bits_) return true;
            u32 n = (1u << esc_bits_) + esc_acc_;
            u32 val = n - (1u << kEscOrder);
            if (val > 32752u) return false;
            mag_ = (i32)val + 15;
            store_magnitude();
            return true;
        }
        case kSign: {
            if (v > 1) return false;
            u_->coef[u_->scan[pos_]] = (i16)(v ? -mag_ : mag_);
            advance_pos();
            return true;
        }
        default:
            return false;
    }
}

// ----------------------------------------------------------------- rANS
// Bytes are appended to a reversed buffer, so push them in the order that
// yields the intended stream order after the final flip.
static inline void put16(std::vector<u8> &b, u32 v) {
    b.push_back((u8)(v & 0xff));   // becomes the second byte of the pair
    b.push_back((u8)(v >> 8));     // becomes the first byte of the pair
}

static bool encode_ops(const std::vector<u8> &lane_of, const std::vector<Op> &ops,
                const TableSet &tabs, int nlanes, std::vector<u8> &out) {
    // Bytes are produced back to front; `rev` holds them reversed and is
    // flipped at the end.
    std::vector<u8> rev;
    rev.reserve(ops.size() * 2 + 64);
    u32 state[kRansMaxLanes];
    for (int l = 0; l < nlanes; ++l) state[l] = kRansL;

    for (size_t i = ops.size(); i-- > 0;) {
        const Op &op = ops[i];
        int l = lane_of[i];
        u32 f, c;
        if (op.kind == OP_SYM) {
            if (op.arg >= kNumCtx) return false;
            const CtxTable &t = tabs.ctx[op.arg];
            f = t.freq[op.value];
            c = t.cum[op.value];
            if (f == 0) return false;
        } else {
            int k = op.arg;
            f = 1u << (kProbBits - k);
            c = (u32)op.value << (kProbBits - k);
        }
        u32 x = state[l];
        // Renormalize so that the encoded state stays below 2^32.
        if (x >= (f << kRansShift)) {
            put16(rev, x & 0xffff);
            x >>= 16;
        }
        state[l] = ((x / f) << kProbBits) + (x % f) + c;
    }
    // Flush: lane 0 first in the stream, so write lane n-1 first (reversed).
    for (int l = nlanes - 1; l >= 0; --l) {
        u32 x = state[l];
        rev.push_back((u8)((x >> 24) & 0xff));
        rev.push_back((u8)((x >> 16) & 0xff));
        rev.push_back((u8)((x >> 8) & 0xff));
        rev.push_back((u8)(x & 0xff));
    }
    out.assign(rev.rbegin(), rev.rend());
    return true;
}

bool RansDecoder::init(const u8 *buf, size_t len, int nlanes) {
    buf_ = buf;
    len_ = len;
    pos_ = 0;
    nlanes_ = nlanes;
    if (len < (size_t)nlanes * 4) return false;
    for (int l = 0; l < nlanes; ++l) {
        state_[l] = (u32)buf[pos_] | ((u32)buf[pos_ + 1] << 8) |
                    ((u32)buf[pos_ + 2] << 16) | ((u32)buf[pos_ + 3] << 24);
        pos_ += 4;
        if (state_[l] < kRansL) return false;
    }
    return true;
}

bool RansDecoder::renorm(u32 &x) {
    if (x >= kRansL) return true;
    if (pos_ + 2 > len_) return false;
    u32 v = ((u32)buf_[pos_] << 8) | (u32)buf_[pos_ + 1];
    pos_ += 2;
    x = (x << 16) | v;
    return true;
}

bool RansDecoder::decode_sym(int lane, const CtxTable &t, u32 &sym) {
    u32 x = state_[lane];
    u32 slot = x & (u32)(kProbTotal - 1);
    u32 s = t.slot2sym[slot];
    x = t.freq[s] * (x >> kProbBits) + slot - t.cum[s];
    if (!renorm(x)) return false;
    state_[lane] = x;
    sym = s;
    return true;
}

bool RansDecoder::decode_bypass(int lane, int nbits, u32 &val) {
    u32 x = state_[lane];
    u32 slot = x & (u32)(kProbTotal - 1);
    u32 v = slot >> (kProbBits - nbits);
    u32 f = 1u << (kProbBits - nbits);
    u32 c = v << (kProbBits - nbits);
    x = f * (x >> kProbBits) + slot - c;
    if (!renorm(x)) return false;
    state_[lane] = x;
    val = v;
    return true;
}

// ------------------------------------------------------- schedule walkers
bool encode_units(const Unit *units, int nunits, int nlanes,
                  const TableSet &tabs, std::vector<u8> &out) {
    int active = nlanes < nunits ? nlanes : nunits;
    std::vector<LaneMachine> lanes(active);
    for (int l = 0; l < active; ++l)
        lanes[l].init(units, nunits, l, nlanes, true);

    std::vector<Op> ops;
    std::vector<u8> lane_of;
    ops.reserve(4096);
    lane_of.reserve(4096);
    bool any = true;
    while (any) {
        any = false;
        for (int l = 0; l < active; ++l) {
            Op op;
            if (!lanes[l].next(op)) continue;
            any = true;
            ops.push_back(op);
            lane_of.push_back((u8)l);
            if (!lanes[l].feed(op.value)) return false;
        }
    }
    return encode_ops(lane_of, ops, tabs, active, out);
}

bool count_units(const Unit *units, int nunits, int nlanes,
                 u32 hist[kNumCtx][kNumSym], u32 *op_count) {
    int active = nlanes < nunits ? nlanes : nunits;
    std::vector<LaneMachine> lanes(active);
    for (int l = 0; l < active; ++l)
        lanes[l].init(units, nunits, l, nlanes, true);
    u32 n = 0;
    bool any = true;
    while (any) {
        any = false;
        for (int l = 0; l < active; ++l) {
            Op op;
            if (!lanes[l].next(op)) continue;
            any = true;
            ++n;
            if (op.kind == OP_SYM && op.arg >= kNumCtx) return false;
            if (op.kind == OP_SYM && hist) hist[op.arg][op.value]++;
            if (!lanes[l].feed(op.value)) return false;
        }
    }
    if (op_count) *op_count = n;
    return true;
}

bool decode_units(const Unit *units, int nunits, int nlanes,
                  const TableSet &tabs, const u8 *buf, size_t len) {
    int active = nlanes < nunits ? nlanes : nunits;
    RansDecoder rd;
    if (!rd.init(buf, len, active)) return false;
    std::vector<LaneMachine> lanes(active);
    for (int l = 0; l < active; ++l)
        lanes[l].init(units, nunits, l, nlanes, false);
    bool any = true;
    while (any) {
        any = false;
        for (int l = 0; l < active; ++l) {
            Op op;
            if (!lanes[l].next(op)) continue;
            any = true;
            u32 v;
            if (op.kind == OP_SYM) {
                if (op.arg >= kNumCtx) return false;
                if (!rd.decode_sym(l, tabs.ctx[op.arg], v)) return false;
            } else {
                if (!rd.decode_bypass(l, op.arg, v)) return false;
            }
            if (!lanes[l].feed(v)) return false;
        }
    }
    return true;
}

}  // namespace nxvc
