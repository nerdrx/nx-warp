# q6 endpoint-precision audit

Read-only source/report audit, 2026-10-05. No source edits, builds, timed trials, GPU/device/app work, or private pixel outputs.

## Decision

A narrowly defined **q6-refit output with endpoints rounded to the q5 lattice while preserving each block's mode and weight symbols** is not an exact repeat of a retained probe. It is a small, potentially useful compression/quality gate, but not a high-confidence or high-impact optimization yet. Do not change production from source inspection alone. A plain q6-to-q5 endpoint-bit change is already an existing quality rung and is not new.

In `server/shaders/astc_encode.comp:31-46`, `quantEndpoint` maps q6 to `endpointBits=6` and step 1 in the ASTC six-bit code domain; q5 maps to five bits and step 2. Both q6 and q5 retain the same ordinary 5×5, one-plane 3-bit weight mode (`0x0F3`) and Q8 weight alphabet `{0,9,18,27,37,46,55,64}`. The q6-only endpoint least-squares refit is guarded at `:109-143`; q5 skips that refit. Thus the proposed retained-q6-mode/weight hybrid differs from the standard q5 rung because q6's refitted endpoints and weight symbols are kept. A new experiment must post-quantize the actual q6 output endpoints, not simply encode at q5.

ASTC is always 16 bytes per block here; lower endpoint precision cannot reduce raw ASTC payload size. Any transport gain must appear in Zstd's frame length, traded against decoded RGB error. If q6 selected dual-plane mode `0x10442` (`astc_encode.comp:154-172`, `astc_encode_colour.glsl:74-80, 138-140`), a faithful gate must retain its mode and both weight planes too, and round/repack its Q160 endpoint values legally; testing only ordinary blocks must disclose coverage.

## Prior probes and boundaries

- The closest retained test is the q6 endpoint least-squares refit: it held 5×5 weight symbols, used existing six-bit endpoint precision, and repacked the same ASTC mode. On two exact photo fixtures it gained only 0.070/0.116 dB, added 164/485 Zstd bytes, and cost about 0.029–0.040 ms GPU encode time. It did **not** test coarser endpoint precision after refit (`nx-warp/bench/results/90fps-2026-10-04/dense-colour/endpoint-refit/README.md`, `comparison.csv`).
- The earlier q2 endpoint-precision contrast gate and luma/chroma transform changed the weight regime as well as endpoint treatment. They were rejected: the 6-bit-on-high-contrast / 3-bit-otherwise gate gained little PSNR but grew Zstd 6.5–12%, and its state flipped on 1,254 then 1,395 blocks across a synthetic one-pixel pan. The luma/chroma candidate grew Zstd 12.3–34.7% and lost PSNR on forest/crowd. This argues against a thresholded per-block policy, but does not test uniform q6-refit endpoints rounded by one bit (`nx-warp/bench/results/90fps-2026-10-04/half-payload/quality-improve/README.md`).
- Finer spatial modes are a different axis and already closed: Q2 selector and balanced Q4 selector failed the production-paired gate; the continuous queue says hold further weight-mode probes. Do not repeat them (`nx-warp/bench/results/90fps-2026-10-05/continuous/QUEUE.md`, sections “Completed bounded tasks” and “Do not repeat”).
- Motion packing is also different: it losslessly predicts between ASTC frames and leaves ASTC decode output unchanged. Its moving 3D clip passed admission on only 9/35 one-frame pairs (4.34% aggregate payload saving, 1.55% with cooldown), none at reference gaps ≥2; it does not test endpoint/color fidelity (`nx-warp/bench/results/90fps-2026-10-04/motion-packing/README.md`).

## Smallest useful next gate

If a compression gate is still wanted, make one CPU/reference experiment, no shader mode search: take the provenance-matched native q6 RGBA8 inputs and exact q6 ASTC payloads used by `astc-production-selector-gate/manifest.txt`; parse each block; round only its already-selected q6 endpoint values to the q5 step-two lattice; repack while bit-for-bit retaining mode and all weight symbols; reference-decode; compare full-frame RGB MSE/PSNR and reused-CCtx Zstd-3 bytes against the exact q6 block payload. Include q6 baseline and q5 controller-rung results if available, with source/payload hashes and changed-block/dual-plane coverage. Do not publish photos, crops, payloads, or pixel dumps.

Predeclare a stop gate: continue only if the same-size/mode candidate saves at least 5% Zstd bytes on both eyes while losing no more than 0.1 dB PSNR on either fixture; otherwise close the endpoint-precision line. Root tightened the proposed 1% threshold to 5% before dispatch, consistent with the user rejecting marginal 2% gains. This is a predeclared decision rule, not a measured result. Passing it only earns a later motion-phase stability gate; static photos do not prove temporal stability or live quality. Since the proposed repack leaves ASTC mode and weights fixed, it also makes no ASTC decoder-work claim.

A higher-impact queued measurement is actual ASTC worker queue dwell/drop rate, not another quality microprobe: the default-off diagnostic already exists at `client/decoder/astc/decoder.cpp:90-92, 230-235, 309-315, 535-548` and is documented in `docs/ASTC_QUEUE_TIMING.md`. One controlled 180-frame window can show whether work waits in the queue before decode; no policy change follows without that evidence. The continuous queue specifically asks for this diagnostic before changing delivery policy.

## Root dispatch scope

The bounded follow-up tests ordinary mode0x0F3/CEM8/QUANT64 blocks only. Dual-plane/CEM9 and other blocks stay byte-identical, with coverage counted. Endpoint orientation changes are guarded/reported. Full-frame reference decode and compressed size determine the result; no shader, decoder, live profile or runtime setting changes. Source 831aafed remains unchanged.
