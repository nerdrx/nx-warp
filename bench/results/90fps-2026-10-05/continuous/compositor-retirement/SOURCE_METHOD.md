# Compositor retirement checks

`results/final/` and `results/revised/` retain the earlier CPU-only state-machine model (32 and 37 checks). They are illustrative and do not execute production code.

`source-check-results/` is a separate regression extracted from the current `compositor.cpp`. `extract-check.py` verifies the header fields and constructor fence initializer, verifies that the complete compute-wait branch does not write `submission_pending`, then extracts the actual pre-acquire guard, acquire/no-image branch, pool reset, and submit/reset/publication block into a small fake-Vulkan C++ harness. The harness executes those snippets with controlled fake results; it does not call Vulkan or execute the complete compositor method. The compute-success check executes only the extracted `motion_unsafe = false` assignment and confirms that it leaves `submission_pending` unchanged. That assignment check is not evidence about the whole branch; the separate static scan covers that property.

Normal and halt-on-error ASan/UBSan runs each pass 37 checks (`source-check-results/normal.log`, `san.log`; both commands exit 0). The harness checks no-pending avoids a fence poll; timeout returns before acquire/pool reset and clears rendering; successful retirement followed by acquire failure clears rendering without resetting the pool; wait errors preserve pending state; reset/submit errors do not publish the semaphore value or pending flag; successful submit passes the fence and compute-stage signal and publishes once. A submit→timeout→late-success→resubmit cycle confirms blocked state is retained, acquisition/pool reset wait for successful retirement, then the same fence is reused with semaphore values 6 and 7. The compute-success assignment does not retire the submit fence.

Run from this directory:

```sh
rtk proxy ./run-source-check.sh /run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/compositor-retirement/source-check-results
```

The captured production source hashes are in `source-check-results/hashes.txt`; extraction provenance and line locations are in `source-check-results/provenance-captured.txt`. The source checkout already had root-owned changes; this gate made no production edits.

This validates control-flow snippets, not Vulkan execution, compositor image lifetime, or downstream consumers. The compositor-submit fence retires the compositor submission only; each downstream reader (including any mirror path) needs its own lifetime protection. No GPU, device, runtime, or application state was changed here.
