# Matched patterned live runs

These runs used the same APK (`39ee6502c84c6ca1f706d9733da2f277a58c12b8c642039707bb74dbe52cc70f`), server base `df83946b`, and a static checkerboard with animated cube edges. This is not the moving-checkerboard fixture. A wake key was attempted every four seconds, but report gaps persisted (up to 4.648 s off and 6.537 s auto).

Active-window median new-source rates were 40.25/s (off) and 45.0/s (auto). Decoder GPU medians were 13.5 and 17.9 ms. Boundary-excluded observed counts across report spans were 34.05/s (off) and 35.25/s (auto). These gaps and uncontrolled session intervals prevent a causal speed claim. All server reports show ATLAS frames with 578 intra tiles, zero skipped and zero PICTURE frames.

Screenshots are retained unchanged: [off](off-screen.png) and [auto](auto-screen.png). Recompute with `python3 summarize_patterned.py`; it imports the parent `read_pair` parser and adds server tile-mode telemetry.
