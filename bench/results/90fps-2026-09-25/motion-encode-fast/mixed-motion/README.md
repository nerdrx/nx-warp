# Mixed local-motion compression probe

This offline benchmark tests whether separate small motions in fixed parts of the NXDF native center compress better than one global translation. It uses two photo-derived NXDF source frames but includes no source photos or fixture data. Supply any two compatible private NXDF frames to reproduce; input paths are CLI arguments only.

The controlled sequences are artificial translations of the 256×256 native center. The captured frame periphery stays unchanged. The partition boundaries are fixed and do not follow objects:

| Case | Fixed region shifts |
|---|---|
| `photo-a-x-halves` | left (+8, 0), right (−8, 0) |
| `photo-b-y-halves` | top (0, +8), bottom (0, −8) |
| `photo-a-four-quadrants` | top-left (+8, 0), top-right (−8, 0), bottom-left (0, +8), bottom-right (0, −8) |
| `scene-cut` | photo A → photo B, no artificial warp |

The transform vectors above generate controlled frames in memory. Candidate vectors are estimated from the old/current native pixels with the same bounded SAD search family as the global method (±16 px, coarse 4 px, then 1 px refinement). Local searches use an 8 px sample grid and a 16 px border within each fixed region. This is a known-partition test; it does not estimate object boundaries or variable region layouts.

The baseline is the independent-frame selector in the sibling `motion-compression/native_motion_bench.cpp`: LZ4 versus Zstd level 3 with its stride-4 predictor and existing selection gates. Global and local proposals compress native-center residuals with that same selector. The local prototype envelope costs 36 bytes for two regions and 44 bytes for four: the existing 24-byte header, 4 bytes for mode/count, then 4 bytes per signed 16-bit (x,y) vector. `selected` applies an independent-frame fallback unless a candidate is at least 10% smaller than baseline.

Results are in [results.csv](results.csv). Local regional search recovered each imposed vector exactly. All baseline, global, and local decodes were byte-exact. Local candidates saved 45.5%, 59.7%, and 44.4% in the three warped cases; global candidates saved 20.3%, 19.0%, and 1.6%. In the scene-cut control, global and local candidates were 11.2% and 11.3% larger, so the independent representation was selected.

Host p50 timings in the CSV are CPU-only measurements on a Ryzen 9 9950X3D, after 8 warmups and over 24 samples. They cover vector estimation and native-center residual-plus-compression separately, not the full encoder. Separate p50 sums are estimates, not paired end-to-end timings. No client, Pico, GPU, full-frame correction, or live stream was measured. Device cost is unknown; it would include decompression, byte reconstruction across native tiles, a region-vector lookup per tile, and the extra metadata.

Only 104 native tiles in the captured fixtures moved; the rest of each NXDF stayed fixed. Results are evidence for a controlled compression opportunity, not evidence that arbitrary independent objects can be tracked or that client decoding is cheap. No source images or generated image/frame artifacts are stored here.

Reproduce with `WIVRN_ROOT=/path/to/wivrn ./run.sh PHOTO_A.nxdf PHOTO_B.nxdf [results.csv]`. Inputs must be compatible native-center NXDF frames. Required host dependencies are a C++20 compiler, Zstd, and LZ4. Source hashes: archived global harness `4ff0b0acdfda6f06c8db0ccb4c20c7ee1d71b75b76c413a47d1ab0f2bee410d0`; local harness `cab6e3a5b2801ceecb45c18ddd4394c5a5d43f07c484fd0cc66c73a814146e9c`; results CSV `fbb1f7407f8f410acb7c93a89f46100ef7cc317c05e05c6832253608947d0d3e`. Controlled transforms and local estimator are in `mixed_motion_bench.cpp`.
