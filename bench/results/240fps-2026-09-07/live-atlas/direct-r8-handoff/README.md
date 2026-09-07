# Direct R8 atlas handoff

This is an initial empirical A/B of the borrowed R8 atlas target. The control
APK is `0582f10bd6a4c45da161981381122ad539b4deab212a1fdb1595ce8db23e8e99`;
the probe APK is
`f826b240fb5db2864c365f2af9e380f2d3a7a8e5bcbef5bd17efd2bd82f71edc`; both
used server `a7b47b3e164eaa1578774526598911546b00ae83b55ba7a33f874e2504040d82`.
The capture uses the static checkerboard/cubes scene and 90 Hz pacing.

The available active 2 s window means are control: decoder 1.2 ms, copy
0.28 ms, wall 2.4 ms, render 2.7 ms; probe: decoder 1.3 ms, copy 0.01 ms,
wall 2.6 ms, render 2.6 ms. Both report about 89 new sources/s. These are
window means, not frame percentiles or physical display FPS. They show a
large copy reduction in the probe, but no end-to-end wall-time speedup.
The reverse arm is retained alongside the same raw logs and manifest for the
matched comparison; session gaps and active-window sampling limit causal
claims. The probe remains opt-in pending further matched measurements.

`atlas` counters in the logs confirm the decoder path is active; the renderer
label `atlas-prototype 0` describes the separate renderer prototype and does
not mean the codec atlas was inactive. Screenshots are retained as capture
artifacts, not image-quality evidence. The three patch files preserve the
capture source variants.

`summary.json` is regenerated with `summarize.py` from the three retained
measure logs. It reports each printed 2 s window and separates direct-target
counts; it does not infer cadence across session gaps.
