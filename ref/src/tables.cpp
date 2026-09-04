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
// The split-block scan (tool bit 19): the four 4x4 sub-blocks of an 8x8 block
// in raster order, each in 4x4 zigzag, concatenated into the same 64-value
// coding unit an unsplit block uses.  Sub-block (sx, sy) keeps its 16
// coefficients in the (sy, sx) quadrant of the block-local 8x8 array, so the
// unit's coefficient layout is a bijection of the unsplit one and nothing
// else about the unit -- CBF, LAST, the level chain, the lane schedule --
// changes.  docs/SYNTAX.md 9.2.
const u8 kScan4Split[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 25, 18, 11, 19, 26, 27,
     4,  5, 12, 20, 13,  6,  7, 14, 21, 28, 29, 22, 15, 23, 30, 31,
    32, 33, 40, 48, 41, 34, 35, 42, 49, 56, 57, 50, 43, 51, 58, 59,
    36, 37, 44, 52, 45, 38, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
const u8 kZigzag2[4] = {0, 1, 2, 3};
const u8 kZigzag1[1] = {0};
// kCflRecip[d] = round(2^15 / d); entry 0 is never read (SYNTAX.md 7.7).
const u16 kCflRecip[256] = {
        0, 32768, 16384, 10923,  8192,  6554,  5461,  4681,
     4096,  3641,  3277,  2979,  2731,  2521,  2341,  2185,
     2048,  1928,  1820,  1725,  1638,  1560,  1489,  1425,
     1365,  1311,  1260,  1214,  1170,  1130,  1092,  1057,
     1024,   993,   964,   936,   910,   886,   862,   840,
      819,   799,   780,   762,   745,   728,   712,   697,
      683,   669,   655,   643,   630,   618,   607,   596,
      585,   575,   565,   555,   546,   537,   529,   520,
      512,   504,   496,   489,   482,   475,   468,   462,
      455,   449,   443,   437,   431,   426,   420,   415,
      410,   405,   400,   395,   390,   386,   381,   377,
      372,   368,   364,   360,   356,   352,   349,   345,
      341,   338,   334,   331,   328,   324,   321,   318,
      315,   312,   309,   306,   303,   301,   298,   295,
      293,   290,   287,   285,   282,   280,   278,   275,
      273,   271,   269,   266,   264,   262,   260,   258,
      256,   254,   252,   250,   248,   246,   245,   243,
      241,   239,   237,   236,   234,   232,   231,   229,
      228,   226,   224,   223,   221,   220,   218,   217,
      216,   214,   213,   211,   210,   209,   207,   206,
      205,   204,   202,   201,   200,   199,   197,   196,
      195,   194,   193,   192,   191,   189,   188,   187,
      186,   185,   184,   183,   182,   181,   180,   179,
      178,   177,   176,   175,   174,   173,   172,   172,
      171,   170,   169,   168,   167,   166,   165,   165,
      164,   163,   162,   161,   161,   160,   159,   158,
      158,   157,   156,   155,   155,   154,   153,   152,
      152,   151,   150,   150,   149,   148,   148,   147,
      146,   146,   145,   144,   144,   143,   142,   142,
      141,   141,   140,   139,   139,   138,   138,   137,
      137,   136,   135,   135,   134,   134,   133,   133,
      132,   132,   131,   131,   130,   130,   129,   129,
};

// Dead-zone offsets by frequency band, in forty-eighths of a quantiser step.
// 16 is 1/3, the flat value these replace and -- measured over a sweep of
// flat and shaped alternatives on the quality harness -- still the best one
// at every band.  ref/RESULTS-detail-a.md section 3.
const u8 kDeadZoneDc[4] = {16, 16, 16, 16};
const u8 kDeadZoneAc[4] = {16, 16, 16, 16};

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
