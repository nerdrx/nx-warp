# Motion-history stability fixes

Two small client fixes, host-tested and Android-built. **Not installed: ADB currently lists no Pico.** No new live FPS, jitter or latency measurement is claimed.

## Source continuity

The old filter required adjacent compositor IDs. Replayed commits advance IDs without advancing application source images, so consecutive usable source fields can have IDs such as10 and12. That unnecessarily reseeds history and selects the11.11ms cap instead of the22.22ms active-history cap.

The new check accepts increasing IDs when the current source timestamp minus its source span exactly equals the retained source timestamp. This identifies the estimator's actual predecessor. Missing timestamps retain the adjacent-ID rule. Real temporal gaps, repeated/backward timestamps, incompatible spans and head motion still trigger fallback. This avoids treating any recent-but-unrelated field as continuous.

## Precision recovery

The old output quantization scale was max(current_scale, old_scale). One large motion could therefore keep later small estimates coarse indefinitely. The new range is half current plus half span-normalized previous range, the bound of the blended values. It remains sufficient to avoid saturation and decays as the old motion decays. No new dispatch, image pass or allocation is introduced by these changes.

A regression starts with scale1 and repeatedly blends scale0.001. After16 blends the new range is below0.0011; the old max range stays1. This is a representation test, not a measured visual jitter reduction.

## Validation

The standalone motion_history_test binary passes, covering continuous skipped IDs, true source gaps, repeated timestamps, missing metadata, scale/span normalization, range recovery, opposite vectors, invalid metadata and head-pose rejection. Android assembleRelease passes in18seconds. Source and regression are in the WiVRn NX atlas-live branch; the ready APK hash is in status.json.

Build uses -Pnxwarp_dir=/run/media/nerdrx/Lex/claude/nx-warp -Psuffix=.warp with the existing Android toolchain. The currently installed f799d6b9 client is unchanged. Next step is a short matched Pico test when ADB reconnects, checking activation and source-switch stability before any claim of faster feel.
