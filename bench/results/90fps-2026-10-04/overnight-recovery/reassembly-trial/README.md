# ASTC reassembly latency trial

The generic receive window holds six frame indices and tolerates three indices
of skew. A complete successor can therefore wait behind a damaged older frame
until a complete frame four indices newer exists. This is useful for reordered
Wi-Fi/USB delivery and reference-dependent codecs, but can add avoidable delay
to independent ASTC images.

The Android client now accepts an **experimental, default-off** property when
constructing an ASTC accumulator:

```sh
adb shell setprop debug.wivrn.nx.astc_skew 1
```

Value `1` keeps one index of skew and retires an incomplete front when a complete
frame two indices newer exists. Value `0` retires it when the first complete
successor exists. Empty, `3`, or any unsupported value preserves the default
three-index policy. The property is read when the accumulator is constructed;
changing it does not alter an active accumulator. It applies only to NX ASTC;
HEVC, H.264 and other codecs retain their existing policy. No property was set,
client installed, or active session restarted for this implementation.

At an illustrative 90 source images/s, the complete-successor thresholds are
44.44, 22.22 and 11.11 ms for skew 3, 1 and 0. These are **frame-index arithmetic**,
not wall-clock deadlines or measured latency. Slow or skipped source updates
stretch the elapsed wait. GPU, presentation, network and photon time are absent.

The window still processes frame indices monotonically. Late retired shards
are refused, and every retirement reports feedback. An incomplete front without
a newer complete frame is retained until the normal hard window bound.
ASTC assembly clears an incomplete packet when a newer frame starts. Motion
packets only reference successfully decoded acknowledgements (maximum age eight
indices, sixteen retained references); skipping an undecoded packet cannot
make it an acknowledged reference. Existing reference availability checks stay.

Shorter skew can forfeit FEC/NACK repairs or legitimate path reordering. Each eye
retires independently: exact stereo intersection prevents mismatched-eye display,
but divergent retirements may reduce the number of usable common pairs. Try
skew 1 before 0 and compare both fresh coherent updates and reassembly delay.
It is not a safe default based on unit checks alone.

Validation: the existing production frame-window/shard-set test plus opt-in
threshold, delivery-order, late-shard and no-successor checks passes **188 checks**
normally and with AddressSanitizer/UndefinedBehaviorSanitizer. Those tests do not
exercise the Vulkan decoder, Android property route, live NACK timing, or XR
stereo presentation. Build and headset validation must be reported separately.

![Component evidence and limits](reassembly-threshold.png)
