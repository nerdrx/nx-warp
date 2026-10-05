# Queue diagnostic APK gate

The report is at `/run/media/nerdrx/Lex/claude/nx-warp/bench/results/90fps-2026-10-05/continuous/QUEUE_DIAGNOSTIC.md`. It documents the diagnostic as
default-off and makes clear that runtime values remain unmeasured. Checkout HEAD is
`831aafed88e16569aa09d78f2d34d812ce8a9b6c`; no tracked source/config changes
were made.

The final arm64 native artifact is
`wt-pyrowave-probe/build/intermediates/cxx/RelWithDebInfo/33s4w1c5/obj/arm64-v8a/libwivrn.so`; final APK/native hashes are in `artifact-sha256.txt`. The pre-assembly hash is retained separately in `native-before.sha256`. The final library contains the
`debug.wivrn.nx.astc_queue_timing` property string and the queue summary format.
Before assembly, the APK at the same output path was dated Oct 3; that old package was not used as diagnostic evidence. The corrected build replaced it with the package hashed below.

`assembleRelease` initially failed because Gradle selected cache `20df562k`,
whose NX Warp path was stale. The supported invocation property
`-Pnxwarp_dir=/run/media/nerdrx/Lex/claude/nx-warp` selected the already-correct
Gradle CMake cache and completed assembly successfully. Full output and exit
status are in `assemble-release-correct-nxwarp.log`.

Reproduction attempted from the checkout root:

```sh
JAVA_HOME=/run/media/nerdrx/Lex/claude/tools/jdk-21.0.12+8 \
ANDROID_HOME=/run/media/nerdrx/Lex/claude/tools/android-sdk \
PATH=/run/media/nerdrx/Lex/claude/tools/jdk-21.0.12+8/bin:$PATH \
  ./gradlew --offline \
  -Pnxwarp_dir=/run/media/nerdrx/Lex/claude/nx-warp assembleRelease
```

The produced APK is `build/outputs/apk/release/wt-pyrowave-probe-release-unsigned.apk`.
`aapt dump badging` verifies package `org.meumeu.wivrn.nx.local`, version
`1.0` / code `1`; full output is `apk-badging.txt`. Its packaged arm64
`libwivrn.so` is byte-identical to Gradle's stripped native library, and
contains both the queue-timing property string and stream-tagged summary. The
comparison copy, hashes, and alignment check are retained here. The APK is
unsigned, so this is reviewable package proof, not install-ready or live
verification. No signing material was read or printed, and no APK was
installed or published. Earlier failed attempts remain in their original logs.

## Independent root checks

Root extracted the packaged arm64 lib in memory and confirmed exact bytes against Gradle stripped output, the diagnostic property string, the correct NX Warp CMake cache, and APK/native SHA256. Root independently ran aapt (exit0), zipalign (exit0) and apksigner verify (exit1, expected unsigned verification failure). Full outputs and exit codes are retained. [Machine-readable artifact review](root-review.json). This package cannot be treated as install-ready or as evidence of enabled runtime metric correctness. No APK or ELF binary is published here.

Raw build logs retain original trailing whitespace. The documentation/source diff check excludes `.log` files; no raw evidence was reformatted to satisfy whitespace checks.
