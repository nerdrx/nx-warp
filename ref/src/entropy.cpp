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
    phase_ = kCbf;
}

void LaneMachine::begin_levels() {
    pos_ = last_;
    prev_class_ = 0;
    phase_ = kLevel;
}

void LaneMachine::advance_pos() {
    prev_class_ = level_class((int)(mag_ < 0 ? -mag_ : mag_));
    if (pos_ == 0) {
        ui_ += stride_;
        if (ui_ >= nunits_) {
            phase_ = kDone;
        } else {
            begin_unit();
        }
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
            op.arg = u_->ctx_cbf;
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
            op.arg = u_->ctx_last;
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
        case kLevel: {
            op.kind = OP_SYM;
            op.arg = (u8)level_ctx(pos_, prev_class_);
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
        default:
            return false;
    }
}

bool LaneMachine::feed(u32 v) {
    switch (phase_) {
        case kCbf: {
            if (v > 1) return false;
            if (v == 0) {
                ui_ += stride_;
                if (ui_ >= nunits_) phase_ = kDone; else begin_unit();
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
            phase_ = kSign;
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
            phase_ = kSign;
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
        if (x >= (f << 22)) {
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
    u32 slot = x & 1023u;
    u32 s = t.slot2sym[slot];
    x = t.freq[s] * (x >> kProbBits) + slot - t.cum[s];
    if (!renorm(x)) return false;
    state_[lane] = x;
    sym = s;
    return true;
}

bool RansDecoder::decode_bypass(int lane, int nbits, u32 &val) {
    u32 x = state_[lane];
    u32 slot = x & 1023u;
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
