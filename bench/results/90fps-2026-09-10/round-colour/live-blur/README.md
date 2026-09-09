# Round-colour live blur pair

This bundle summarizes one bounded 60-second off/on capture pair from the
Pico harness. Both runs used the fresh APK, `round-colour=1`, colour encoder
mode, priority 1, wait 4000 us, compact borrowed FDM 1, and the full-field
scene. The off run had `peripheral_smooth=0`; the on run had
`peripheral_smooth=2`.

The on run raised client own-GPU time from 5.710 to 10.226 ms (+4.517 ms,
79.1%), source display-time offset from 56.738 to 78.913 ms (+22.175 ms,
39.1%), and lowered fresh-source rate from 80.65 to 55.12/s (-31.7%).
Decoder nxvc GPU time changed 3.662 to 3.793 ms (+0.131 ms, 3.6%); fence-post
time changed 6.293 to 9.980 ms (+3.687 ms), with queue time 2.597 to 6.167 ms.

These are descriptive results from one pair, not a statistical proof. Timings
are harness-reported own-GPU and source-offset values, not photon-to-photon
latency. Both runs completed their requested window, with no ERROR/ANR/crash
text. Visibility/session transitions occurred repeatedly in both logs (40
off, 42 on state-change lines); the on log also shows a stop/restart interval,
so transitions and queue instability are part of the observed capture.

Reproduce the offline summary from the repository root:

```sh
python3 nx-warp/bench/results/90fps-2026-09-10/round-colour/live-blur/summarize_round_colour_blur.py \
  nx-scratch/motion-live/round-colour-off-client.log \
  nx-scratch/motion-live/round-colour-on-client.log \
  > nx-warp/bench/results/90fps-2026-09-10/round-colour/live-blur/summary.json
```

`logs.tgz` contains the paired client, scene, server, and status captures;
`ACTIVE_USER_PROFILE.json` preserves the harness profile snapshot.
