# Adaptive-QP pace-45 capture

This ACK-feedback capture used APK `5a095bbc730a51f52522f4e3f2ae103169ace65f57006f3dc4fa7614bd7721ac` with adaptive QP and a configured 45 fps server pace. It is a follow-up to the [ACK-only capture](../ack-only/README.md), not a fixed-QP result.

Active-window medians were 12.8 ms decoder GPU time and 29 new sources/s. Boundary-excluded reported counts were 23.84/s across 43.742 s, with report gaps up to 4.704 s. The capture does not show an improvement; these are reported-window measurements rather than physical display FPS.

The retained screenshots ([awake 1](awake-1.png), [awake 8](awake-8.png)) show the checkerboard/cube scene. Recompute with `python3 summarize.py`; it imports the parent `read_pair` parser. `summary.json` retains the original source hashes.
