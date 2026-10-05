# Unsigned recovery APK with matching package ID

The release package now builds with the existing installed NX package ID, **`org.meumeu.wivrn.nx`**, by passing supported `-Psuffix=`. Source `6e2293d58acc8b7a20e9276ae25f5e97257b37d9` is unchanged. Offline assembly succeeds in5s; this is build time, not codec timing. Root independently repeats the package verifier and obtains byte-identical review JSON, verifies the copied APK hash, package name, alignment, current native commit/property strings and native Build ID continuity.

The earlier `.nx.local` release is a different package and cannot be treated as the deployed NX client. A read-only device package query reports installed NX versionName `nx-1.4.1`, code1, update time2026-10-04 16:35:02; this identifies package metadata, not the installed native commit/profile. The new unsigned build is versionName1.0/code1. Package name alone does not establish signing/version compatibility or runtime correctness.

```sh
JAVA_HOME=/run/media/nerdrx/Lex/claude/tools/jdk-21.0.12+8 \
ANDROID_HOME=/run/media/nerdrx/Lex/claude/tools/android-sdk \
PATH=/run/media/nerdrx/Lex/claude/tools/jdk-21.0.12+8/bin:$PATH \
  ./gradlew --offline -Pnxwarp_dir=/run/media/nerdrx/Lex/claude/nx-warp -Psuffix= assembleRelease

python3 ../quiet-retirement-kernel/package/verify-package.py /path/to/wivrn-nx \
  /path/to/android-sdk/build-tools/35.0.0 /path/to/scratch-output
```

Run from the configured checkout with cached offline dependencies. Gradle `build.gradle:107` sets baseID and `:178` adds the release suffix; an explicit empty suffix overrides the `.local` default. Native libSHA256 `7d669a24184e077c4dec7f5c0e111817397f9dcb5b7c91d9e398053689a972ee`, BuildID `6d07949f6352ce36bc83b61869e42c11d5599a11` match the earlier current-source native build. APK SHA256 `6f6f5ca5c668adfa5fdfd02c09fcb598d7e85fb79e51e7fdba58c9fc0cdc8ee7`.

Signature verification fails as expected because the package is unsigned. **Not install-ready.** No signing/install, device app launch/stop, property activation, live profile/session change or APK/ELF publication occurred. Build outputs update normally; no source or cache-configuration edits were made. Actual Pico client remains running and untouched. A later explicitly authorized test needs a compatible signed package, matched server/client identities, deliberate opt-in and actual fresh-display correlation.
