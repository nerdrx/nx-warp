# Temporal tile updates: proposed next experiment

**Design proposal, not implemented or benchmarked.** The target is lower latency
for fresh centre pixels by doing less peripheral work, rather than presenting
old centre frames more frequently.

At a 90 Hz display rate:

| Region | Update cadence | Phase policy |
|---|---:|---|
| Native centre | Every frame, up to 90 Hz | Always due |
| Middle ring | Every second frame, up to 45 Hz | Alternate neighbouring tiles |
| Outer ring | Every fourth frame, up to 22.5 Hz | Spread tiles across four phases |

These are fractions of admitted source frames. If source throughput is below
90 Hz, the absolute correction rates are lower. Give the ring boundaries a
transition band of mixed tile classes; do not switch an entire ring at once.
Use a stable phase derived from tile coordinates, shared between corresponding
eye regions. Centre updates take priority; stale or invalid tiles may override
the ordinary schedule.

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
