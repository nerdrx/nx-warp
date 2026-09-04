#include "common.h"

namespace nxvc {

const u16 kQStep[64] = {
       16,    18,    20,    23,    25,    29,    32,    36,
       40,    45,    51,    57,    64,    72,    81,    91,
      102,   114,   128,   144,   161,   181,   203,   228,
      256,   287,   323,   362,   406,   456,   512,   575,
      645,   724,   813,   912,  1024,  1149,  1290,  1448,
     1625,  1825,  2048,  2299,  2580,  2896,  3251,  3649,
     4096,  4598,  5161,  5793,  6502,  7298,  8192,  9195,
    10321, 11585, 13004, 14596, 16384, 18390, 20643, 23170,
};

const u8 kWeight[4][64] = {
  { // matrix 0
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
  },
  { // matrix 1
    16, 17, 18, 19, 20, 21, 22, 23,
    17, 18, 19, 20, 21, 22, 23, 24,
    18, 19, 20, 21, 22, 23, 24, 25,
    19, 20, 21, 22, 23, 24, 25, 26,
    20, 21, 22, 23, 24, 25, 26, 27,
    21, 22, 23, 24, 25, 26, 27, 28,
    22, 23, 24, 25, 26, 27, 28, 29,
    23, 24, 25, 26, 27, 28, 29, 30,
  },
  { // matrix 2
    16, 18, 20, 22, 24, 26, 28, 30,
    18, 20, 22, 24, 26, 28, 30, 32,
    20, 22, 24, 26, 28, 30, 32, 32,
    22, 24, 26, 28, 30, 32, 32, 32,
    24, 26, 28, 30, 32, 32, 32, 32,
    26, 28, 30, 32, 32, 32, 32, 32,
    28, 30, 32, 32, 32, 32, 32, 32,
    30, 32, 32, 32, 32, 32, 32, 32,
  },
  { // matrix 3
    16, 17, 19, 20, 22, 23, 25, 26,
    17, 19, 20, 22, 23, 25, 26, 28,
    19, 20, 22, 23, 25, 26, 28, 29,
    20, 22, 23, 25, 26, 28, 29, 31,
    22, 23, 25, 26, 28, 29, 31, 32,
    23, 25, 26, 28, 29, 31, 32, 32,
    25, 26, 28, 29, 31, 32, 32, 32,
    26, 28, 29, 31, 32, 32, 32, 32,
  },
};
const u8 kRaster8[64] = {
     0,  1,  2,  3,  4,  5,  6,  7,
     8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63,
};

const u8 kZigzag8[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
const u8 kZigzag4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
const u8 kZigzag2[4] = {0, 1, 2, 3};
const u8 kZigzag1[1] = {0};
// LAST classes.  Classes 0..7 code position 0..7 exactly; classes 8..14 add
// raw bits; class 15 is reserved and illegal in v1.
const u8 kLastBase[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 24, 32, 48, 64};
const u8 kLastRawBits[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 3, 4, 4, 0};

int last_class_of(int pos) {
    for (int c = 14; c >= 0; --c)
        if (pos >= kLastBase[c]) return c;
    return 0;
}

const u8 kLevelCtx[4][3] = {
    {0, 1, 2},
    {3, 4, 2},
    {5, 6, 7},
    {5, 6, 7},
};

#include "default_tables.inc"

const u16 kDeltaMul[32] = {
     16,  19,  23,  27,  32,  38,  45,  54,
     64,  76,  91, 108, 128, 152, 181, 215,
    256, 304, 362, 431, 512, 609, 724, 861,
   1024,1218,1448,1722,2048,2435,2896,3444,
};

void normalize_freqs(u16 f[kNumSym]) {
    i32 sum = 0;
    for (int s = 0; s < kNumSym; ++s) {
        if (f[s] < 1) f[s] = 1;
        sum += f[s];
    }
    if (sum == kProbTotal) return;
    i32 g[kNumSym];
    i32 total = 0;
    for (int s = 0; s < kNumSym; ++s) {
        // Table construction runs once per frame; integer division is allowed
        // here and never appears in the per-symbol decode path.
        g[s] = (i32)(((i64)f[s] * kProbTotal) / sum);
        if (g[s] < 1) g[s] = 1;
        if (g[s] > kProbMax) g[s] = kProbMax;
        total += g[s];
    }
    while (total < kProbTotal) {
        int best = 0;
        for (int s = 1; s < kNumSym; ++s)
            if (g[s] > g[best]) best = s;
        g[best]++;
        total++;
    }
    while (total > kProbTotal) {
        int best = -1;
        for (int s = 0; s < kNumSym; ++s)
            if (g[s] > 1 && (best < 0 || g[s] > g[best])) best = s;
        if (best < 0) break;
        g[best]--;
        total--;
    }
    for (int s = 0; s < kNumSym; ++s) f[s] = (u16)g[s];
}

bool finalize_ctx(CtxTable &t) {
    i32 c = 0;
    for (int s = 0; s < kNumSym; ++s) {
        if (t.freq[s] == 0) return false;
        t.cum[s] = (u16)c;
        c += t.freq[s];
    }
    t.cum[kNumSym] = (u16)c;
    if (c != kProbTotal) return false;
    for (int s = 0; s < kNumSym; ++s)
        for (int k = t.cum[s]; k < t.cum[s + 1]; ++k) t.slot2sym[k] = (u8)s;
    return true;
}

u16 default_freq(int nctx, int set_index, int c, int s) {
    set_index = clamp_i32(set_index, 0, 7);
    if (nctx >= kNumCtxV3) return kDefaultFreqV3[set_index][c][s];
    if (nctx >= kNumCtxV2) return kDefaultFreqV2[set_index][c][s];
    return kDefaultFreq[set_index][c][s];
}

// The built-in default row of (set, ctx) at the active probability precision.
// The families in default_tables.inc are stored at 10 bits; a wider kProbBits
// scales them and renormalizes, so this is the single definition of "the
// default row" that the delta base, the row-skip fallback and
// build_default_set all read.
void default_row(int nctx, int set_index, int c, u16 f[kNumSym]) {
    for (int s = 0; s < kNumSym; ++s)
        f[s] = (u16)(default_freq(nctx, set_index, c, s) * (kProbTotal / 1024));
    if (kProbBits != 10) normalize_freqs(f);
}

u16 default_row_freq(int nctx, int set_index, int c, int s) {
    if (kProbBits == 10) return default_freq(nctx, set_index, c, s);
    u16 f[kNumSym];
    default_row(nctx, set_index, c, f);
    return f[s];
}

void build_default_set(TableSet &ts, int set_index, int nctx) {
    set_index = clamp_i32(set_index, 0, 7);
    if (nctx < kNumCtxV1) nctx = kNumCtxV1;
    for (int c = 0; c < kNumCtx; ++c) {
        // Contexts beyond the model's count are never coded; fill them so the
        // table object is always well formed.
        default_row(nctx, set_index, c < nctx ? c : 0, ts.ctx[c].freq);
        finalize_ctx(ts.ctx[c]);
    }
}

}  // namespace nxvc
