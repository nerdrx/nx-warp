# Installed client identity: current recovery code is not deployed

The installed `org.meumeu.wivrn.nx` APK was copied read-only over ADB on 2026-10-05 at approximately 21:32 UTC. Its whole-APK and native-library hashes match the [archived installation manifest](../../../90fps-2026-10-04/live-failure-recovery/final-install-manifest.json) exactly. The embedded source identity is **c514841f87b9718a50c974e79c3e902ef38a3a82**, not the current source **6e2293d5**. Signing verification passes with the archived certificate. Root independently runs the published local verifier against the copied artifact.

| Evidence | Observed |
| --- | --- |
| Package/version | org.meumeu.wivrn.nx / nx-1.4.1 / code1 |
| APK SHA-256 | 021077c6873ee8a626b35442e3c27d36c2cbb1727e23e91f41e0cae5e3386e5b |
| Native SHA-256 | 1b671b13000b82356bda3ec4a05ca19a5fcc03d330839d35484358817bc7ba72 |
| Native Build ID | 2ebfebc018a72755c199815b20a1eef4b6dd196e |
| Certificate SHA-256 | fb6a4688fd7c9234f42ba6c2c9efe5922e6a804c805ab0982b04b6c02afbc150 |
| Last package update | 2026-10-04 16:35:02, device-reported |
| Client process | PID9926 remains running |

The current `recovery_poll`, `astc_deadline` and `astc_queue_timing` property strings are absent from this native library and the corresponding archived source files. Those newly built receiver features therefore cannot be credited with current headset behavior. This corrects the earlier unknown installed-artifact boundary; it does not establish which module is loaded in the running process or the current profile, option values, frame delivery or latency.

## Reproduce locally

`verify.py` accepts a previously copied APK and never connects to a device. It checks exact artifact/native hashes, the source token, ELF Build ID, parsed package name and verified signing certificate against the archived manifest. Configure the existing JDK for `apksigner`:

```sh
JAVA_HOME=/path/to/jdk PATH=/path/to/jdk/bin:$PATH \
  python3 verify.py /path/to/copied-base.apk \
  ../../../90fps-2026-10-04/live-failure-recovery/final-install-manifest.json \
  /path/to/android-sdk/build-tools/35.0.0 /path/to/scratch-output
```

Public files contain the runnable verifier, review JSON, package/signature/ELF metadata and archived source-token checks. APK and native ELF stay in local scratch and are not published. The initial line-only commit search missed a source string preceded by binary bytes; the final verifier searches the expected full commit directly and binds it to both exact hashes.

No device benchmark, app execution, installation, signing, property change, wake-up, restart or session/profile change occurred. Reading an installed artifact does not prove the deployed decoder runs the prepared recovery path, or that recovery is faster. The matching-ID new package remains unsigned and uninstalled; a deliberate matched server/client test is still required.
