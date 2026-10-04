# Matched native ASTC handoff capture

Three short off-head, stationary WayVR captures used the same installed APK and 2176×2176-per-eye native ASTC 8×8 input. The client had no foveation, field warp, or blur enabled. These are observed runtime windows, not a motion, complex-scene, perceived-quality, or photon-latency test. Quality and bytes were not controlled as an A/B comparison.

| Capture | PID | filtered client windows (steady) | render rate median | estimated fresh-source rate median | server nonblack / idle-black windows* | own GPU pass min–max |
|---|---:|---:|---:|---:|---:|---:|
| planar | 9916 | 8 (8 steady) | 89.5/s | 86.0/s | 18 / 2 | 1.6–2.8 ms |
| rgb | 9918 | 8 (8 steady) | 89.5/s | 81.4/s | 18 / 2 | 1.7–2.9 ms |
| pause | 9920 | 13 (12 steady) | 89.5/s | 87.6/s | 24 / 0 | 1.4–6.4 ms |

![Per-window render iteration rate versus fresh source update rate](window-rates.png)

The app-PID log dump includes buffered records preceding the host-recorded capture. `windows.csv` marks those rows `buffered_pre_or_outside_capture` and retains their valid per-window stats; session medians and graph use only the selected filtered client extracts (8 planar, 8 RGB, 13 pause rows). Thus dump row counts are not strict 20-second totals and are not time-aligned with the server window counts marked *.

The app prints window duration to 0.1 s and its iteration rate rounded to 0.1/s. The graph uses that app-reported rate; fresh updates/s is estimated as reported rate × new-source / iterations. It is approximate, not a rate computed from exact duration. Startup partial rows are retained but omitted from the graph. The graph shows app iterations and `new-source` counts, not network throughput or encoded quality. GPU cost varied materially: the pause capture reached 6.4 ms per iteration, so these captures do not establish a stable pass-time advantage.

The old client log label `source->first` is a signed offset to a predicted display target, not photon-to-photon latency; this report makes no photon-latency claim. A later, separate controlled producer-pause run (`jit-gradual-pause-g`, APK `0662e875…`) records device-clock order: pause BEGIN 08:19:07.220; stale-frame warning 08:19:08.216; fresh-view state resume 08:19:09.774; device END marker 08:19:09.805. The headset session then becomes idle at 08:19:10.063, returns visible/focused at 08:19:11.221, logs an old-frame warning at 08:19:11.227, and logs fresh-view resume at 08:19:11.228. Thus the warning precedes the fresh callback; it does not show nearest-target selection choosing an old frame while a fresh shared pair was already available. `client/scenes/stream.cpp` skips publication in `push_blit_handle()` while the XR session is hidden, which explains why the first render after wake can still use a retained image. The host UTC SIGSTOP/SIGCONT records are separate from device log time and cannot establish a cross-clock recovery interval. The run is a producer-pause check, not a real network-recovery guarantee. The full pacing evidence and filtered device timeline are in [ASTC JIT pacing](../astc-jit-pacing/README.md). The asterisked server windows remain independent counts, not row-for-row matches to client stats.

The first RGB capture attempt stopped on a PID race before producing a valid timed run. The included RGB capture is the successful rerun (PID 9918). All three included captures report the same installed APK SHA-256. Source was clean at commit `9d05f4d`; the manifest records source/native/APK/signing hashes and installed-package verification. The APK and unfiltered/private startup logs are not included.

`*-client-extract.txt`, `*-server-extract.txt`, and `*-server-stages.txt` preserve filtered per-window evidence. `windows.csv` preserves all app-PID summary rows, marks buffered/outside rows, and records printed rates, estimated fresh-update rates, and nearby GPU-pass stats. `pause-host-markers.json` preserves the PC markers with the clock limitation above. `manifest.json` hashes every report file.
