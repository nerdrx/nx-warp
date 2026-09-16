# Decoder buffer cache before driver queries

Status: **Android build passed; not installed or timed on the Pico.** This candidate also includes the earlier [cached-submission change](../cached-submit/README.md).

## Work removed from decoded-frame delivery

The Android ImageReader callback imports a decoded image into Vulkan, then publishes it to the renderer. It already cached these imports by `AHardwareBuffer*`. However, every use still called `AHardwareBuffer_describe` and `vkGetAndroidHardwareBufferPropertiesANDROID` before checking that cache.

The lookup now comes first, under the same mutex. A cached buffer immediately returns its existing mapping. A new buffer still follows the full descriptor/property query, format check and import path. There is no buffer copy, extra GPU pass, resolution reduction, frame drop or change to MediaCodec job ordering.

This removes two queries on each cache hit; it does not establish how expensive those queries were on this Pico. Driver cost and cache-hit frequency are unmeasured while the headset is disconnected.

## Why the buffer identity remains valid

Vulkan imports acquire a reference to the Android hardware buffer and keep it until the imported memory is freed. The mapping cache owns that imported memory, preventing the buffer identity from being recycled while cached. Buffer dimensions and format are intrinsic allocation properties. The existing format-change handling remains on the new-buffer path. [Vulkan memory specification](https://docs.vulkan.org/spec/latest/chapters/memory.html).

The mutex and object lifetimes are unchanged. This is a reordered existing cache check, not a new cache or an increase in retained decoder surfaces.

## Optional latency instrumentation

Set `debug.wivrn.nx.decode_trace=1` before restarting the client to enable diagnostics. It defaults off. When the decoder shuts down normally, it reports per-stream output callback count, mean/max wait from callback enqueue to worker execution immediately before output release, plus mapping-cache hits and property-query attempts.

The queue measurement excludes the hardware decode, the `releaseOutputBuffer` call itself, subsequent ImageReader processing and presentation. It is not motion-to-photon latency. No per-frame logging occurs; the counters are printed after the worker joins. Mapping counters are a shutdown snapshot under the existing image-map mutex, not a guarantee that every late ImageReader callback has drained. A force-kill may produce no summary.

Use that measurement to decide whether the output-release queue warrants a separate change. Input/output job ordering and decoder threading have deliberately not changed in this candidate.

## Verification and next device check

- Release Android build passed with the same custom `.warp` package; wire protocol unchanged.
- Independent source review checked cache ownership, mutex coverage, initialization before MediaCodec starts, and output-counter reads after worker join.
- No device test, host simulation of Android hardware, or latency gain is claimed.

On reconnect, compare the same moving application and resolution with the prior cached-submission APK versus this candidate. Record source/fresh-frame cadence and latency stage counters; enable decode tracing only for the diagnostic run, then disable it for performance comparisons. Exercise normal decoder shutdown and reconnect to collect the summaries. Keep the motion-off headroom profile for both comparisons.

Source: [WiVRn 5d8704f5](https://github.com/nerdrx/wivrn-nx/commit/5d8704f569e9eda0b7d48e90ed950504e2712162). [Build log](android-build.log) · [Candidate APK identity](status.json). Local APK: `nx-scratch/motion-regions/decode-cache/client.apk`. It includes the prior cached-submission optimization and uses the same wire protocol.
