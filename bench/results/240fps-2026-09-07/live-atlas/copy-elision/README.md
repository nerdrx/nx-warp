# Live atlas copy-elision arm

This is a separate empirical arm using APK SHA-256
`39ee6502c84c6ca1f706d9733da2f277a58c12b8c642039707bb74dbe52cc70f`.
The decoder copy term fell from approximately **0.56 ms to 0.29 ms** in the
compared logs. Source cadence was **51.0/s**, versus **52.5/s** in the previous
atlas-auto run, so the copy reduction produced no net source-rate gain.

The retained logs are filtered to WiVRn client lines and nxwarp server lines;
the startup extract records the atlas path. `summarize.py` uses only the Python
standard library and reproduces `summary-recomputed.json`. Its rates are
medians of logged active windows, not continuous wall-clock cadence or physical
display FPS. The screenshot is scene evidence only and is not an image-quality
claim. This arm does not alter the parent live-atlas comparison files.
