# Sparse coefficient clear candidate — rejected

The corrected Pico run produced exact output but was slower: **30.6606 ms GPU p50 / 31.574 ms p95**, versus **26.1212 / 26.3982 ms** for valid sequential Haar. It is not integrated. See [candidate samples](pico-sparse-bench.log) and [control samples](pico-control-bench.log).

The notes below describe the initial prototype. Later validation corrected the full-subgroup flag on Haar inverse and the host API-level declaration. See [clean validation](validation-clean.log).

Offline-only experiment based on the paired-Haar candidate at
`/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-haar-20261003`. The patch
changes only the private decoder and dequant shader. It clears every mip and
array layer in the high-resolution coefficient image (and optional low-res
image) before dequantization, skips the per-block zero stores for absent blocks,
and skips stores for individual zero coefficients. Images already include
`TRANSFER_DST` usage. Each decode records an execution/memory dependency from
previous compute/fragment reads/writes to transfer, transitions Undefined to
TransferDst, clears, then transitions TransferWrite to General with
ShaderRead|ShaderWrite visibility. The existing compute-write to inverse-read
barrier remains after dequantization.

## Verification

- Feature-enabled RX 7900 XTX host decode of the 4352x2176 Haar 4:4:4 fixture
  produced Y/Cb/Cr planes bit-for-bit equal to the prior host decode.
- A single Decoder instance decoded dense source then neutral black. The second
  decoded frame had Y/Cb/Cr MAE 0 against neutral black, showing absent blocks
  did not retain coefficients from the preceding frame.
- The Android arm64 static archive compiled. `android-objects/pyrowave_decoder.o`
  is AArch64 ELF. The initial build did not include Pico timing; the later isolated run above did. No APK was installed or live playback tested.
- A host validation-layer run reported three existing shader/pipeline VUIDs
  (SPV_KHR_8bit_storage enablement and full-subgroup local-size constraints).
  It reported no clear/layout/synchronization VUID. See `validation-layer.log`;
  this is not a clean validation-layer pass.

The whole-image clear adds image-bandwidth work. The measured extra clear cost outweighs the avoided zero writes. The existing `CDF` default was not
modified by this private Haar-only patch.

## Reproduce

Private source, fixtures, and decoded planes are retained in
`/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-sparse-clear-20261003`.

```sh
cd /run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-sparse-clear-20261003
./build-host-paired-haar.sh
./pyrowave-haar-host-decoder validation/reference-haar.pyrowave validation/sparse-clear
./validation/readback-sequence validation/dense-then-black.pyrowave validation/sequence
./build-android-sparse-clear.sh
```

The second host command compares each generated plane bitwise with
`validation/reference-{y,cb,cr}.raw`; the multi-frame harness decodes the dense
source followed by a 25,412-byte black-frame packet using the same Decoder.
Patch SHA-256: `585dc21197e5a101dac1c81672548983e01178711eda458b8110cf923902f9c6`.
Android archive SHA-256: `9631627d990e7efb7859945c3787dc88ade41ac109ce2aff1bbcd8b50b53b6bb`.
Host decoder SHA-256: `7cc47da0831e9c3e4457ec35a5732e0ed96006848b1fa1c5490e305139de6a0c`.
