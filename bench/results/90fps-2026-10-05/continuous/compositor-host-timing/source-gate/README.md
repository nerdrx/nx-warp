# Host timing helper extraction

`extract-check.py` extracts the current `host_stage_names`, `host_times`, `host_stamp`, and `host_dump` block from `compositor.cpp`. It adapts `std::format` to a counted fake formatter and substitutes a fake monotonic clock/session boundary; stage capture logic is otherwise extracted as written. Static source-order assertions check the 18 start/end placements around the guarded acquire, recording, lock/submit, encoder-present loop, timeline, query, and GC sites, plus all three flush paths.

Normal and halt-on-error ASan/UBSan executions each pass 34 checks. Tests cover disabled no-clock/no-format/no-dump behavior, one completed plus one incomplete interval, full nine-stage capture, positive duration, exact frame/outcome fields, and omission of unexecuted stages. The C++ harness does not run `layer_commit` or Vulkan.

Run from this directory:

```sh
rtk proxy ./run-check.sh /run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe results
```
