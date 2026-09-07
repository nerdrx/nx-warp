# Matched fixed-QP40 pace-45 capture

The matched `atlas:auto` and `atlas:off` runs used APK `5a095bbc730a51f52522f4e3f2ae103169ace65f57006f3dc4fa7614bd7721ac`, integration commit `8868b201`, fixed QP 40, and server pace 45. Active-window new-source medians were 45/s for both. Boundary-excluded wall counts were 32.43/s (atlas) and 32.35/s (off); report gaps reached 5.522 and 6.842 s. Decoder GPU medians were 13.2 ms (atlas) and 13.9 ms (off). This does not verify a causal small gain, and makes no 240 FPS claim.

Both option configurations and the [Pico thermal snapshot](pico-thermal-1918.txt) are retained. Clock state was not captured, so thermal and frequency effects are uncontrolled. Screenshots ([atlas awake 1](atlas-awake-1.png), [atlas awake 8](atlas-awake-8.png), [off awake 1](off-awake-1.png), [off awake 8](off-awake-8.png)) are capture artifacts, not quality proof.

Recompute with `python3 summarize.py`; it imports the parent `read_pair` parser. The ACK merge fix in `8868b201` is separately covered by the [ACK-only evidence](../ack-feedback/ack-only/README.md).
