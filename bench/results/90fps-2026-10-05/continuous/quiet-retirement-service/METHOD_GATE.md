# Quiet retirement service gate

This CPU fixture compiles the exact extracted accumulator methods `next_nack_deadline`, `next_poll_deadline`, `poll_nacks`, `try_nack`, `pump`, and `try_submit_front` against the production `frame_window`, `shard_set`, and NACK deadline helper. A small shell substitutes only scene sending, clock, and decoder interfaces. It does not execute the constructor, `push_shard`, `process_packets`, the poll loop, or XR/Vulkan decoding.

The passing virtual-time case leaves frame 0 with confirmed hole 1 and a terminal shard, while frame 1 is complete. It confirms the earlier NACK deadlines win first, both existing NACK rounds are spent, the retirement deadline remains next, just-before-due service retains the front, and service at due retires frame 0 and pumps frame 1 exactly once. Controls cover deadline opt-out, retransmission disabled, non-ASTC, expired scene, zero/negative/overflowing display periods, invalid/rolled-back/overflowing first timestamps, no newer complete frame, and ceil/future/overdue timeout behavior.

`extract.py` also structurally checks the actual `stream::process_packets` callsite: its timeout supplier uses `next_poll_deadline`, passes the 100 ms cap to network poll, then calls `poll_nacks` after poll. This is a source check, not execution of the network loop. No measured headset, Wi-Fi, decoder, display, or latency claim follows.

Run from the checkout used for the evidence:

```sh
./run-check.sh /run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe ./results
```

Final captured outputs are in `methods/`: normal and halt-on-error ASan/UBSan each report 96 checks, 0 failures. `provenance.json` and `hashes.txt` pin the extracted source and headers. Earlier iterations remain in scratch; see HARNESS_ITERATIONS.md.
