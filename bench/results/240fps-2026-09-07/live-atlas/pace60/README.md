# Exploratory pace comparison

This is an exploratory capture, not a matched optimization benchmark. The
The captures are **pace45** (the earlier `live-r8-full-q40-repeat`) and
**pace60** (`live-r8-full-q40-pace60`); both use atlas auto mode. Each preserves its raw
scene, server, measure, startup, manifest, configuration, and screenshots.

The observed active new-source medians were 45.0/s for pace45 and
36.25/s for pace60. Decoder GPU median was 8.8 ms in both captures.
The report-span source rates were 37.04/s and 31.28/s respectively. These
runs were not matched for thermal state or capture duration, and do not
support a 60 FPS claim. Screenshots are retained for visual inspection only;
no quality conclusion is drawn.

Recompute from the packaged logs with:

```sh
python3 summarize.py \
  --pace45-prefix pace45/live-r8-full-q40-repeat \
  --pace60-prefix pace60/live-r8-full-q40-pace60 \
  --out summary-recomputed.json
```

The baseline APK was the atlas-immediate-ack build with SHA-256
`5a095bbc730a51f52522f4e3f2ae103169ace65f57006f3dc4fa7614bd7721ac`.
The configurations are `pace45/config.json` and `pace60/config.json`.
