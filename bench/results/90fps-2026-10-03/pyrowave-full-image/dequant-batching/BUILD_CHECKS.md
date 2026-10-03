# Integrated build and correctness checks

Source: WiVRn NX branch `pyrowave-probe`, commit `718d6887838f4e29f41e45800138d4e7a67c7aa4`.

- Built and linked complete Android `wivrn` shared module with `PYROWAVE_HAAR_FORMAT=ON`.
- Saved that exact CMake-built archive; linked standalone Pico helpers against production headers and archive.
- Built and linked complete Android `wivrn` shared module again with `PYROWAVE_HAAR_FORMAT=OFF`; cached build now restores default CDF.
- CMake 3.31.5, Android NDK 29.0.14206865, Release, arm64 API 29.
- Integrated native 4352 x 2176 Haar 4:2:0: six changed packets, 18 readback planes byte-exact to desktop reference. See `evidence/verified-planes.log`.
- Existing CDF: separately built batched candidate's Pico readback matched prior Pico CDF baseline byte-for-byte across all three planes; maximum difference from host was 1 byte level, as before. This is a correctness gate, not a CDF speed result.
- Private batched Haar candidate also passed host 4:4:4, 4:2:0 and non-32-aligned 1920 x 1080 output checks.
- Source whitespace check passed.
- No APK installation, live-stream validation or photon measurement performed. Server remains off; temporary standalone device files removed.

## Build logs

`nx-batch-build-on.log` completed successfully; final step: `[26/27] Linking CXX shared module client/libwivrn.so`.

`nx-batch-build-off.log` completed successfully; final step: `[32/33] Linking CXX shared module client/libwivrn.so`.

## Final artifact hashes

```text
afb25f42b4a8fea0f8d58fdbffefade6e5fa229f8ff7b4366108f10d9201c760  /run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-batched-integrated-20261003/libhaar.a
05a75b2cdfb97967d3e01674d852e53af09b951751fe4dac39875597d7ccf62c  /run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-batched-integrated-20261003/bench
d6850da7440a0064f752baf022abd83e226c2d4b9d891494151b00b2ae0baef8  /run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-batched-integrated-20261003/readback
```
