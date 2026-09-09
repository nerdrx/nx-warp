# Alternating eye detail: integration boundary

This experiment models image quality and schedule behavior on the CPU. It is
not enabled in WiVRn NX and does not omit any production decoder work.

## The actual live geometry matters

The current native eye is 2176×2176. Compact output is 928×928, retaining the
512×512 native centre and every fourth sample on each outside axis. A guide
at half *native* width/height alone contains 1,183,744 luma samples, 37.5% more
than the entire current packed eye (861,184). Thus "half resolution" must name
its reference resolution; it is not automatically a cheaper live decode.

`packing_budget.py` counts a hypothetical cheap eye with every other current
packed peripheral sample in each dimension, keeping its native centre:
411,904 samples versus 861,184. Alternating full/cheap eyes averages 636,544
samples per eye, a 26.1% reduction in decoder output samples. This is not a
26.1% GPU saving. Entropy, centre transforms, tile dispatches, final display
writes, history reads, motion estimation and warping still cost work.

## Required implementation

1. Keep the stereo packet geometry stable. Signal each eye's detail generation,
   full/guide coverage and source pose; derive cadence from source sequence,
   not packet arrival count. A lost detail frame must not flip eye phases.
2. Emit the cheaper representation at the encoder. On a full-detail refresh,
   derive any needed guide from that detail rather than transmitting it twice.
   On a cheap refresh, retain fresh native centre data and explicit guide data.
3. Dispatch only required independent decode units and retained guide samples.
   The current flat PLANAR kernel already avoids entropy/IDCT for its peripheral
   units. A post-decode resize cannot save these already-absent operations.
4. Keep per-eye presentation history, separate from codec references. Preserve
   the image and its source pose until the sampling fence completes. The current
   smoothing predecessor is deliberately not pinned between refreshes; it is
   insufficient as a guaranteed alternating-eye cache.
5. Reproject retained history into the current view. Existing frame smoothing
   samples the previous image at current-image UV and fades on pose changes;
   it is not motion-correct history reconstruction. Depth/flow is needed for
   reliable translation and object motion; guide mismatch only detects some
   errors. Low confidence, out-of-view pixels and stale history use fresh guide.
6. Refresh both centres every source frame. Reject expired detail and flush
   history on reconnect/geometry changes. Measure binocular disagreement as
   well as per-eye quality; alternating sharpness can look worse despite lower
   aggregate error.

Integration locations: `vk/decoder/passB/planar_flat_main.glsl` (compact stores),
WiVRn `client/shaders/reprojection.glsl` (`compact_map_uv`, `sample_prev`),
`client/scenes/stream.cpp` (previous frame lifetime and smoothing), and
`client/decoder/nxwarp/nxwarp_decoder.cpp` (borrowed output and decoding).

Before deployment: exercise pose rotation/translation, independent moving
objects, disocclusions, loss and reconnect. Benchmark the complete guide +
history + repair pipeline on Pico against the current wider-ring profile.
Synthetic horizontal motion and sample accounting cannot establish latency,
GPU savings or binocular comfort.
