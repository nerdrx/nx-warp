# ASTC frame freshness

NX ASTC frames are independent. When the measured adaptive playout delay is zero,
select the newest **complete stereo pair** from the existing frame intersection.
Server display timestamps can be predicted beyond the client's current refresh;
choosing the timestamp nearest that refresh can otherwise hold a ready newer image.

An enabled de-jitter setting can still measure zero delay on an early-arriving stream.
The decision follows the measured delay rather than the setting's enabled flag.
A positive delay retains the existing nearest-target selection. Alpha streams retain
the existing selection, and the diagnostic nearest override remains available.
No image generation, reconstruction, GPU dispatch, history buffer or network change
is required. This changes which already-decoded pair is presented.

Validation: Android native build and data-preserving signed APK update passed.
The existing de-jitter test passed 41,096 checks. Short stationary Pico captures
recorded no backward source transitions and approximately 90 render iterations/s.
The new policy reported zero older-than-available choices in the zero-delay sample.
Comparison timings varied with reconnect and source clock phase: this is evidence
for the selection rule, **not a fixed millisecond latency improvement**. Off-head
resume can still show a retained old frame before new frames arrive. No sustained
motion, headset visual-quality or photon-latency claim is made.

Experiment evidence belongs in the separate NX Warp results repository under
`bench/results/90fps-2026-10-04/astc-frame-freshness/`.
