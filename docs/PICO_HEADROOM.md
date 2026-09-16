# Pico headroom: remove speculative object motion

Status: **offline preparation, not a measured Pico improvement** (2026-09-16).

The wearer reports distracting object-warp artifacts and insufficient frame rate. The next baseline uses the existing hardware-decoded HEVC image and the headset runtime's head-pose reprojection. Dense object-motion estimation is opt-in research again. No replacement optical-flow algorithm is added.

## What the full profile removes

- Server motion pyramids, estimation, readback and vector transport: `motion_mode=off` exits before estimator creation. The forced-field environment variable cannot override off.
- Client motion-field selection, temporal vector filtering and object displacement. The regular platform head-pose reprojection remains enabled. This does not predict independently moving objects or eliminate video/network delay.
- Optional image blending, Kuwahara, FSR, sharpening and ambient glow. Plain sampling and existing defoveation remain.
- The experimental fourth retained decoded image. The normal three-image history remains; this is not a claim that the decoder allocates exactly three surfaces.

Unchanged-image reuse is explicitly enabled. It is conditional on the existing cache validity checks, not a promise that every repeated refresh skips rendering. Stream geometry, foveation, centre size, bitrate and bit depth are preserved. The 8px code and matched wire protocol are retained for reversible comparisons.

## Two explicit cadence choices

| Profile | Requested fresh images | Requested headset refresh | Tradeoff |
|---|---:|---:|---|
| Clean native | 90/s | 90 Hz | Full-rate target without object warp; decode load can exceed the old 60/s source |
| Economy | 45/s | 90 Hz | Lower decode/input workload; moving objects update at 45/s, while runtime head reprojection continues |

These are requests, not measured delivered rates. Economy uses the existing `fps_divider=2`, avoiding the alternating one/two-refresh cadence of an ideal 60-to-90 stream. Real arrival jitter and runtime behavior still require testing. It halves the requested source rate relative to native 90, not total GPU time or latency. No automatic quality reduction or hidden resolution change is included.

## Prepare or apply

Run `python3 tools/pico_headroom.py` to inspect the full plan without contacting a device.

**Existing release APK:** use `python3 tools/pico_headroom.py --apply --properties-only` when the Pico returns. This backs up and overrides Android debug properties, stops the client, disables object-motion mode/EMA/blur/past-frame preference/four-source retention, and enables unchanged-image reuse. It does not access private settings. It preserves the saved source divider, refresh rate and image filters; therefore it does **not** claim to apply either full cadence preset. The release build is not debuggable, so this is the deployable first step without another APK.

**Debuggable development APK only:** `--apply` also backs up and patches private `client.json`; add `--economy` for the lower-workload preset. The helper stops on denied `run-as` access before changing device state. The full patch disables the optional image filters listed above and requests the chosen cadence. `--economy --properties-only` is rejected rather than silently ignored.

Use `--restore BACKUP` to restore the saved configuration (when present) and properties on the same device. Backups have unique local directories. Restart the client after apply/restore; properties are temporary across headset reboots. Neither mode installs a client, starts a server, or launches a game. A matching existing client/server pair is required.

The old `pico-motion8/install_and_start.py` explicitly reinstates dense warp and the 60 Hz source cap. **Do not use that launcher for this comparison.** Restart the matching server with `WIVRN_NX_SOURCE_FPS`, `WIVRN_NX_ALWAYS_MOTION_FIELD` and `WIVRN_NX_MOTION_BLOCK_PX` removed from its environment, using the existing HEVC configuration (equivalent to `env -u WIVRN_NX_SOURCE_FPS -u WIVRN_NX_ALWAYS_MOTION_FIELD -u WIVRN_NX_MOTION_BLOCK_PX /path/to/wivrn-server -f /path/to/profile.json`). The client's saved/requested cadence then controls the server. Properties-only mode does not change that cadence. Preserve the existing running-session ownership checks when restarting.

## Evidence and limitations

Source audit: WiVRn `163d0b84`, `configuration::motion_mode`, compositor motion preparation/estimation gates, stream field selection and unchanged-image cache. The main code path already supports this; a new shader or protocol revision is unnecessary.

The earlier [8px transport exercise](../bench/results/90fps-2026-09-12/live-motion8/README.md) produced 126,758 wire-budget bytes for one stereo field. Repeating that example at 60 Hz would consume about 60.8 Mbit/s in motion data alone. This is an example-derived calculation, not measured live traffic. Turning estimation off removes this auxiliary stream entirely on a compliant server. The HEVC video stream remains.

No new headset FPS, latency, temperature or power measurement is available. The installed release APK compatibility was checked from its merged manifest and build configuration; actual property application remains untested while disconnected. Host/mock validation proves profile handling only. In particular, clean native 90 might still exceed the decoder budget; economy is the explicit fallback, not fake 90 FPS content.

## Short next-device comparison

1. Save original settings; run economy first with a moving application, then clean native with identical geometry and content.
2. Check requested refresh, actual eye dimensions, decoder count, fresh frame rate and frame-time p50/p95/p99 separately from panel refresh.
3. Confirm no new motion-field traffic, no active estimator, motion off in render telemetry, and unchanged-image cache hits when eligible.
4. Compare visible object stability during head movement; record thermals and clocks. Keep the faster stable profile; do not promote by decoder-only FPS.

## Offline checks

`python3 -m unittest discover -s tools -p test_pico_headroom.py -v` passes seven tests. They cover a no-device dry run, lossless backup/restore and unknown-key preservation, denied private access before mutation, release-compatible property-only apply/restore, rejection of incompatible options and malformed backups, and shell quoting/stdin preservation. These mocked checks do not establish Android execution or rendering speed.
