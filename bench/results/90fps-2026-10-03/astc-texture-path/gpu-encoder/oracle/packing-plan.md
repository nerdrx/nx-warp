# Fixed ASTC 8x8 LDR RGB block

Validated with pinned Basis Universal `basisu_astc_helpers.h` CPU oracle.
The helper packs/unpacks 34 test blocks (flat, ramp, 6x6 comparison, and 32
deterministic random blocks); all 5x5 direct blocks matched the upstream
`pack_astc_block()` byte-for-byte and survived `unpack_block()` field checks.
Build and run: `cmake -S . -B build -DBASIS_DIR=/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003/basis_universal && cmake --build build -j6 && build/astc_fixed_oracle`.

## Fixed mode and bit layout

- Footprint: 8x8 texels; one partition; CEM 8 (LDR RGB direct).
- Weight grid: 5x5, ISE range 5 = 8 weight symbols, 3 bits each.
- ASTC block-mode config: decimal 243, hex `0x0f3`; low 11 bits are `0x0f3`.
- Bits 11..12: partition count minus one = 0.
- Bits 13..16: CEM = 8.
- Bits 17..52: six endpoint symbols, each 6 bits, order
  `R0,R1,G0,G1,B0,B1`. Endpoints are the two RGB endpoint colors; do not
  reorder channels or endpoint pairs.
- Bits 53..127: 25 3-bit weight symbols in reverse sequence/bit order.
  For logical ISE weight `w[i]` (row-major i=0..24), write `reverse3(w[i])`
  at bit offset `125 - 3*i`. This is exactly Basis's `rev_dword()` mapping.
- Endpoint ISE range is 14 (64 values), using exactly 36 bits. The layout is
  full: 17 header + 36 endpoint + 75 weight = 128 bits.

The direct packer is `direct_5x5_cem8()` in `oracle.cpp`. It writes LSB-first
into four 32-bit words, so on little-endian hosts the word bytes are the ASTC
block bytes. In GLSL, write each bit with shifts/ORs into `uvec4`, then store
the four words little-endian (or explicitly byte-swap on a big-endian host).

## Reference vectors

- Flat endpoints `{10,10,25,25,50,50}`, 25 zero weights:
  `f3001525cb6419000000000000000000`; decoded row R is eight copies of 40.
- Ramp endpoints `{0,63,0,63,0,63}`, row-major grid weights
  `round(x*7/4)` repeated by row: `f300811ff881ff8df0467823bc11de08`.
  Decoded R row: `0,40,80,124,159,179,215,255`.
- 6x6 range-2 alternative has mode config `0x108`, but endpoint range 15
  (80 levels, 38 endpoint bits), not six-bit endpoints. It is not the direct
  layout above.

`oracle.log` contains the full run output. The CPU oracle validates packed
layout and decoding; it does not validate the root shader's endpoint fitting,
weight fitting, or image quality.
