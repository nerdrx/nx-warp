# Compositor host-timing source checks

The source-linked fake-boundary checks and runnable commands are in `source-gate/` and `lifecycle/`. They target the root-owned, uncommitted timing patch at source SHA `6d760a74f4110c0fb7fa228043d4cf5498540608d1a592c5fe458ea6c4ff6aa8` (compositor) and header SHA `31dd5ac382dfb90f9a6c7193cdb43d422748e79edec7bdcb955e29adbb482ede` (session).

`source-gate` extracts the exact stage-name/clock/dump block, adapting only `std::format` to a counted fake formatter and the monotonic clock/session calls to fakes. Normal and ASan/UBSan runs each pass 34 checks. It verifies disabled mode calls no new clock, formatter, or dump; all nine named stages pair and carry end/start, frame and outcome; incomplete/unrun slots emit no rows; and source-order checks bracket acquire, command recording, queue lock/submit, encoder-present loop, timeline, query, and GC. Flush order is checked for timeout/no-image exits and the normal after-GC path. This executes only the extracted helper block, not `layer_commit`.

`lifecycle` adapts the earlier extracted fence-lifecycle harness to include the current host helper block with a disabled fake session and to parse the current timeline assignment. Normal and halt-on-error ASan/UBSan each retain 37 passing lifecycle checks. It remains a control-flow snippet test with fake Vulkan objects, not Vulkan or full-compositor execution.

Run each from its own directory with the source checkout and an output path:

```sh
cd source-gate
rtk proxy ./run-check.sh /run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe results
cd ../lifecycle
rtk proxy ./run-source-check.sh /run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe results
```

Raw stdout, generated C++, commands, and source provenance are retained under each `results/` directory. No production edits or runtime/GPU/device changes were made by these gates.
