# Reproduce the centre/periphery study

This is an offline representation experiment, not a WiVRn feature. It reuses the
production NXDF tile layout and independent compression selector for the centre,
plus libjpeg-turbo through Pillow for a reduced-resolution outer image. There
is no server, GPU workload, headset connection or desktop window in these runs.

Dependencies: C++20, WiVRn NX headers, LZ4, Zstd, TurboJPEG 3, Python, Pillow,
NumPy and Matplotlib. Paths below are caller-supplied. Keep generated image
files, source RGBA, NXDF and NXJP containers in a private directory.

1. Obtain the six NXDF fixtures listed in the preceding `mjpeg-vs-nxvc` report.
   Its `harness/export_nxdf.cpp` produces the full reconstructed RGB PPMs.
2. Build `nx_centre_bench.cpp` with `-O3 -DNDEBUG -std=c++20 -Wall -Wextra
   -Werror`, include WiVRn's `common` and `server/encoder`, and link `-llz4
   -lzstd`. Run with `CORPUS CENTRE_OUTPUT SUMMARY_JSON`. Export its rebuilt
   `.nxdf` files through the same PPM exporter.
3. Supply the original 2160×2160 RGBA files matching the hashes in
   `../fixtures.json`. The comparison intentionally refuses unmatched inputs.
4. Run:

```sh
python3 compare.py --forest "$FOREST_RGBA" --dark "$DARK_RGBA" \
  --nx-decoded "$NX_PPM" --centre-envelopes "$CENTRE_OUTPUT" \
  --centre-decoded "$CENTRE_PPM" --nx-summary "$NX_SUMMARY" \
  --private "$PRIVATE" --results "$RESULTS"
python3 "$RESULTS/plot.py"
```

`NX_SUMMARY` is the earlier independent-frame baseline's `nx-summary.json`.
The script writes 144 quality rows: six inputs × three JPEG dimensions × four
qualities × two chroma modes. It checks centre preservation in both eyes,
validates serialized component lengths/bytes, and rejects truncated and trailing
packet data. Selected private `.nxjp` examples include the full NX envelope and
JPEG payload used by the measurement; they are not a supported network format.

The 36-byte study header is little-endian `<4s8I>`: `NXJP`, version 1, stereo
width, height, exact-NX radius, blend-end radius, NX byte count, JPEG byte count,
reserved zero. The payloads follow in that order. Geometry and limits are fixed
for this experiment; `packet_parts()` is not an untrusted-network JPEG parser.
NX envelope validation/decoding uses the production C++ helper. JPEG decode uses
Pillow/TurboJPEG. Python reconstructs the periphery and composites it with the
independently verified NX centre. This is a reference reconstruction, not a
performance implementation.

For native host timing, reuse `mjpeg-vs-nxvc/harness/jpeg_decode_bench.cpp` and
the private `decode-jobs.json`. Only JPEG decode to preallocated RGB buffers is
timed. Keep centre envelope timings separate; adding independently measured
percentiles does not measure the complete hybrid pipeline.
