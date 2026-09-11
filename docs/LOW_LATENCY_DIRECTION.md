# NX Warp: an independent low-latency alternative

## Decision — September 11

The independent NX codec remains one research path. A subsequently requested parallel experiment uses full-frame hardware HEVC plus NX/WiVRn headset motion warp. That hybrid depends on HEVC for compression; it does not establish an independent replacement format. Both paths must justify latency and visual quality with measurements.

The objective is lower measured delivery latency at useful visual quality and sustainable bandwidth. More fresh frames are valuable, but do not by themselves establish lower latency. Preserve the working 1024px native sharp centre at 2688 pixels per eye. Keep initial experiments to short, approximately 30-second screens.

## What the evidence rules out

The current centre costs roughly 5–6 ms of entropy work and 8–10 ms of reconstruction on the measured Pico runs. Splitting reconstruction and adding special cases has not produced a useful overall win. Transform skip moves cost into entropy decoding. Sending frames faster increases waiting. The current representation can exceed its bitrate target by about an order of magnitude.

Consequently, tuning the existing stages independently is insufficient. A candidate must reduce combined processing, bytes transferred and frame age. Published results remain evidence for specific configurations, not proof of general codec superiority.

## Representation-first experiments

1. **Account for every centre byte.** Attribute metadata, coefficient payload, unchanged content and peripheral payload on fixed motion fixtures. Separate actual payload rate from the controller's requested bitrate. Identify which information the receiver truly needs.
2. **Test directly usable tile data.** Compare a small fixed-layout representation that avoids transforms and variable-length coefficient parsing with the current centre. Keep pixel locations native. A CPU/reference reconstruction check precedes the Pico test; record visual error and actual byte count alongside GPU cost.
3. **Reuse only demonstrably useful history.** Pose-aware prediction and sparse corrections must survive translation, disocclusion and full-field change. Static reuse is a feasibility check, not the success case. Cap dependencies and queued corrections so old work cannot block a newer useful update.
4. **Combine work with presentation where practical.** Sample retained or directly usable tile data in the existing presentation pass when that removes a reconstruction copy. Count extra reads, dispatches, synchronization and stereo alignment costs.

A texture-block representation is one possible feasibility experiment, not a selected format. Even 2 bits per pixel for two 1024² centres at 90 complete updates/s requires approximately **377.5 Mbit/s before transport overhead**. Cheap GPU sampling alone does not solve the network problem. Reject any design that reaches its bandwidth target only while the scene is still.

## Experiment gate

Use nearby controls with the same scene, requested eye dimensions and centre extent. Report fresh source selections, actual bytes, decode and presentation cost, queue age, loss recovery and visible error. Include full-field motion. A faster stage that increases total waiting or another stage's cost is not a win.

Do not hide a bandwidth problem behind a lower update rate without reporting it. Distinguish each-eye freshness from synchronized stereo delivery. Signed display-time proxies are not physical motion-to-photon measurements. A claim of beating HEVC requires comparable timing endpoints and disclosed quality/bitrate differences.

Keep the stable profile available. Retain a prototype only after a repeat short comparison shows a useful overall improvement; larger validation requires a separate reason and authorization under the current short-test preference.

## Reference evidence

- [Hardware baseline, including 8-bit and 10-bit HEVC](../bench/results/90fps-2026-09-11/hardware-baseline/README.md)
- [Native transform-skip tradeoff](../bench/results/90fps-2026-09-11/native-tskip/README.md)
- [Fixed-pacing latency regression](../bench/results/90fps-2026-09-11/pace60-screen/README.md)
- [Retained large-centre stability evidence](../bench/results/90fps-2026-09-11/large-centre-soak/README.md)

## Full-frame hardware experiment

[HEVC with headset motion fields](../bench/results/90fps-2026-09-11/hevc-motion/README.md) now executes as an opt-in prototype. The final short screen selected 84.39 fresh source frames/s, but extrapolation was usually zero or tiny. No latency or perceptual gain is demonstrated. Resolve scene-time versus predicted-display-time semantics before increasing extrapolation. The original NX profile remains selected.
