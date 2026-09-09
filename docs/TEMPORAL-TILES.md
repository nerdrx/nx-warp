# Temporal tile updates: adaptive encoder prototype

**Opt-in encoder fit caching implemented; decoder skipping and pose-aware history remain proposed.** The target is lower latency
for fresh centre pixels by doing less peripheral work, rather than presenting
old centre frames more frequently.

At a 90 Hz display rate:

| Region | Update cadence | Phase policy |
|---|---:|---|
| Native centre | Every frame, up to 90 Hz | Always due |
| Middle ring | Every second frame, up to 45 Hz | Alternate neighbouring tiles |
| Outer ring | Every fourth frame, up to 22.5 Hz | Spread tiles across four phases |

These are fractions of admitted source frames. If source throughput is below
90 Hz, the absolute correction rates are lower. A future decoder scheduler should give the ring boundaries a
transition band of mixed tile classes; do not switch an entire ring at once.
Use a stable phase derived from tile coordinates, shared between corresponding
eye regions. Centre updates take priority; stale or invalid tiles may override
the ordinary schedule.

## Current prototype

`NXVC_PLANAR_CADENCE=1` enables caching only for independent graduated GPU
PLANAR-centre encoding. The centre remains fresh; fine peripheral tiles use
period 2 and other peripheral tiles period 4, counted in admitted source frames.
A 48-sample Y/Cb/Cr fingerprint detects change against the last fitted tile.
Mean absolute difference >=12 marks a tile hot; <=6 for three checks clears it.
Hot tiles may refresh on the doubled cadence, subject to rotating half-tile
eligibility. This bounds discretionary promotion; it is not a measured GPU
budget or a guarantee that every changed tile immediately doubles its rate.

First use, changed QP/flags, and age >=4 force a new fit. Every transmitted frame
still contains independently decodable bodies, so dropping an entire frame does
not desynchronize decoder references. Corresponding eyes share cadence phases,
but content-driven promotion decisions remain per-eye. No tiny blur is enabled.

This caches screen-space samples without historical tile poses. It is therefore
an **offline approximation, disabled in the live profile**, not the required
motion-correct decoder cache below. Sparse fingerprints can miss small changes.
[Checks, timings and decoded images](../bench/results/90fps-2026-09-09/adaptive-planar/README.md)
show preserved centre pixels, Pico/CPU decode equality and reduced fit counts.
The larger measured saving came from independently bypassing unused PLANAR
transforms; fit caching alone did not establish a useful latency improvement.

## Required cache contract

A skipped update means retain a valid previous tile, not leave an uninitialized
destination. Track each retained tile's generation, age and source pose. Reproject
from the correct source pose; static image-space reuse can trail badly during
head motion. Missing cache entries, loss, reference invalidation and disocclusion
must force a repair. Put a bound on tile age so peripheral content cannot freeze.

The current GPU PLANAR-centre path is independent-frame coding. It bypasses
Pass W and reference-ring reconstruction. Merely forcing WARP_SKIP in the mode
selection is unsafe: that syntax expects a valid predictor and synchronized
references. Re-enabling the generic predictor could cost more than it saves.
Evaluate an explicit independent-tile output cache against the existing atlas
path before choosing the representation. No unsignalled decoder omissions.

## CPU quality model; proposed low-resolution guide with retained detail

A low-resolution current image could guide a separately retained full-resolution
history. The CPU quality model for this design is implemented in the
[guide-history fixture](../bench/results/90fps-2026-09-09/guide-history/README.md),
using a synthetic grayscale translation sequence. It measures guide/history
quality, repair behavior and motion ambiguity; it is not a live or Pico output.
GPU and codec integration, including decoder skipping and pose-aware history,
remain proposed. The guide is not simply upscaled as the final image.

1. Keep the native centre current in both eyes.
2. Update a small stereo guide every admitted frame; refresh peripheral detail
   less often and spread those repairs spatially.
3. Reproject retained detail using its historical pose and available motion.
   Compare the new guide with the downsampled prediction to identify changes.
4. Reject bad history at disocclusions and changed objects. Use the fresh guide
   as a temporary fallback and request a detail repair under the work budget.

Alternating which eye receives a full-detail update now has a
[CPU quality comparison](../bench/results/90fps-2026-09-09/alternating-eye/README.md).
It spreads detail work across frames but increases the stereo residual proxy
relative to synchronized controls at the same budget. Centres, balanced phases,
lost-detail recovery and history reset are checked; GPU and live integration
remain open. This is not an enabled alternate-resolution decoder.
It needs per-eye history age/pose tracking even if the wire picture remains a
single stereo frame; corresponding features can otherwise differ between eyes
during motion. Start with coordinated stereo peripheral repairs, then compare
alternating-eye cadence against that control. A single pose describes neither
mixed-age eye nor mixed-age tiles unless every retained sample is first moved
into a common current-pose image. Rotation can use pose alone; robust translation
and newly visible surfaces need more scene information or fresh pixels.

The current independent compact path has no depth image in `view_info_t` and no
partial decoder submission API. Its PLANAR periphery already skips entropy and
transforms; shrinking that output saves only remaining sample/storage work.
The native INTRA centre still incurs entropy and transform costs. To remove
those costs, the encoder must emit a cheaper guide/detail representation or the
decoder must explicitly support skipping unneeded independent units. Merely
resizing a fully decoded image does not establish a decode-time saving.

Keep presentation history separate from codec references. Do not re-enable
WARP_SKIP against a modified reconstruction. Test fast rotation, translation,
moving objects, stereo disagreement, disocclusions and packet loss; count guide,
warp, repair and history-memory costs together. A quality-only presentation
prototype can validate appearance, but cannot prove decoder savings.

## Tiny peripheral blend, tested separately

Keep the centre unblended. After the cadence-only path works, try a short blend
of reprojected history into newly corrected peripheral tiles, increasing gently
outward. A blend without motion alignment is temporal ghosting, not an accurate
motion blur. Reject history across disocclusions and large colour/depth changes;
never let smoothing delay the arrival of a new centre correction.

## Acceptance evidence

Measure encode and decode GPU time, bytes, centre correction age, tile-age
histograms, queue waits, full-scene motion and tail latency. Preserve a current
full-rate control. Demonstrate cache validity through loss and reconnect before
live use. A lighter frame is useful only if the scheduling/cache overhead and
visible stepping justify the saving. Physical latency still needs an independent
measurement; a smaller predicted-display offset is insufficient.


## Synchronized reconstruction on GPU

A [standalone Pico Vulkan probe](../bench/results/90fps-2026-09-09/synchronized-gpu/README.md)
now validates synchronized full/guide reconstruction at the packed stereo
geometry. Fourteen cases match an independent NumPy oracle, including native
coordinate mapping across sampling boundaries and fresh-centre/guide fallback.
The guide/history pass costs 1.67–1.77 ms on isolated Pico runs; a shared-decision
variant was slower and rejected. This adds reconstruction work and does not
implement decoder skipping, pose-aware presentation or persistent history.
The next integration question is how to fuse history sampling into presentation
while omitting detail decode work. Static resident-input timings are not a
live-motion, latency or sustained-throughput result.
