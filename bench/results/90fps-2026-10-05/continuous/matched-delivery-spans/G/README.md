# Matched send/receive spans candidate (scratch only)

This scratch copy starts from production D at `412a2bfe9ef54333fdc35cf9919c6443fa409909`.
It changes only the rate denominator in `bitrate_controller::close_frame`: for streams that already contribute bytes and valid positive receive intervals, use the larger of the receive-interval union and send-interval union when **all** such streams also have positive, ordered send endpoints. The unions are measured within each timestamp domain; absolute send and receive values are never compared. If any contributing stream lacks valid send endpoints, D's receive-only denominator is preserved. The send cap is skipped when the existing direct-quality predicate (`quality_stream_mask == 1 && measured_stream_mask == 1`) holds. Receive-based loaded admission and utilisation, loss, radio, startup, AIMD and wire behavior are unchanged. Duplicate metadata is merged only when both endpoints in that individual observation are positive and ordered, so begin-only/end-only fragments cannot fabricate a pair.

Duplicate feedback conservatively merges positive send starts by minimum and positive send ends by maximum for that stream/frame. The focused actual-controller test verifies overlap (533.333 Mbit/s), serial send spans (266.667 Mbit/s), independent send-clock origin shift invariance, missing/zero/reversed endpoint fallback, and split duplicate updates recovering the same full envelope. Existing five-suite fixtures retain zero send timestamps and continue to test the D fallback; they do not establish the behavior or availability of real send timestamps. No runtime or network measurement is represented here. In particular, a pacing-only send span shorter than the receive span will not clamp the rate.

## Validation

From this directory, using the clean source checkout for dependencies and the other four unchanged test sources:

```sh
/home/nerdrx/.local/bin/rtk bash run_suites.sh /run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe results/normal-final
/home/nerdrx/.local/bin/rtk bash run_suites_san.sh /run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe results/san-final
```

Both commands exited 0. The normal and halt-on-error ASan/UBSan runs each passed all five suites; the BBR suite reported 101 checks and 0 failures. Published final stdout is retained in `../validation/{normal,san}/`; compile flags and source hashes are in each run's `source-sha256.txt` (the scripts compile the scratch controller and the scratch BBR test first on the include path).

Frozen SHA-256:

- `server/driver/bitrate_controller.cpp`: `7d3eb312ce8db64718f6ea6cbf28ffbd927cb6a5a732a69b2d3f348f47e910ad`
- `server/driver/bitrate_controller.h`: `1f0cf3b818927ba9a3c6eabfbff6144b40cd306903d6f0a318a1142359c14f9b`
- `tests/bitrate_bbr_test.cpp`: `90b8133a1059d78d452055e3cf1c1f92f30a4053a20e3344754758b55b48f9ce`
