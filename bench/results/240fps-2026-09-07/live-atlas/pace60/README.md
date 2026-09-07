# Exploratory pace comparison

This is an exploratory capture, not a matched optimization benchmark. The
semantic arms are **off / pace45** (the earlier `live-r8-full-q40-repeat`)
and **auto / pace60** (`live-r8-full-q40-pace60`). Each preserves its raw
scene, server, measure, startup, manifest, configuration, and screenshots.

The observed active new-source medians were 45.0/s for off / pace45 and
36.25/s for auto / pace60. Decoder GPU median was 8.8 ms in both captures.
The report-span source rates were 37.04/s and 31.28/s respectively. These
runs were not matched for thermal state or capture duration, and do not
support a 60 FPS claim. Screenshots are retained for visual inspection only;
no quality conclusion is drawn.

Recompute from the packaged logs with:

```sh
python3 summarize.py \
  --off-prefix off-pace45/live-r8-full-q40-repeat \
  --auto-prefix auto-pace60/live-r8-full-q40-pace60 \
  --out summary-recomputed.json
```

The baseline APK was the atlas-immediate-ack build (hash prefix
`5a095...` as recorded by the capture provenance). The pace60 configuration
is `auto-pace60/config.json`; pace45 is `off-pace45/config.json`.
