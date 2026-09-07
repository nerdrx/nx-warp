# Pico 4 automatic preset verification

**Dated caveat (2026-09-07): the prior output-scale renders were cropped/zoomed because viewport dimensions were scaled while foveation runs were not. This applies to both the 0.40 preset and AUTO 0.50. Timing checks remain genuine; full-field corrected measurements are pending.**

Client commit: `7c884d68` on WiVRn NX `atlas-live`.
Signed APK SHA256: `b8ce06b966bb46cddaae58f63a5dbe03f6dbbbfe9439a8b6209a861aa1c04fee`.

The automatic-model smoke reports direct targets active and 864×864 output per eye
(scale 0.40). With `debug.wivrn.nxwarp_atlas_speed=false`, the same APK reports
direct targets zero and 1088×1088 output (AUTO scale 0.50). The original empty
property value was restored and checked after testing. The speed-preset APK
remains installed; benchmark-owned processes were stopped.

These short checks verify model selection and its override, not performance.
See [180-second ABBA evidence](../abba-180s/README.md) for the measured tradeoff.
Explicit configured output scale takes precedence over AUTO. Other models retain
their defaults. Raw screenshots are capture artifacts, not quality equivalence.
