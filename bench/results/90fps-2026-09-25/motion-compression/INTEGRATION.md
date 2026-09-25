# Motion compression: matched Pico integration run

The opt-in motion stream reduced the encoder-reported frame size by 12.1% in this matched run, while adding about 1.78 ms of host encode time per frame. The headset rendered at about 89.8 Hz in both arms. One transport-incomplete frame was dropped during the motion run; the decoder reported zero missing references.

| Measurement | Motion enabled | Independent-frame baseline | Change |
|---|---:|---:|---:|
| Encoder frame bytes, mean | 90,938 B | 103,419 B | −12.1% |
| Host encode time, mean | 7.12 ms/frame | 5.34 ms/frame | +1.78 ms |
| Pico render rate, mean | 89.79 Hz | 89.76 Hz | +0.03 Hz |
| New-source iterations, mean per 2 s | 172.7 | 178.0 | −5.3 |

The comparison used nine aligned, complete two-second windows per arm after the first two seconds of the uploaded scene. Both runs used the NX direct backend at 2176×2176 per eye and 90 Hz, with native RGB888, Zstd byte prediction, LZ4 safety, and checkerboard disabled. The same decoder APK and server binary were used. The scene was a controlled four-position photographic sequence, not head movement, a game, or a general live-VR benchmark.

The motion run logged `exact motion compression true`; its last periodic sender checkpoint showed 1,264 selected candidates out of 1,792 attempts and 26,761,305 cumulative saved bytes. The client’s last report showed 1,261 exact motion details decoded and zero missing references. In one matched network window, one frame had a transport hole and was dropped; this was not a missing-reference event. The explicit rejected-dependent-frame counter was not present in the captured logs.

The protocol feature bit is 64. The complete stream flag value is expected to be 82 for this trusted-LAN predictor stream; that value is derived from the format version and feature bit, because the run did not retain a packet-header capture. The client’s motion-enabled and exact-decode logs confirm feature negotiation and decoding.

The encoder byte counts include the safety/detail envelope as reported before packetization. They exclude later FEC, transport padding, and retransmissions, so they do not represent end-to-end bandwidth. The nine windows are a short controlled comparison, not independent randomized trials. The periodic motion savings checkpoint is cumulative and may precede the final frame.

The production Vulkan codec test passed no-ACK fallback, exact reconstruction from an ACKed reference, forgotten-reference fallback, periodic anchor, and reset cases. Host and Pico protocol tests also passed. [Machine-readable measurements](integration.json) include the matched window samples, checksums, and caveats.
