# Sequential decoder plus atlas-render throughput

This is a cold-to-steady offscreen Pico 4 measurement of the native R8 atlas
path. The input is a 1200-frame, 4352x2176 stereo `yuv420p` stream (2176x2176
per eye, with 2160x2160 visible output). Each frame was decoded, submitted to
the native renderer, and waited to a GPU fence before the next frame. The
capture used an Adreno 650 and did not involve OpenXR, transport, encoding, or
headset presentation.

The first 120 frames are warmup, but were still decoded and rendered. Across
all 1200 pairs, wall throughput was 131.122/s. The 1080 steady rows reached
143.042/s; their logged p50/p95/p99 total pair times were 6.993/8.600/9.370
ms, with zero pairs at or below the exact 240-Hz budget of 4.166667 ms. These
numbers describe completed offscreen work, not physical display FPS or a
240-Hz result.

| run | frames | mode | throughput |
|---|---:|---|---:|
| combined | 1200 (1080 steady) | decode + atlas draw + fence | 131.122/s overall; 143.042/s steady |
| decoder dirty | 1200 | decode only, no output | 271.101/s |
| decoder full | 1200 | decode only, no output | 129.347/s |

The decoder-only rows are stage comparisons, not complete-pipeline estimates.
The combined measurement is sequential and therefore does not establish
pipeline overlap or sustained presentation cadence. The retained Pico logs
show 72-Hz and 90-Hz runtime modes; no retained evidence shows a 240-Hz
presentation mode. Offscreen throughput on other hardware remains an open
measurement.

Reproduction uses the sequence harness described in
[`probe/sequence/README.md`](../../../../probe/sequence/README.md). The copied
`sequence-pico-baseline.log` is the authoritative aggregate report;
`timings.csv` contains per-frame decode, render, and total rows, including the
120 warmup rows. `left-native.png` is the lossless 2160x2160 left-eye
readback; `left-resized.png` is a smaller preview. They are sanity images, not
quality or motion proofs. The fixture metadata records the generator and
encoded-stream SHA; raw YUV was streamed through a FIFO and is not retained.

## Controlled output-format experiment

On the same Android binary, retaining synchronous completion of every render:

| Mode | Steady completed pairs/s | p99 pair ms | Pairs within 4.166667 ms |
|---|---:|---:|---:|
| UNORM, first | 177.824 | 7.704 | 159/1080 |
| UNORM, repeat | 177.235 | 7.754 | 143/1080 |
| SRGB restored | 148.690 | 8.982 | 0/1080 |

All three use async decode submission on the same graphics queue, with an explicit
compute-to-sampling dependency and a completed render fence before reuse. UNORM
removes the fragment shader's sRGB-to-linear conversion together with the output
attachment's inverse conversion. Scale is one and bias zero. Compared with the
original SRGB left-eye readback, the final UNORM image differs by at most one
8-bit value per channel. This grayscale sparse-motion fixture does not establish
color accuracy on a broad scene suite or the OpenXR compositor's color handling.
This is an offscreen probe option, not an enabled WiVRn production feature.

![Actual UNORM left-eye readback](unorm-left-native.png)

`unorm-timings.csv` contains the first UNORM run. `unorm-binary-sha256.json`
identifies the executable and shaders used for these three runs. Host Vulkan
synchronization validation passed for 16 complete pairs; that short validation
run is not a performance result.

Other short experiments did not establish a useful gain: async submission alone
144.958/s; static FDM with peripheral byte value 128, 145.596/s; corrected value
127, 153.293/s; no GPU timestamps, 157.105/s; FDM127 plus no timestamps,
152.003/s; UNORM plus both, 176.765/s. The control repeat was 144.343/s.
FDM keeps the central circle at full density and requests lower peripheral
shading density; actual fragment sizes are implementation-defined. These
exploratory single runs are retained as negative/inconclusive evidence, not
additive speedups. No variant met the 240-Hz deadline throughout the capture.

## Cadence-4 follow-up

A follow-up capture rendered each decoded frame four times at a static pose:
4800 renders from 1200 decoded frames. It reports 304.453 render iterations/s,
but only 76.113 fresh corrections/s; render p99 was 6.651 ms and 2968/4320
steady render iterations met 4.166667 ms. This is repeated static-pose work,
not 240 fresh frames/s or proof of a 240-Hz deadline. The retained
[`sequence-pico-cadence4.log`](sequence-pico-cadence4.log) and
[`cadence4-timings.csv`](cadence4-timings.csv) contain the aggregate and
per-render rows; the plot keeps render and fresh-correction rates distinct.

![Pico offscreen sequence rates](cadence-comparison.png)

## Sustained multi-rate capacity check

The 6,000-frame fixture produced **24,000 completed renders over 81.526495 s**.
After 120 decoded warmup frames, 23,520 renders completed over 78.951640 s:
**297.904 renders/s and 74.476 fresh corrections/s**. Render p50/p95/p99 were
2.943/6.216/7.289 ms; **15,907/23,520 (67.63%)** met 4.166667 ms.
Thus repeated-render capacity exceeded 240/s for more than a minute, while
240-Hz deadline consistency still failed. This is the same static-pose sparse
fixture experiment, not 240 independent frames, a paced presentation test,
a thermal qualification, or motion-to-photon latency proof.

The long CSV includes every render. The manifest records fixture and binary
hashes plus opt-ins; both final eye readbacks are retained as PNG files.

## Additional controls

Pinning only the benchmark process to the fastest CPU core did not help:
cadence-4 throughput was 285.759 renders/s, render p99 7.109 ms, versus the
unpinned short run's 304.453/s and 6.651 ms. Battery saver was already off.
The affinity experiment ended with its process and is not a recommended setting.

An optional cubic inverse-gamma approximation (`NX_SEQUENCE_FAST_SRGB=1`)
retaining SRGB output reached 177.576 fresh pairs/s, p99 7.230 ms, with
108/1080 steady renders within budget. It avoids the power function using
`x*(0.0125218351+x*(0.682174119+0.305304046*x))`. Least-squares fitting
constrains endpoints to zero and one; 65,537 uniformly sampled grayscale
values yield maximum encoded round-trip error 4.979/255 and mean 0.658/255.
This is an approximation with no spatial blur, not equivalent color decoding.
It remains a benchmark-only experiment: the measured speed was comparable
to the exact UNORM-output alternative, and no production color approximation
was enabled. Host synchronization validation passed.

## Spin-wait experiment (negative result)

Three same-binary cadence-4 runs compare `spin_us=0` control, `spin_us=4000`,
and restored `spin_us=0`. All decode 1200 frames and render each four times
(4800 render iterations; 1080 steady decode rows). Spin improved repeated-render
rate from 290.233/s to 324.370/s and fresh-correction rate from 72.558/s to
81.093/s, but render p99 moved from 7.280 to 7.520 ms and total steady pair
p99 from 17.056 to 16.195 ms; the restored run returned 300.456/75.114/s,
7.237 ms, and 17.052 ms. No total pair met 4.166667 ms. CPU process time rose
from 1.805 s (control) to 12.767 s (spin), about 7.1×, against 17.457 and
15.332 s wall time. Individual render-iteration deadline fractions were 2919/4320 (67.57%),
3303/4320 (76.46%), and 2950/4320 (68.29%) for control/spin/restored. This is therefore off by default and has no production
recommendation. The spin rate is repeated static-pose work, not fresh 240-Hz
corrections or presentation FPS.

![Spin-wait comparison](spin-comparison.png)

Recreate the chart with `python3 plot-spin.py`; it parses the copied aggregate
logs and shows render throughput, p99, and CPU cost.

The copied logs and compressed per-render CSVs are the raw records. Binary
SHA-256: host `cd43fefec86a970a871168ee6ce9a24176b19a84331d775db2a4e0a4be49ceb5`;
Android `ccd2315856264362bb631182a969efdcaa02beccd0a9bc47ce69b9a403d6c4e9`.

## GPU timestamp diagnostic

Three same-binary async+UNORM+dirty-catchup arms used 4 repeats and 120 warmup
source frames: GPU timestamps were `0/1/0` for control/GPU/restored, with
`spin_us=0`. The GPU-enabled arm reports pooled GPU p50/p95/p99 of
1.694/3.060/3.986 ms; its corresponding wall `total_ms` percentiles are
2.944/6.161/7.047 ms. GPU TOP→BOTTOM intervals may include dependency stalls and
are not isolated shader time or whole decoder GPU time. The CSV timing query is
outside each render timer but included in source-cycle wall throughput.

Wall `total_ms` for `render_index=0` includes decode; indices 1–3 repeat the same pose. Host
synchronization validation passed for 64 renders, apart from a deprecated
settings warning. Reproduce the steady percentile extraction with
`python3 summarize-gpu.py`; it excludes the first 120 source frames and uses
the harness rule `ceil(p*n)-1`. This diagnostic establishes no end-to-end or
240-Hz result.

| Timestamp arm | Steady renders/s | Fresh corrections/s | Wall p99 ms |
|---|---:|---:|---:|
| Off control | 298.548 | 74.637 | 7.058 |
| On | 300.188 | 75.047 | 7.047 |
| Off restored | 299.410 | 74.853 | 7.504 |

All arms processed 1,200 source frames. Android executable SHA-256:
`1facc3ec25545493a8b76092ed1ea9240c6d03237222e8579e7331c4eb73b45c`.
