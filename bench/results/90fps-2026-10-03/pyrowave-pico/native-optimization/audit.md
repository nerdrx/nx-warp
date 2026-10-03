# NXVC vs PyroWave Pico timing audit

The saved figures show a real wall-time gap, but not a controlled 8x decoder comparison. The NXVC run is 4:4:4 (718,376-byte QP22 stream); PyroWave is 4:2:0 (694,484 bytes). NXVC reports ~185 ms wall p50 and PyroWave ~22.3 ms host-wall p50. Chroma workload differs, and the two harnesses do not measure an identical pipeline.

## Original timestamp evidence

`nxvc-vkdec-profile.cpp:338` prints `gpu_timestamps=unavailable` as a hardcoded literal; it never calls `nxvc_vk_decoder_timestamp_info`. But the 30 warm rows in `nxvc-native-q22-444-repeat42.log` contain positive Pass A, Pass B, and GPU values (medians 129.024, 53.598, and 182.115 ms). The runtime API reports period 0 only when `have_timestamps` is false; that flag requires timestamp compute/graphics support, positive timestamp period, nonzero valid bits, and successful query-pool creation (`vk/decoder/nxvc_vkdec.cpp:905-907, 4742-4746`). Thus the header is not proof of unavailable timestamps. The positive rows imply timestamps were reported to stats, but the saved harness omitted the capability record needed to independently validate them. Treat the stage split as strong but unverified; do not cite the header as evidence that timings are invalid.

## GPU cost center

The corrected runtime check below confirms Pass A as the dominant measured stage: ~129 ms median versus ~54 ms Pass B; host parse and submit are only ~0.38 and ~0.30 ms. Pass A's rANS shader advances lanes through a serial scheduling-round loop (`rans_decode.comp`: `lane_next`, symbol/table decode, state update), with workgroup barriers to publish/clear shared renormalization flags every round (around lines 1400-1465). The shader demonstrates serial dependencies and synchronization; this run does not isolate the cost of each barrier or measure occupancy.

Pass B is still substantial. The 4:4:4 fixture reconstructs three full-resolution planes; its coded-block path dequantizes coefficients, runs row and column inverse transforms, then enters an intra-prediction wavefront with barriers (`reconstruct.comp`, around lines 1490-1530 and 2018-2130). The shader notes large transforms use dense odd-coefficient products. This supports transform/reconstruction as the secondary cost, but the fixture does not isolate transform time from prediction, plane count, or stores. No occupancy measurement is present in these logs.

## Conclusion and next measurement

### Completed runtime check

The corrected profiler subsequently ran twelve warmups and thirty retained samples on the existing device QP22 4:4:4 fixture. It confirmed period 52.083332 ns, 48 valid bits, and thirty measured GPU spans. Pass A p50/p95 was 128.681/137.137 ms; Pass B was 53.767/54.555 ms. GPU total was 181.761/190.905 ms and synchronous wall time was 184.630/193.955 ms. Entropy dominance is therefore measured in this reference fixture, rather than inferred solely from the earlier contradictory header. Full provenance and the separate archive used by this repeat are recorded in `native-profile-results.md` and `nxvc-timestamps-provenance.txt`.

The runtime check establishes entropy decode first and reconstruction second as the NXVC reference GPU bottlenecks. The wall-time disparity is established for these isolated runs; its precise quality-matched codec ratio is not. The later PyroWave 4:4:4 timing probe removes the sample-count difference, but still needs a validated encoder fixture and reference output. The remaining comparison gate is matching native dimensions, chroma format, encoded quality, and timing scope across both implementations.
