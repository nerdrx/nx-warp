# Guarded motion-history candidate: ready for controlled live testing

## Assessment

The candidate has passed the available code-level readiness checks. It is ready for a controlled Pico trial, **not a production-quality declaration**. Headset disconnection prevents activation, perceived smoothness, tracking, stereo and thermal validation. The earlier scene still contains bent silhouettes; these fixes do not solve correspondence or occlusion.

## Changes completed

- Source continuity follows the estimator's exact predecessor timestamp, so replay-driven ID skips do not unnecessarily reset history.
- The quantization range recovers after large motion instead of keeping later small motion coarse.
- Disabled EMA no longer copies fields; blending normalizes its weights once rather than dividing each vector component.
- Extreme finite scales cannot overflow intermediate vector arithmetic.
- The packet assembler rejects nonfinite/negative/out-of-producer-range scale and nonpositive/excessive field spans before use by either raw or filtered warp. Zero-scale stationary fields remain valid. The scale ceiling matches the estimator's existing0.25-eye-width limit; span is below500ms.
- Existing head-pose checks, raw fallback, exact source matching, and11.11/22.22ms limits remain. No extra GPU pass, blur, or network payload is added.

## Checks

- Motion-history assertions pass with undefined-behavior and float-cast-overflow sanitizers, including continuity, quantization recovery, malformed metadata and extreme scales.
- Packet serialization/reassembly:273 checks,0 failures.
- Retention regression passes skipped IDs, duplicates, stale/out-of-order sources and capacity.
- Final Android assembleRelease succeeds in13seconds.

The CPU helper microbenchmark measured64² grids at p50/p95=28.85/28.94 microseconds and128² at93.49/100.15 microseconds on this PC. The prior implementation measured24.12/27.16 and97.39/99.76 respectively. These short host measurements show no robust speed win; they establish only the approximate helper cost, not headset timing or end-to-end latency. The removed disabled-path copy is established by code inspection.

## Handoff

The exact ready APK is saved locally as nx-scratch/motion-regions/pico-motion-history-rc.apk; status.json records its hash and revision. The currently installed client is unchanged. When ADB reconnects, install this package, keep the existing resolution/bitrate profile, and run a short matching moving-scene trial with the actual image visible. Compare EMA disabled/enabled, source delivery and abrupt source changes. Roll back on stereo mismatch, visible jitter regression, crashes or sustained missed deadlines. No240FPS or physical latency claim is made.
