# Pico directional reconstruction isolation

Generated 194x130 mono YUV420, four ATLAS frames, QP28, reference encoder.
`small-host.nxv` uses default directional intra prediction; `small-nodir.nxv`
differs by setting `cfg.intra_dir=0`. These are diagnostic fixtures, not a
performance corpus. Original decoder binary reproduces the first failure and
completes the no-direction stream. A completed decode alone does not establish
pixel correctness. The retained dirty/full R8/R16 view comparison passes
after every frame on Pico for the no-direction clipped mono fixture and the
original stereo fixture (see the parent `pico-final-view.log`).

| Probe | Observation |
|---|---|
| Original, directional enabled | Frame 0 timestamps unavailable/zero; frame 1 invalid atlas reference |
| Omit reset-frame compose | Same failure |
| Omit reset-frame entropy dispatches | Same failure |
| Omit reset-frame reconstruction dispatches | Completes, timestamps become available |
| Encode without directional prediction | Original decoder completes all four frames |
| Move predictor reference arrays out of function arguments | Same failure; discarded |
| Fold full-schedule predicate in reference gathering | Same failure; discarded |

The reconstruction omission intentionally produces wrong pixels and is an
isolation probe only. All temporary decoder bypasses and the unsuccessful
shader rewrite were removed. Logs with zero timestamps must not be interpreted
as zero-cost decode. The failure boundary is directional reconstruction;
its exact compiler/runtime cause remains unresolved.

The live custom WiVRn NX configuration uses the Vulkan encoder, which already
codes the non-directional path. The direct sampled-atlas consumer in sibling
`nx-scratch/wt-atlas-wiring` is a different wiring path from live
`wivrn-nx-e2e`. Changing the live encoder's `intra-dir` option would not test
this distinction. No live speedup is claimed from this finding. The existing
Pass B documentation's XFORM_LARGE miscompilation note concerns another build
variant and does not by itself explain this default-size fixture.
