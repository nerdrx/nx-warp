# R8 view-only experiment

This failed view-only prototype used APK `1e96ed81b0b75fb53b836986fa1b06532d98d9466f129d2742bd86c5b37e5746`. Its active-window medians were 12.15 ms decoder GPU time and 45 new sources/s, versus 8.8 ms and 45/s in the preceding full repeat. Render-report gaps and off-head session intervals leave causal uncertainty; this does not justify a gain claim.

Host and Pico full-resolution R8/R8G8/R16 conformance logs pass the retained 2176×1088 fixture. The view-only prototype was reverted and is not shipped. The screenshots are capture artifacts, not quality proof. Recompute with `python3 summarize.py`; it imports the parent parser and preserves report timestamps.
