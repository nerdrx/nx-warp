# ASTC upload-fence audit

## Decision

Skip per-image upload command buffers and fences for now. The measured CPU wait
is usually small, while a per-image design adds queueing and resource-lifetime
complexity. Revisit only if a larger measured wait appears in the target path.

## What the trace shows

The current single command buffer and fence wait for the previous upload only
after the next image has decoded and its staging copy has completed. Six image
buffers are in use. A per-image command buffer could remove that CPU wait, but
submitting all work to the same graphics queue can build a backlog. It also
needs reliable lifetime handling for each image.

Renderer-held handles are released after their render fence; unrendered handles may be released earlier. The existing scheme
gates use on upload completion. Submission is protected by the host mutex for
the shared queue. The barrier and teardown handling would still be needed with
per-image submissions.

## Measurements

The capture logs contain 392 same-queue asynchronous and 78 synchronous
180-frame window means for the prior-upload-fence wait:

| Mode | Windows | Median | p95 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Same-queue async | 392 | 0.8 µs | 2.4 µs | 28.3 µs |
| Sync | 78 | 1.0 µs | 1.5 µs | 7.4 µs |

These percentiles and maxima summarize **window means**, not individual fence
waits. The async run has a few larger windows, but its p95 is 2.4 µs. This
evidence does not justify the added queue and lifetime complexity on its own.

`window_means.csv` contains the sanitized aggregate samples; `plot.png` shows
their distribution. `plot.py` regenerates the figure from the CSV. The samples
were reparsed from the client capture logs and matched the aggregate counts and
statistics above.

Source audited: WiVRn NX `221f6834`, `client/decoder/astc/decoder.cpp` (single upload command buffer/fence, allocation gate and upload barriers) and `client/scenes/stream.cpp` (renderer handle lifetime). These CPU waits were sampled after the next decode and staging copy; the result does not characterize GPU upload completion or presentation latency.
