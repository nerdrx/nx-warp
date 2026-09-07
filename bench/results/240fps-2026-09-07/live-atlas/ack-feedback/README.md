# ACK-bootstrap capture

This isolated capture used APK `5a095bbc730a51f52522f4e3f2ae103169ace65f57006f3dc4fa7614bd7721ac`. The atlas bootstrap sent its first frame with an observed 887.9 ms submit, exceeding the 500 ms confirmation gate; the server then resumed after the timeout. This records the ACK/bootstrap behavior only and makes no performance or deadlock claim.

The active-window medians were 12.9 ms decoder GPU time and 32 new sources/s. Boundary-excluded reported counts were 22.42 new sources/s across a 46.432 s span, with report gaps up to 6.806 s. Server reports show roughly 140–230 skipped tiles in later windows, so this is not a zero-skip comparison. Rates are reported-window measurements, not physical display FPS.

The retained screenshots ([awake 1](awake-1.png), [awake 8](awake-8.png)) are copied unchanged; the first shows the checkerboard scene while the second is black. They are capture evidence, not a quality proof. Recompute with `python3 summarize.py`; it imports the parent `read_pair` parser. `summary.json` retains the original source hashes.
