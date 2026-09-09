# Four-tap Kuwahara-inspired filter

Four neighbouring bilinear block means are each scored against the shared centre
sample. The closest luminance match wins; its colour is averaged with the centre.
This replaces full quadrant variance with a rough two-value score. It reduces
filtering work and strength, and can retain block boundaries that a larger kernel
would clean up. This is an approximation, not an equivalent Kuwahara kernel.

Same isolated RGBA methodology as [eight-tap](../kuwahara-eight-tap/RESULTS.md):
Pico 4, two 2160×2160 draws, synthetic 2176×2176 source, four alternating-order
comparisons; each uses the minimum of three repetitions of 12 frame pairs after
one untimed warmup. This is GPU draw time, not motion-to-photon latency.

| Kernel | Mean of four run minima |
|---|---:|
| Eight extra taps | 7.029 ms |
| Four extra taps, shared centre | 5.486 ms |

**22.0% lower probe draw time.** Default remains off.
Use `debug.wivrn.nx.kuwahara_fast=2` when low-poly filtering is enabled and its
full-kernel option is disabled. Mode 1 selects eight taps; mode 0 the original
sixteen. Spatial only: no extra frame, history buffer, or separate pass.

Raw [pico.log](pico.log), [summary.json](summary.json); probe source included.
Build/run instructions match the eight-tap experiment.

![CPU reference of the three kernels on a prior Pico capture](comparison.png)

The CPU montage uses the prior `compact-direct/native-final-eye0.png` capture
at native resolution before display downscaling. It approximates filtering an
already presented RGB image, not live NV12 sampling, and covers only one scene.
