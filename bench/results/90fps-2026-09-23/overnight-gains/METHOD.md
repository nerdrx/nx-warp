# Method and evidence boundaries

These experiments target the custom WiVRn NX direct Vulkan path on the connected
Pico (reported model A8110), at 100% stream scale and 90 Hz. They do not establish
240 FPS or physical motion-to-photon latency.

## Workload

Two private supplied photographs were prepared as 2160×2160 RGBA fixtures and
duplicated for the two eyes. The live headless OpenXR fixture uploads them before
measurement. Ordinary repeats alternate a horizontal −8/+8 pixel shift every
four stereo frames. The changing-picture tests alternate the two entire images
every stereo frame. This stresses changing content but is not a real game's
stereo rendering, disocclusion, camera motion, or a wearer comfort assessment.
Private source pictures, device identifiers, addresses, and raw device logs are
not published. Public graphs contain numeric data only.

## Common settings

| Setting | Value unless an individual report overrides it |
|---|---|
| Requested quality budget | 500 Mbit/s |
| Native centre / precision | Enabled / RGB888 |
| Detail lossless compression | Zstd selection, with existing LZ4/raw fallback |
| Compression credit | Disabled |
| Motion field | Disabled |
| Packet window / UDP tail | 0 / 64 packets |
| Synthetic loss injection | Disabled |
| Selection | Newest complete stereo image |

The compression candidate uses `NX_DIRECT_COMPRESSION_CACHE=1` and
`NX_DIRECT_PREDICTOR=1`; paired baselines disable the settings named in each
report. The timing candidate uses `debug.wivrn.nx.jit_max_sleep_us=5000` and
`debug.wivrn.nx.ready_wait_us=4000`. Controller tests force the named mode through
the nonpersistent `debug.wivrn.test.bitrate_mode` override. The loss-only AIMD
candidate additionally uses `WIVRN_BITRATE_AIMD_LOSS_ONLY=1` on the server.
Neither the timing candidate nor the controller diagnostic is a global default.

The 500 Mbit/s request, direct planning budget (about 433.604 Mbit/s at this
configuration), compressed codec payload, and total network traffic are four
different quantities. Payload includes detail plus the independent safety image,
but excludes transport/FEC and tail padding. Lower payload is not a saving at
unchanged detail if the controller also lowered the planning budget.

## Counting

Client windows logged at least ten seconds after source upload are retained when
the printed duration is two seconds. The first five server telemetry windows are
discarded; later server windows shorter than 1.9 seconds are excluded. Incomplete
runs and identified competing GPU/CPU experiments are not combined with valid
paired captures. Network dips within valid captures remain included.

Fresh-source selection rate is `new_source_count / render_iterations × reported
render_rate`. Dividing by the rounded printed duration alone slightly overstates
it; the overnight reports use the corrected calculation. This measures changing
source identifiers selected by the client, not unique image content, actual panel
scanout, or a guarantee that every selection was physically displayed.

The derived software delay sums wire, queue, decode, decode-to-selection, and
selection-to-predicted-display telemetry. It excludes the pose-derived source
field and is not measured photon latency. Reported p95 values are percentiles of
roughly two-second telemetry-window means, not per-frame tail latency. Encoder
FPS and payload estimates also inherit the rounded server interval duration.

## Reproduction and code state

Numeric chart scripts read only their bundled CSV/JSON. The image codec fixture
can be rebuilt using the integration repository's
[`tests/build_direct_photo_fixture.py`](https://github.com/nerdrx/wivrn-nx/blob/atlas-live/tests/build_direct_photo_fixture.py).
Exact-byte checks compare the encoded representation restored by decompression,
not the original photograph before lossy representation/foveation.

Matched server builds also contained pre-existing, unrelated NXFuse working-tree
changes, held constant across each comparison and left untouched by this task.
Those changes are not part of the compression commits. The final build manifest
records binary hashes; a published commit alone should not be treated as a
byte-for-byte identity claim for the tested executable.
