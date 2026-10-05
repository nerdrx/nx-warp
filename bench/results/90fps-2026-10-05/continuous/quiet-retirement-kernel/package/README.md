# Current-source Android package, not installed

Source `6e2293d58acc8b7a20e9276ae25f5e97257b37d9`, configured checkout `wt-pyrowave-probe`. Offline release assembly completes in 8 seconds (52 tasks, 9 executed). This is build time, not codec timing. No source edits were made here.

The packaged arm64 library exactly matches Gradle's stripped output, and its Build ID matches the unstripped native module. The full current source commit and both recovery/deadline property strings are present. APK manifest is `org.meumeu.wivrn.nx.local`, version 1.0/code 1; do not use this unchanged app version as commit identification. [Artifact hashes and review](review.json) pin the actual package.

`aapt` and `zipalign` checks exit 0. `apksigner verify` exits 1 because this release package is unsigned, as expected. It is not install-ready; no signing, install, property activation, headset test or binary publication occurred. Static identity checks do not prove runtime correctness or live recovery.

Reproduce with the same configured checkout and available offline dependencies:

```sh
JAVA_HOME=/run/media/nerdrx/Lex/claude/tools/jdk-21.0.12+8 \
ANDROID_HOME=/run/media/nerdrx/Lex/claude/tools/android-sdk \
PATH=/run/media/nerdrx/Lex/claude/tools/jdk-21.0.12+8/bin:$PATH \
  ./gradlew --offline -Pnxwarp_dir=/run/media/nerdrx/Lex/claude/nx-warp assembleRelease

# Needs Java on PATH for the SDK signature verifier. Output contains a scratch ELF.
python3 verify-package.py /path/to/wt-pyrowave-probe \
  /path/to/android-sdk/build-tools/35.0.0 /path/to/scratch-output
```

Default artifact paths follow this checkout's existing Gradle configuration. A different ABI/cache or checkout name requires updating those paths. The script never signs, installs or activates options. Root's repeated verifier produces identical review JSON; full logs and ELF notes are retained here without the binaries.
