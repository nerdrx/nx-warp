# Annex A — Constant tables (normative)

Every table in this annex is transcribed **verbatim from the reference
implementation**, not retyped from a document, and each carries the file and
the commit it was taken from. Where `docs/SYNTAX.md` states a table by formula
and `ref/` states it by value, both are given and they agree; if they ever stop
agreeing, that is a defect in one of them and neither may be interpreted
([R-20], clause 9.2).

No constant in this annex was invented for this document set.

## A.1 Inverse transform constants

Nine-bit constants, `round(512 * cos/sin)` [SYNTAX 6.1]. These are this
format's own constants; they are not HEVC's or AV1's matrices, which matters
for Annex B.

| Name | Value | Equals |
|---|---|---|
| `C4` | 362 | `512 cos(pi/4)` |
| `C2` | 473 | `512 cos(pi/8)` |
| `S2` | 196 | `512 sin(pi/8)` |
| `A1` | 502 | `512 cos(pi/16)` |
| `A3` | 426 | `512 cos(3pi/16)` |
| `A5` | 284 | `512 sin(3pi/16)` |
| `A7` | 100 | `512 sin(pi/16)` |

The flow graph that consumes them is the Loeffler-Ligtenberg-Moschytz
factorisation [R-5]: 11 multiplies and 29 adds per 8-point 1D transform. It is
clause 6.4.2.

## A.2 Quantiser steps and weighting matrices

`qstep[qp] = round(16 * 2^(qp/6))`, Q4, so `qstep[0] = 16` is a step of exactly
1.0. Source: `ref/src/tables.cpp` @ `d43182b`.

```c
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
```

Weighting matrices, Q4, values constrained to `[1, 32]`. `quant_matrix` selects
index 0 to 3 for the **luma** matrix (also used for alpha); the **chroma**
matrix is index 3's, except that `quant_matrix == 0` is flat for chroma too
[SYNTAX 6.5, decision 13]. By formula, with `s = u + v`:

| `quant_matrix` | Luma / alpha | Chroma |
|---|---|---|
| 0 | `16` (flat) | `16` (flat) |
| 1 | `min(32, 16 + s)` | `min(32, 16 + s + (s >> 1))` |
| 2 | `min(32, 16 + 2s)` | `min(32, 16 + s + (s >> 1))` |
| 3 | `min(32, 16 + s + (s >> 1))` | `min(32, 16 + s + (s >> 1))` |
| 255 | Custom bytes 0..63 | Custom bytes 64..127 |

By value, `ref/src/tables.cpp` @ `d43182b`:

```c
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
```

## A.3 Probability-table delta multipliers

`kDeltaMul[i] = round(256 * 2^((i-16)/4))`, Q8 with 256 meaning 1.0, indexed by
the 5-bit `table_delta` of a transmitted table set [SYNTAX 9.4].
Source: `ref/src/tables.cpp` @ `d43182b`.

```c
const u16 kDeltaMul[32] = {
     16,  19,  23,  27,  32,  38,  45,  54,
     64,  76,  91, 108, 128, 152, 181, 215,
    256, 304, 362, 431, 512, 609, 724, 861,
   1024,1218,1448,1722,2048,2435,2896,3444,
};
```

## A.4 Interpolation filter taps

The 16-phase 4-tap Catmull-Rom table used by the inter predictor (clause 6.7.5)
and, unchanged, by stereo prediction (clause 6.8.1). Each row sums to **exactly
64**, so a flat region reproduces exactly [STEREO 5]. Derived from the
`a = -1/2` cubic convolution kernel of [R-10].

Source: `warp/ref/warp_ref.cpp` @ `9083dd1`.

```c
const int8_t kCatmullRom[16][4] = {
    {   0,  64,   0,   0},  // f= 0
    {  -2,  64,   2,   0},  // f= 1
    {  -3,  61,   6,   0},  // f= 2
    {  -4,  59,  10,  -1},  // f= 3
    {  -5,  56,  15,  -2},  // f= 4
    {  -5,  51,  20,  -2},  // f= 5
    {  -5,  47,  25,  -3},  // f= 6
    {  -4,  41,  30,  -3},  // f= 7
    {  -4,  36,  36,  -4},  // f= 8
    {  -3,  30,  41,  -4},  // f= 9
    {  -3,  25,  47,  -5},  // f=10
    {  -2,  20,  51,  -5},  // f=11
    {  -2,  15,  56,  -5},  // f=12
    {  -1,  10,  59,  -4},  // f=13
    {   0,   6,  61,  -3},  // f=14
    {   0,   2,  64,  -2},  // f=15
};
```

The bilinear filter of the Lite profile needs no table: its weights are
`16 - f` and `f` per axis, with the product on 256 and a single rounding
(clause 6.7.5).

**Provisional.** This table lives in `warp/`, which no normative document yet
ratifies, and nothing in the syntax selects between the two filters. See
clause 8.2 and Annex C issues C-7 and C-2. [pending WARP.md]

## A.5 Scan orders and the LAST class table

Zigzag scans [SYNTAX 9.2]. Source: `ref/src/tables.cpp` @ `d43182b`.

```c
const u8 kZigzag8[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

const u8 kZigzag4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
```

The 4-value DC-plane scan is `0, 1, 2, 3` and the 1-value scan is `0`. A
transform-skip block uses the raster scan `scan[i] = i`.

LAST classes [SYNTAX 9.3]:

```c
const u8 kLastBase[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 24, 32, 48, 64};
const u8 kLastRawBits[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 3, 4, 4, 0};
```

Class 15 is **reserved and MUST be rejected**; the value 64 at index 15 of
`kLastBase` is a sentinel used by the encoder's class-selection loop, not a
codable base. A decoder MUST also reject a `last_base >= ncoef` and a resulting
`last >= ncoef` (clause 5.5).

LEVEL context mapping, indexed `[band][prev]`, with the result added to 4 to
give the context index [SYNTAX 9.3]:

```c
const u8 kLevelCtx[4][3] = {
    {0, 1, 2},
    {3, 4, 2},
    {5, 6, 7},
    {5, 6, 7},
};
```

## A.6 Default probability tables

Eight built-in table sets, `[8][12][16]`, 10-bit frequencies, every entry at
least 1, every row summing to exactly 1024 [SYNTAX 9.3, 9.4, decision 21].
A frame uses set `k` directly when bit `k` of `tables_present` is clear, and
uses it as the base for the log-domain deltas of clause 6.6.2 when the bit is
set.

Source: `ref/src/default_tables.inc` @ `c1ce6a4`, generated by
`ref/tools/nxv-gentables`. Row order is context index 0 to 11 (clause 6.6.5);
column order is symbol 0 to 15.

**This is a generated table.** It is reproduced here in full because a second
implementer needs it and because the conformance vectors pin it, but the
generator is the source of truth and a regeneration changes every conformance
digest. It was last regenerated at commit `c1ce6a4`.

```c
const u16 kDefaultFreq[8][kNumCtx][kNumSym] = {
  { // set 0
    {  19,  991,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {  58,  952,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {   9,    4,    5,   18,    5,   30,   12,   17,  116,   39,  203,  141,  178,  148,   98,    1},
    { 243,   54,   52,   66,    7,  177,   39,    4,   50,   22,   53,   67,   50,   49,   90,    1},
    { 310,  254,  123,  117,   67,   55,   28,   25,   23,    4,    3,    3,    3,    1,    3,    5},
    { 326,  303,  128,   85,   52,   41,   23,   12,   10,    7,    3,    2,    2,    2,    2,   26},
    { 169,   97,   68,   66,   58,   57,   47,   45,   37,   34,   30,   28,   22,   20,   17,  229},
    { 519,  259,   95,   46,   24,   21,   13,   10,    8,    5,    4,    4,    3,    2,    2,    9},
    { 334,  252,  147,  114,   58,   33,   20,   13,    9,    8,    5,    5,    4,    3,    3,   16},
    { 742,  225,   27,    9,    4,    3,    2,    2,    2,    1,    1,    1,    1,    1,    1,    2},
    { 591,  304,   55,   26,   13,    9,    5,    4,    3,    2,    2,    2,    2,    1,    1,    4},
    { 220,  156,  113,  103,   65,   57,   42,   39,   31,   30,   24,   22,   15,   14,   11,   82},
  },
  { // set 1
    {   1, 1009,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {   1, 1009,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {   1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1, 1009,    1},
    {   1,    1,    1,    1,    1,    1,    1,    1,    1,    1,   19,    1,    3,    3,  987,    1},
    { 493,   30,   12,   30,   36,   41,   34,   22,   23,   21,   23,   21,   14,   16,   22,  186},
    {  13,   63,   25,   27,   22,   49,   45,   60,   52,   48,   42,   59,   31,   45,   33,  410},
    {  12,   18,   17,   18,   16,   19,   16,   19,   17,   17,   16,   19,   18,   19,   17,  766},
    { 369,   34,   37,   31,   26,   28,   27,   20,   20,   16,   17,   18,   16,   12,   14,  339},
    {  45,   58,   52,   32,   34,   37,   27,   31,   29,   29,   23,   27,   22,   26,   21,  531},
    { 115,   27,   24,   26,   24,   25,   23,   24,   22,   22,   22,   25,   21,   23,   21,  580},
    {  32,   43,   39,   38,   35,   37,   32,   32,   31,   33,   27,   31,   26,   26,   24,  538},
    {  13,   18,   18,   19,   19,   20,   19,   20,   20,   20,   19,   23,   21,   23,   21,  731},
  },
  { // set 2
    {   1, 1009,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {   1, 1009,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {   1,    2,    1,    9,    1,    2,    1,    1,    2,    1,    3,    3,    1,  313,  682,    1},
    {   1,    1,    1,   10,    1,   63,   26,    1,    5,    1,   28,   16,  398,  470,    1,    1},
    { 236,   84,   73,   73,   63,   60,   63,   47,   40,   34,   35,   26,   21,   24,   16,  129},
    {  99,   49,   83,   81,   40,   31,   21,   21,   17,   20,   20,   35,   27,   14,   31,  435},
    { 245,   53,   55,   56,   50,   49,   43,   40,   35,   32,   28,   28,   24,   21,   21,  244},
    { 493,   24,   19,   31,   31,   27,   26,   23,   22,   24,   20,   21,   16,   16,   17,  214},
    { 166,  160,  154,  119,   69,   69,   21,   17,   22,   20,   23,   19,   15,   12,    9,  129},
    { 684,  129,   57,   35,   24,   18,   13,   10,    9,    7,    6,    5,    5,    3,    3,   16},
    { 421,  316,  149,   63,   30,   14,    8,    6,    4,    2,    2,    2,    1,    1,    1,    4},
    { 354,  122,  114,   93,   69,   53,   40,   31,   25,   20,   16,   13,   11,    9,    8,   46},
  },
  { // set 3
    { 938,   72,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    { 940,   70,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    { 330,   61,   86,   11,    9,    1,    1,    1,   43,    1,   22,   11,    1,    4,  441,    1},
    { 136,   57,   65,   26,    4,   29,   18,    9,   24,   11,  224,   12,    6,    5,  397,    1},
    { 297,  623,   48,   13,    5,    2,    3,    1,    2,    2,    2,    2,    1,    1,    2,   20},
    { 452,  404,  112,   19,    5,    1,    1,    1,    1,    1,    5,    5,    2,    1,    2,   12},
    { 196,  237,  200,  126,   68,   39,   29,   20,   15,   11,    8,    8,    5,    7,    6,   49},
    { 384,  344,  100,   56,   32,   19,   13,   11,    7,    6,    3,    5,    3,    5,    2,   34},
    { 340,  353,  165,   71,   38,   23,   12,    6,    4,    2,    2,    2,    2,    1,    1,    2},
    { 444,  346,  141,   53,   19,    8,    3,    2,    1,    1,    1,    1,    1,    1,    1,    1},
    { 362,  381,  171,   64,   23,    9,    4,    2,    1,    1,    1,    1,    1,    1,    1,    1},
    { 279,  320,  213,  115,   52,   21,   10,    4,    2,    2,    1,    1,    1,    1,    1,    1},
  },
  { // set 4
    { 244,  766,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    { 622,  388,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {   4,    4,    3,    3,    2,    2,    2,   11,   10,    2,   16,   29,   12,   29,  894,    1},
    {   2,    7,    5,    5,    4,    4,    6,   17,    6,    7,   76,   22,   23,   18,  821,    1},
    { 862,  131,    7,    4,    3,    3,    2,    2,    2,    1,    1,    1,    1,    1,    1,    2},
    { 116,  830,   61,    3,    2,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    2},
    {  10,   41,  397,  282,  147,   39,   20,   15,    9,    7,    4,    4,    3,    4,    3,   39},
    { 814,  187,    5,    2,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    5},
    { 101,  827,   75,    5,    2,    2,    1,    1,    1,    1,    1,    1,    1,    1,    1,    3},
    { 782,  194,   19,    8,    5,    3,    2,    2,    1,    1,    1,    1,    1,    1,    1,    2},
    { 174,  775,   53,    7,    3,    2,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {  30,  109,  448,  259,  119,   28,   11,    6,    3,    2,    1,    1,    1,    1,    1,    4},
  },
  { // set 5
    { 183,  827,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    { 489,  521,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    { 118,  117,  211,   63,   81,  234,    6,   18,   53,    2,   10,    6,    1,    1,  102,    1},
    { 472,  151,   96,   47,    1,   69,    1,    1,    2,    1,   43,    6,    9,    2,  122,    1},
    { 120,  613,  196,   42,   20,    9,    6,    4,    3,    2,    2,    2,    1,    1,    1,    2},
    { 300,  344,  176,   92,   39,   20,   14,   10,    6,    5,    5,    3,    3,    2,    2,    3},
    { 125,  172,  139,  114,   98,   91,   63,   51,   34,   26,   19,   18,   12,   10,    7,   45},
    { 211,  577,  125,   46,   18,   10,    7,    5,    4,    3,    2,    2,    2,    1,    1,   10},
    { 317,  311,  176,   82,   44,   29,   18,   13,    9,    6,    4,    3,    3,    2,    2,    5},
    { 287,  328,  128,   81,   57,   44,   29,   21,   14,    9,    6,    5,    4,    3,    3,    5},
    { 252,  282,  148,  105,   74,   53,   36,   24,   17,   11,    7,    5,    3,    2,    1,    4},
    { 102,  141,  138,  140,  126,  123,   87,   61,   34,   21,   14,   11,    7,    5,    4,   10},
  },
  { // set 6
    {   1, 1009,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {   1, 1009,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {   1,    1,    1,    2,    1,    1,    1,    1,    1,    1,    2,    1,    1,  208,  800,    1},
    {   1,    1,    1,    4,    1,    1,    3,    1,    1,    1,   19,    1,  489,  498,    1,    1},
    {  57,  108,   55,   46,   20,   14,    8,   11,    9,    9,    6,   11,    8,    9,   12,  641},
    {  20,   87,   72,   58,   17,   11,    9,   13,   16,   22,   16,   21,    8,   17,   10,  627},
    { 182,   29,   16,   13,    9,    7,    6,    6,    6,    7,    7,    7,    7,    8,    8,  706},
    { 487,    9,    4,    5,    5,    5,    3,    3,    5,    3,    3,    3,    5,    4,    2,  478},
    { 165,  109,   23,   19,   11,    4,    2,    3,    5,    7,    5,    5,    4,    3,    1,  658},
    { 681,   21,   16,   15,   14,   14,   12,   11,   11,   10,    9,    9,    9,    9,    7,  176},
    { 293,  176,  160,  107,   66,   38,   23,   14,   12,   11,    5,    8,    5,    4,    3,   99},
    { 278,   25,   32,   37,   37,   34,   34,   31,   29,   27,   24,   25,   19,   20,   19,  353},
  },
  { // set 7
    { 132,  878,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    { 200,  810,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1},
    {  39,   84,  189,   17,  171,    3,    1,   20,   19,    1,    3,    1,    1,    1,  473,    1},
    {  29,  178,  275,    6,    1,    1,    1,    1,    1,    1,   22,    1,    6,    6,  494,    1},
    { 814,  156,   10,    9,    6,    5,    4,    3,    3,    3,    2,    2,    2,    1,    1,    3},
    { 859,   41,   24,   16,   14,   15,   11,    8,    7,    6,    5,    4,    3,    3,    2,    6},
    {  51,   59,   59,   61,   57,   58,   53,   57,   64,   65,   56,   54,   47,   40,   36,  207},
    { 277,  639,   40,    7,    7,    6,    5,    4,    4,    3,    3,    3,    3,    3,    2,   18},
    { 803,   53,   20,   15,   13,   11,   10,   10,    9,    8,    8,    7,    6,    5,    5,   41},
    { 223,  148,   69,   66,   59,   54,   51,   45,   44,   40,   34,   31,   27,   21,   20,   92},
    { 106,  138,   86,   81,   73,   67,   60,   55,   49,   44,   39,   35,   29,   25,   21,  116},
    {  45,   65,   66,   68,   66,   68,   67,   70,   69,   67,   57,   52,   45,   38,   32,  149},
  },
};
```

## A.7 Tables that do not exist yet

| Table | Needed by | Status |
|---|---|---|
| Level limits | Clause 8.3 | Not defined anywhere [pending SYNTAX.md] |
| 10-bit sample domain and quantiser scaling | Clause 6.2 | Not defined anywhere [pending SYNTAX.md] |
| Enhancement-layer blend weights for `wgt` | Clause 6.9 | Four values in [R-18], five in [I-1], no blend formula [pending HYBRID.md] |
| Homography quantisation format | Clause 6.7.2 | Three incompatible published formats [pending WARP.md] |
| `ENT_BITPLANE`, `INTRA_DIR`, `XFORM_WAVELET`, `XFORM_4X4_SPLIT` constants | Tool bits 16-19 | Declared, never specified. A v1 decoder refuses them, which is sufficient |
