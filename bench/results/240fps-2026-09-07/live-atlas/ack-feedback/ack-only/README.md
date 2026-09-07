# ACK-only capture

This capture used the same APK (`5a095bbc730a51f52522f4e3f2ae103169ace65f57006f3dc4fa7614bd7721ac`) with the bootstrap wait removed; the server helper build completed. Removing the bootstrap did not establish an advantage.

Active-window medians were 12.65 ms decoder GPU time and 32.5 new sources/s. The boundary-excluded reported count was 26.406/s over the captured wall span, with report gaps up to 4.844 s. These are reported-window measurements, not physical display FPS, and do not support a performance claim.

The retained screenshots ([awake 1](awake-1.png), [awake 8](awake-8.png)) are unchanged capture artifacts. Recompute with `python3 summarize.py`; it imports the parent `read_pair` parser. `summary.json` retains the original source hashes.
