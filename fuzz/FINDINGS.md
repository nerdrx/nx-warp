# Fuzzing and sanitizer findings

What the harnesses in `fuzz/` and the ASan/UBSan build found. **Nothing here is
fixed** -- this directory owns `fuzz/**` and the CI workflows only, and the
component code belongs to the agents landing it. Each entry names the file and
line so it can be picked up directly.

Every reproducer is committed under `fuzz/regressions/<target>/` and is replayed
by `ctest -R '^fuzz\.'` on every build, so a fix can be verified without a
libFuzzer toolchain.

Run that produced these:

* host: 32-core x86-64, everything under `chrt -i 0 taskset -c 12-15 nice -n 19`,
  at most 4 libFuzzer workers.
* build: clang 22.1.8, `RelWithDebInfo`, `NXWARP_SANITIZER=address` plus
  `-fsanitize=undefined -fno-sanitize-recover=undefined`, `NXWARP_FUZZ=ON`,
  `NXVC_FUZZ=ON`.
* campaign: 5 minutes per target, `-jobs=4 -workers=4`, seeded from
  `fuzz/corpus/` with the dictionaries in `fuzz/dict/`.
* plus one full `ctest` pass of the whole tree under the same sanitizers.

What each target did in its five minutes (four workers, totals across them):

| target | executions | corpus units added | crashes |
|---|---:|---:|---|
| `nxvc_decode_fuzz` | 52 k | 1 603 | 4, all F3 |
| `nxvc_headers_fuzz` | 1.53 M | 5 941 | 2, both F5 |
| `nxvc_rans_fuzz` | 6.69 M | 9 797 | none |
| `transport_depacketize_fuzz` | 2.50 M (v1) / 3.12 M (v2 re-run) | 2 227 / 1 959 | none |
| `transport_rs_fuzz` | 2.72 M | 7 422 | none |
| `transport_feedback_fuzz` | 3.26 M | 1 228 | none |
| `warp_tile_fuzz` | 1.8 k (aborted by F6) / 62.6 k after the fix | 1 441 | 4, all F7 |

`nxvc_decode_fuzz` is three orders of magnitude slower per execution than the
header target because every input reconstructs pixels; that is the intended
split and the reason the two targets exist separately. The v2 re-run of
`transport_depacketize_fuzz` reached 659 edges against 1 144 features, roughly
double what the v1 model reached, which is the measurable argument for keeping
the wire model in `fuzz/common/nxt_wire.h` in step with TRANSPORT.md.

The tree moved under the campaign: `ref/` fixed F3 partway through, and
`transport/` landed the v2 datagram header (TRANSPORT.md decision D19) after
`transport_depacketize_fuzz` had already run against the v1 layout. The
datagram model in `fuzz/common/nxt_wire.h`, `fuzz/dict/nxt.dict` and the seeds
were updated to v2 and that target was re-run; the numbers below are from the
re-run.

| # | severity | status | component | one line |
|---|---|---|---|---|
| F1 | medium | fixed (fff0fde) | transport | `Receiver::mark_tile_undecodable()` indexes the tile ring with an unchecked `(row, col)` |
| F2 | medium | open | warp | `warp_tile_corners()` does not saturate corners in `kModeStatic`, contrary to `kCornerClamp`'s contract |
| F3 | **high** | **fixed upstream** | ref | signed-integer overflow in `idct8_1d()` on legal int16 coefficients, reachable from a bitstream |
| F4 | medium | open | build | `NXWARP_SANITIZER=address+undefined` enables `-fsanitize=integer`, which fires on well-defined code and makes the preset unusable |
| F5 | low | open | ref / spec | the stream header accepts `color_transform == 1` with 4:2:0, which SYNTAX.md section 2 forbids |
| F6 | -- | fixed here | fuzz | signed overflow in this directory's own warp mutator; found by the mutator rounds the regression runner executes |
| F7 | medium | open | warp | two signed-overflow / negate-INT_MIN sites in `corner_component()`, in the code that is supposed to be the saturation |
| F8 | **high** | open | ref | `Unit::ctx_level` / `ctx_mode` are tested for truthiness, so the `-1` their header documents becomes context index 255 and indexes `TableSet::ctx[16]` out of bounds |
| F11 | -- | fixed here | fuzz | the headers harness bounds a plane at 4096, but `width` is per eye, so a stereo plane is 8192; and `run_campaign.sh` let each target overwrite the previous one's libFuzzer logs |

F10 is allocated on `merge-main` and is a deliberate gap here; see F11, which
is the same defect as that branch's F10. F9 was never used on any branch.

Reproducers for a finding that is still **open** live in
`fuzz/regressions/<target>/open/` and are skipped by the CTest entry so it stays
green; `--include-open` replays them. A fixed finding's reproducer moves up one
directory and then runs on every push. See `fuzz/regressions/README.md`.

---

## F3 -- signed integer overflow in the inverse DCT (high) -- FIXED UPSTREAM

**Status:** fixed in `ref/src/transform.cpp` while this campaign was running.
The fix adds `mul_c4_rnd9()`, which computes `((s * kC4) + 256) >> 9` as
`hi*kC4 + ((lo*kC4 + 256) >> 9)` with `hi = s >> 9`, `lo = s & 511` -- an exact
two-word split, not an approximation, so no conformance vector changes. Both
minimized reproducers now pass, `ref.transform`, `ref.saturate` and
`ref.vectors` pass under ASan+UBSan, and the two inputs have been promoted out
of `open/` into `fuzz/regressions/nxvc_decode_fuzz/` where they are replayed on
every push. **The analysis below is kept because it is the reason the bound in
SYNTAX.md 6.3 is still wrong as written** -- the code no longer overflows, but
the document still claims a bound the flow graph does not have.

**Target:** `nxvc_decode_fuzz`, and `ctest -R ref.transform` under UBSan.
**Reproducers:** `fuzz/regressions/nxvc_decode_fuzz/F3-idct8-1d-signed-overflow-{178,214}.bin`
**Site:** `ref/src/transform.cpp:23`, in `nxvc::idct8_1d()` (pre-fix line number)

```
ref/src/transform.cpp:23:23: runtime error: signed integer overflow:
    -21364084 * 362 cannot be represented in type 'i32' (aka 'int')
    #0 nxvc::idct8_1d(int const*, int*)  ref/src/transform.cpp:23:23
    #1 nxvc::idct8x8(int const*, int*)   ref/src/transform.cpp:75:9
```

The line is the odd-part rotation of SYNTAX.md section 6.2:

```cpp
i32 O1 = ((P + Q) * kC4 + 256) >> 9;   // transform.cpp:23
```

**Root cause.** SYNTAX.md 6.3 argues the transform is int32-safe: *"Dequantized
coefficients are themselves clamped to int16 (section 6.5), which bounds every
product in the transform to about 8.9e7, comfortably inside int32."* That bound
is computed for a product of one coefficient with one 9-bit constant
(`32767 * 512` is about `1.7e7`), but `P` and `Q` are not coefficients -- they
are already sums of two such products:

```
A = x1*A1 + x7*A7   ->  |A| <= 32767 * (502 + 100)  ~ 1.97e7
C = x3*A3 + x5*A5   ->  |C| <= 32767 * (426 + 284)  ~ 2.33e7
P = A - C           ->  |P| <= 4.3e7
Q = B + D           ->  |Q| <= 4.3e7
(P + Q) * C4        ->  up to 8.6e7 * 362 ~ 3.1e10     // int32 max is 2.1e9
```

So the overflow is about 15x over int32, not marginal, and it is in the
**first** pass, whose inputs are dequantized coefficients straight from 6.5's
int16 clamp. The `clamp16` that bounds the intermediate happens *after*
`idct8_1d` returns, which is too late.

**Reachability.** Not theoretical: `ctest -R ref.transform` already fails under
UBSan at `tests/ref/test_transform.cpp:103`, which feeds coefficients drawn
uniformly from `[-32768, 32767]` -- exactly the range 6.5 declares legal. And
`nxvc_decode_fuzz` reached it from a byte stream through the public C ABI in
under five minutes, so a conforming-looking bitstream drives it.

**Why it matters beyond UB.** The value that overflows is wrong, not merely
undefined, so on a target where the wrap differs (or where the optimizer uses
the no-overflow assumption) the GPU decoder and the CPU reference will disagree
on a pixel -- which breaks the bit-exactness that SYNTAX.md section 12
conformance is defined by. This is a specification bug as much as an
implementation one: 6.2's flow graph, 6.3's bound and 6.5's coefficient range
cannot all three hold.

**What was still open after the fix.** The implementation is correct now, but
SYNTAX.md 6.3 still states a bound ("about 8.9e7, comfortably inside int32")
that the flow graph does not satisfy, and 6.2 still presents
`O1 = ((P + Q) * C4 + 256) >> 9` as if it were an int32 expression. A GPU
implementer following the document rather than the code will reintroduce the
overflow. The normative text needs the same note the code now carries.

---

## F1 -- `mark_tile_undecodable()` indexes the ring without a range check (medium)

**Target:** `transport_depacketize_fuzz`
**Reproducer:** `fuzz/regressions/transport_depacketize_fuzz/F1-mark_tile_undecodable-oob-tile_first-32768.bin`
**Site:** `transport/src/receiver.cpp:319`, via `transport/include/nxvc/transport/receiver.h:48`

```
stl_vector.h:1253: std::vector<nxt::TileMeta>::operator[]:
    Assertion '__n < this->size()' failed.
  #4 std::vector<nxt::TileMeta>::operator[](__n=32768)
  #5 nxt::FrameRing::at(layer=0, tile=32768)   receiver.h:48
  #6 nxt::Receiver::mark_tile_undecodable(frame_id=0, layer=0, row=481, col=60)
                                                receiver.cpp:319
```

```cpp
void Receiver::mark_tile_undecodable(uint16_t frame_id, uint8_t layer,
                                     uint16_t row, uint16_t col) {
    FrameRing::Slot* s = ring_.find(frame_id);
    if (!s || layer >= cfg_.layers) return;          // layer is checked
    ring_.at(*s, layer, cfg_.tile_index(row, col)).state = TileState::kUndecodable;
}                                                    // row/col are not
```

`layer` is validated; `row` and `col` are not. `tile_index(row, col)` is
`row * cols + col` with no clamp, and `FrameRing::at()` indexes
`meta[layer * tiles_per_frame() + tile]` directly. With `row = 481` the index is
32768 against a 2312-entry vector -- a heap overflow in a release build, caught
here only because libstdc++ assertions were on.

**Caveat, stated plainly.** The harness reached this by passing a `row`/`col`
derived from a hostile datagram's `tile_first`, not from a `TileOutput` the
receiver had returned. The documented flow is the latter, so this is a *caller*
mistake -- but it is the exact caller mistake an integration makes, the function
is public, its precondition is not documented, and every other entry point in
`Receiver` validates its input and counts a `bad_range` (TRANSPORT.md section
12). The harness now respects the contract, so the target no longer aborts on
it; the reproducer is kept, and the two-line fix is

```cpp
if (row >= cfg_.rows || col >= cfg_.cols) return;
```

next to the existing `layer` check.

---

## F2 -- `warp_tile_corners()` does not saturate in `kModeStatic` (medium)

**Target:** `warp_tile_fuzz`
**Reproducer:** `fuzz/regressions/warp_tile_fuzz/F2-corner-clamp-static-mode.bin`
**Site:** `warp/ref/warp_ref.cpp:189-192`

```cpp
if (mode == kModeStatic) {
    for (int i = 0; i < 4; ++i) {
        c.x[i] = (tile_x + ((i & 1) ? kTile : 0)) << kQCorner;   // no clamp
        c.y[i] = (tile_y + ((i >> 1) ? kTile : 0)) << kQCorner;  // no clamp
    }
    return c;
}
```

The `kModeWarp` path below it saturates every component through
`clamp_i32(v, -kCornerClamp, kCornerClamp)` (`warp_ref.cpp:182`) and saturates
to `kCornerClamp` when the denominator leaves its range
(`warp_ref.cpp:208-209`). The `kModeStatic` path does neither, so the contract
stated in `warp/include/nxvc/warp.h:50-52` --

> Corner coordinates are saturated to +-2^18 in Q.6 (== +-4096 pel) so that the
> in-tile bilinear interpolation cannot overflow int32.

-- does not hold for `STATIC_MV` tiles. The reproducer uses `tile_x = -9583`,
giving `c.x = -613312` against a `kCornerClamp` of 262144.

**This is reachable without a hostile input.** `kCornerClamp` is 4096 pel and
the picture may be 4096 wide (SYNTAX.md section 2), so the last tile column of a
maximum-size picture has `tile_x + 64 = 4160`, i.e. `266240` in Q.6 -- already
past the clamp, in static mode, on a perfectly legal stream.

No out-of-bounds access was observed: the sampling loop clamps to the reference
picture separately. What is lost is the overflow argument the constant exists
for, and the guarantee that the GLSL twin (`warp/glsl/warp_tile.comp`, which
must agree bit for bit) computes the same corners.

The harness counts this rather than aborting, so it does not mask
memory-safety findings; see the comment in `fuzz/warp_tile_fuzz.cpp` around the
`warp_tile_corners` call.

---

## F7 -- signed overflow inside the corner saturation itself (medium)

**Target:** `warp_tile_fuzz` (after the F6 mutator fix, which was masking it)
**Reproducers:** `fuzz/regressions/warp_tile_fuzz/open/F7-corner_component-{negate-int_min,origin-add-overflow}.bin`, 60 bytes each
**Site:** `warp/ref/warp_ref.cpp:179` and `warp/ref/warp_ref.cpp:181`, in `corner_component()`

```
warp_ref.cpp:179:19: runtime error: negation of -2147483648 cannot be
    represented in type 'int32_t'; cast to an unsigned type to negate this
    value to itself
warp_ref.cpp:181:7:  runtime error: signed integer overflow:
    2147418117 + 1419328 cannot be represented in type 'int32_t'
```

```cpp
    const uint32_t q = nxvc_warp_div(mag, ud);
    v = neg ? -static_cast<int32_t>(q) : static_cast<int32_t>(q);   // :179
}
v += origin << kQCorner;                                            // :181
return clamp_i32(v, -kCornerClamp, kCornerClamp);                   // :182
```

Two separate defects in four lines, and they share one cause: **the clamp is
applied after the arithmetic that can overflow.**

1. `:179` -- `q` is an unrestricted `uint32_t` quotient. When it is
   `0x80000000`, `static_cast<int32_t>(q)` is `INT32_MIN` and negating it is UB.
2. `:181` -- `origin << kQCorner` is added to a `v` that has already been
   allowed to reach the full int32 range, so the sum overflows before
   `clamp_i32` ever sees it.

**Why this is a decoder bug and not a caller bug.** The comment above the
`mag.hi >= ud` branch says the case "cannot happen for any homography that
passes `derive_homography()`'s validation" -- true, but `derive_homography()` is
the *encoder* side. `warp/include/nxvc/warp.h:61-64` is explicit about the
decoder's obligation:

> Legal range of the homogeneous denominator. The encoder MUST guarantee this
> for every corner of every tile it emits; **the decoder saturates if violated.**

A decoder receives `H` from a Phase 2 bitstream, not from `derive_homography()`,
so the out-of-envelope case is exactly the case it must survive -- and it is the
case that overflows. The branch at `:176` saturates only when `mag.hi >= ud`;
everything that gets past it reaches `:179` and `:181` unbounded.

This is the same shape as F2: the saturation is documented, the constant exists,
and the code does not apply it early enough. F2 is the `kModeStatic` path with no
clamp at all; F7 is the `kModeWarp` path with the clamp in the wrong place.

Clamping `v` into `[-kCornerClamp, kCornerClamp]` **before** the `origin` add,
and doing the negation in `uint32_t`, fixes both without changing any
in-envelope result -- but it is a normative change to the corner computation
(and to `warp/glsl/warp_tile.comp`, which must agree bit for bit), so it needs
the warp owner's decision, not a patch from here.

---

## F8 -- the v2 `ctx_level` / `ctx_mode` sentinel is `-1` in the header and `0` in the code (high)

**Target:** `nxvc_rans_fuzz` (found while updating the harness for the v2
entropy model, not by a mutated input -- see "How to reproduce" below).
**Sites:** `ref/src/entropy.cpp:154`, and `ref/src/entropy.cpp:67` with `:131`

`ref/src/entropy.h` documents both new `Unit` fields with `-1` as the sentinel,
and both are `i16` -- signed, which only makes sense if a negative value is
meant to be storable:

```cpp
i16 ctx_level;    // -1: the banded LEVEL contexts; else a fixed context
i16 ctx_mode;     // UNIT_MODE: -1 = bypass coded, else a context index
```

`entropy.cpp` tests them for **truthiness** instead:

```cpp
// :154
op.arg = (u8)(u_->ctx_level ? u_->ctx_level : level_ctx(pos_, prev_class_));

// :67
phase_ = u_->ctx_mode ? kModeSym : kModeFlag;
// :131
op.arg = (u8)u_->ctx_mode;
```

`-1` is truthy, so the documented value takes the "fixed context" branch and
`(u8)(-1)` is **255**. `op.arg` is then used as an index into
`TableSet::ctx[kNumCtx]`, which has 16 entries. The result is a reference 239
entries past the end of the array, and reading `t.slot2sym[slot]` through it
segfaults:

```
Program received signal SIGSEGV
#0 nxvc::RansDecoder::decode_sym (lane=1, ...) ref/src/entropy.cpp:404
      404      u32 s = t.slot2sym[slot];
#1 nxvc::decode_units (nunits=25, nlanes=32, len=288) ref/src/entropy.cpp:492
```

**Why `0` cannot be the intended sentinel.** Context 0 is `kCtxCbfLuma`, a real
context. If `0` means "banded", then no unit can ever select fixed context 0,
and the field would not need to be signed. The header is right and the three
comparisons are wrong; they should be `< 0` (or the field should be `u8` with an
explicit `kCtxNone` constant, and the header updated to match).

**Severity.** High, because it is a wild array index reachable from a value the
public-facing internal header documents as normal usage, and because it is new
code: nothing in the tree passes `-1` today, so no existing test covers it. The
first caller that follows the header gets memory corruption.

**How to reproduce.** This is not input-dependent, so there is no binary
reproducer: *every* input crashes. In `fuzz/nxvc_rans_fuzz.cpp`, change

```cpp
u.ctx_level = 0;   // to -1, the value ref/src/entropy.h documents
```

rebuild, and run any file from `fuzz/corpus/nxvc_rans_fuzz/`. The harness ships
with `0` so the target keeps fuzzing; the line carries a comment pointing here.

**Consequence for coverage, worth flagging.** Because of this, `nxvc_rans_fuzz`
currently drives only the v1 shape: `UNIT_COEF` units with the twelve v1
contexts. `UNIT_MODE` (intra modes, `kModeSym` / `kModeFlag` / `kModeIdx`) and
the dedicated DC-plane contexts `kCtxCbfDc` / `kCtxLastDc` / `kCtxLevelDc` /
`kCtxMode` are **not** fuzzed yet. Once F8 is fixed, the harness should grow
`UNIT_MODE` units and DC-plane contexts; the invariant to add with them is that
every decoded mode is `< kNumIntraModes`, since an out-of-range mode indexes a
predictor table.

---

## F4 -- the `address+undefined` sanitizer preset is unusable (medium, build)

**Site:** `cmake/nxwarp_sanitizers.cmake`, the `undefined` branch

```cmake
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND _nxwarp_san_c_flags -fsanitize=integer
                                    -fno-sanitize=unsigned-integer-overflow)
endif()
```

`-fsanitize=integer` is not a UB check. Minus `unsigned-integer-overflow` it
still contains `unsigned-shift-base`, `implicit-signed-integer-truncation` and
`implicit-integer-sign-change`, all of which flag behaviour that is fully
defined in C++. Combined with `-fno-sanitize-recover=all`, every one of them
aborts.

This is not a hypothetical preset: `CMakePresets.json` uses
`NXWARP_SANITIZER=address+undefined` for **both** `asan-ubsan` and `fuzz`, and
`tests/ref/CMakeLists.txt` points `ref.saturate` at the `asan-ubsan` preset
precisely so that "a signed overflow in the inverse transform aborts" -- i.e.
so that it catches F3. With `-fsanitize=integer` on, F3's report is one of 26
and 24 of the others are noise, so the signal the preset exists for is the
hardest thing in the output to find.

A full `ctest` run with `-DNXWARP_SANITIZER=address+undefined` gave **26 of 45
tests failing**. Re-running with ASan plus plain UBSan gave **2 failures** (F3,
and one load-sensitive flake). So 24 of the 26 were reports on correct code,
including:

| site | report |
|---|---|
| `/usr/include/c++/16/bits/uniform_int_dist.h:375` | `implicit conversion ... changed the value` -- inside libstdc++, via `examples/transport_loopback.cpp:207` |
| `warp/ref/warp_ref.cpp:33`, `:65` | `left shift of 94648 by 16 places cannot be represented in uint32_t` -- the deliberate emulated-64-bit arithmetic |
| `transport/src/aead.cpp:28` | `left shift ... uint32_t` -- a SHA-256 rotate |
| `rc/src/synth.cpp:11` | `left shift ... uint32_t` -- a PRNG |
| `tests/ref/test_util.h:139`, `tests/transport/test_util.h:59` | `left shift ... uint32/64_t` -- the tests' own PRNGs |

Unsigned left-shift overflow is well defined (it wraps); it is exactly how a
xorshift PRNG, a SHA-256 rotate and the warp module's `(hi, lo)` 64-bit
emulation are supposed to work.

Two consequences worth acting on:

1. Drop `-fsanitize=integer`, or narrow it to
   `-fsanitize=implicit-integer-truncation` only and add a suppressions file --
   otherwise the preset the module advertises as "the CI configuration" cannot
   be used by CI.
2. The flags are applied with `add_compile_options()`, which places them
   **after** `CMAKE_CXX_FLAGS` on the command line. A `-fno-sanitize=` passed in
   `CMAKE_CXX_FLAGS` therefore cannot undo them, so there is no workaround from
   outside the module, and none from a preset either. `.github/workflows/sanitizers.yml` and the fuzz job in
   `nightly.yml` consequently ask for `NXWARP_SANITIZER=address` and add plain
   UBSan themselves.

---

## F5 -- YCoCg-R accepted with 4:2:0 chroma (low, spec divergence)

**Target:** `nxvc_headers_fuzz`
**Reproducer:** `fuzz/regressions/nxvc_headers_fuzz/open/F5-ycocgr-with-420.bin`
**Site:** `ref/src/codec_impl.inc:790-795` (stream header validation)

SYNTAX.md section 2, "Constraints a decoder must check":

> `color_transform == 1` requires `chroma_format == 1`.

The decoder checks the ranges of `chroma`, `color_transform`, `alpha` and
`color_space`, and checks that `color_space == RGB` agrees with
`color_transform == YCOCGR`, but never checks the 4:4:4 requirement:

```cpp
if (si.chroma > 1 || si.color_transform > 1 || si.alpha > 1 ||
    si.color_space > 3)
    return NXVC_ERR_BITSTREAM;
if ((si.color_space == NXVC_CS_RGB) !=
    (si.color_transform == NXVC_CT_YCOCGR))
    return NXVC_ERR_BITSTREAM;
// nothing here rejects color_transform == 1 with chroma == 0
```

The reproducer is a header with `chroma_format = 0`, `color_transform = 1`,
`color_space = RGB` and `tools` including `YCOCGR`, and
`nxvc_decoder_parse_stream_header` returns `NXVC_OK`.

Why the constraint exists: YCoCg-R chroma is 9-bit and biased by 256
(SYNTAX.md 5.1), and 5.1 says the transform is applied before subsampling, so a
4:2:0 YCoCg-R stream subsamples 9-bit chroma through an 8-bit plane API. The
decoder then reconstructs planes whose declared and actual ranges disagree.

Note that `color_space` does not appear in SYNTAX.md section 2 at all -- it is a
field the code has and the normative document does not. Either the document or
the check needs to move; this entry is filed against whichever side turns out to
be wrong, and is low severity because no memory-safety consequence was observed.

---

## F6 -- signed overflow in this directory's own warp mutator (fixed here)

**Site:** `fuzz/warp_tile_fuzz.cpp:191` (as it was), in `LLVMFuzzerCustomMutator`

```
warp_tile_fuzz.cpp:191:41: runtime error: signed integer overflow:
    2147483647 + 1636687 cannot be represented in type 'int32_t'
```

```cpp
int32_t v = int32_t(get32(p + 4 * i));
int32_t step = int32_t(rng.below(1u << (10 + rng.below(12)))) - (1 << 9);
set32(p + 4 * i, uint32_t(v + step));   // v + step overflows int32
```

The perturbation is meant to wrap -- the homography entry is a fuzzer-controlled
`int32` and the step may push it past the end of the format -- but doing it in
signed arithmetic is UB, so a UBSan build aborts *inside the mutator*. libFuzzer
then writes a zero-byte artifact, because the crashing "input" is the one being
constructed, which is why the campaign's only warp artifact was empty.

Fixed by doing the arithmetic in `uint32_t`. Recorded here rather than silently
patched because it is the argument for the `--mutate` rounds in the regression
runner: a structure-aware mutator is ordinary code with ordinary bugs, it runs
on every input, and nothing else in the pipeline tests it. That is also why
`fuzz.<target>.replay` runs 400 mutation rounds on every build.

---

## F11 -- the headers harness bounds a plane at 4096, but a stereo plane is 8192 (harness bug)

**Target:** `nxvc_headers_fuzz`
**Reproducer:** `fuzz/regressions/nxvc_headers_fuzz/F11-stereo-plane-width-4094-ci.bin`
**Sites:** `fuzz/nxvc_headers_fuzz.cpp`, the plane-geometry invariant;
`fuzz/tools/run_campaign.sh`, the per-target log directory

Found by CI, not by a workstation run: the `Sanitizers` workflow's
`fuzz-smoke (60s per target)` job, run `33927326224` on `c6e2ffb`, reported
`a target reported a crash` with
`artifacts/nxvc_headers_fuzz/crash-5ca0c279f5f90c081896e8b90694bbf49abb218c`.

**The decoder is right and the harness is wrong.** The input is an ordinary
stereo stream header -- `width` 4094, `height` 64, `eyes` 2, `chroma` 4:4:4,
`tools` `0x09`, `ext_len` 0 -- and it trips the harness's own invariant:

```cpp
if (w > 4096 || h > 4096) __builtin_trap();
```

A picture is one eye and `width` is per eye (SYNTAX.md 3.3), so
`nxvc_decoder_plane_size` correctly returns `eyes * width` -- 8188 here, and
8192 at the maximum legal width. The bound was written for the Phase 1 mono
header and was never widened when `eyes == 2` landed in syntax v1.4. Nothing in
`fuzz/corpus/nxvc_headers_fuzz/` had a wide stereo header in it, so the campaign
only reached it once the mutator built one.

Fixed in the harness by bounding the width at `4096 * eyes`. There is **no
bitstream to reject and therefore no `r<n>` vector**: the constraint the
decoder must enforce is `width <= 4096`, which it already does at
`ref/src/codec_impl.inc:1627`, and it must go on accepting this stream. The
behaviour is pinned instead from the other side, by an assertion in
`tests/ref/test_headers.cpp` case 5 that a 4096-wide stereo header parses and
reports a luma plane 8192 samples wide -- so `ref.headers` fails if
`nxvc_decoder_plane_size` ever stops doubling for the second eye.

**Same defect as F10 on `merge-main`.** `tourney/ctx-b` hit it independently
(commit `a041e3a`) while measuring the v1.5 entropy package, at `width` 4096
rather than 4094, and filed it as F10 with reproducer
`F6-stereo-plane-width.bin`. That fix never reached `main`, which is why CI
found it again here. `main` has no F10 for the same reason: that number is
allocated on `merge-main` and is deliberately left as a gap rather than reused
(F9 was never used on any branch). Both reproducers are worth keeping -- 4094 is the odd-width case and
4096 the boundary -- but the harness change is one edit, so a cherry-pick onto
`merge-main` carries only the new reproducer, the campaign-log fix below, and
this entry.

**Second, genuinely new defect: the crash log was thrown away.** The uploaded
`fuzz-{0..3}.log` in `fuzz-smoke-artifacts` are all clean `DONE` runs, because
`run_campaign.sh` ran every target with `cd "$OUT"` and libFuzzer writes
`fuzz-<N>.log` relative to the working directory. Each target overwrote the
previous target's logs, so the artifact that survived was `warp_tile_fuzz`'s --
the last target in the loop -- and the stack trace for the target that actually
crashed was gone. Triage started from the raw crash input rather than from a
report. Fixed by giving each target its own `$OUT/logs/<target>/`, uploading
that directory, and printing the tail of any worker log containing a sanitizer
or libFuzzer error before the script exits non-zero, so the diagnosis survives
even when the artifact does not.

**Severity: harness, not codec.** No memory-safety consequence in `ref/`. It
matters because it was aborting the headers target inside the first minute and
masking everything the campaign would otherwise have reached past it -- the same
argument F10 makes. After the fix, a 300-second two-worker campaign from the
same corpus, seeded with this reproducer, ran 310 252 inputs clean and wrote no
artifact.

---

## Not findings

Recorded so the next run does not re-triage them.

* **`hybrid.e2e256` failed once under `ctest --parallel 4` and passes on its
  own.** Load-sensitive, not a sanitizer report; no ASan or UBSan output was
  produced. Worth a look by whoever owns `hybrid/` if it recurs.
* **`UNIT_MODE` and the v2 DC-plane contexts are not fuzzed yet**, blocked on
  F8. See the last paragraph of that entry for what to add once it is fixed.
* **`scan_frame` / `decode_frame` length agreement.** `nxvc_decode_fuzz`
  counts, but does not abort on, a disagreement between the bytes
  `nxvc_decoder_scan_frame` reports and the bytes `nxvc_decoder_decode_frame`
  consumes. None was observed. The counter is kept because the two entry points
  walking a frame differently is exactly the shape of bug that turns into an
  out-of-bounds read later.
