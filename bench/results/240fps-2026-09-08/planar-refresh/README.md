# PLANAR refresh decoder evidence

This directory preserves the decoder evidence for the opt-in flat PLANAR
specialization and the default unused-fill behavior.

The flat path is opt-in through `NXVC_VKD_PLANAR_FLAT`; ordinary decoding keeps
the generic Pass B path. PLANAR tiles skip the unused Pass-A coefficient/unit
buffer fills unless `NXVC_VKD_PLANAR_FORCE_CLEAR` is set. The selector only
chooses the flat module after parser validation establishes an all-PLANAR,
full-resolution, aligned 8-bit 4:2:0, CT_NONE, no-alpha frame with R2/coarse
zero-slope bodies.

`planar-final-conformance.log` is the host Radeon RX 7900 XTX run: 260 streams checked,
3 skipped, 0 failures. The earlier `planar-pico-conformance.log` is an
interrupted full Pico run. Its frozen vectors pass through v35, then directional
and related cases fail from v36 onward; it must not be treated as a completed
pass. `planar-pico-control-synthetic.log` is the quick control with flat mode
off and `NXVC_VKD_PLANAR_FORCE_CLEAR=1`; it reproduces the synthetic failures
(14 failures), so those failures remain unresolved and are not attributed to
the flat selector or fill bypass.

The six `flat-wired-paired-*.log` files are paired native 120-frame decode-only
runs at 4352x2176 on Adreno 650. Their p50 frame wall times range from 9.849 to
11.432 ms for the flat arm and 31.057 to 31.649 ms for the generic arm in the
paired runs (first two frames excluded). These are measured run results, not an identity or sustained
240-fps claim. The paired logs use the same GPU-fit fixture.

The paired fixture is the GPU R2/coarse fit stream `native-gpu.nxv` (SHA-256 `9d4e65022521dfb8d45ec72851ca588012a7a622fd1d2001b3cf70e369d4399b`). Timing includes reconstruction and output stores, but excludes transport and compositor work.
