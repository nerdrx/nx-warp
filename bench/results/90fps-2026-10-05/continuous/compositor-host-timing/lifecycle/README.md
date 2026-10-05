# Fence lifecycle extraction against the host-timing patch

This is the previous compositor-retirement snippet harness adapted to the current timeline assignment and local host-timing helper block. The helper runs with a disabled fake session in the lifecycle cases. Normal and halt-on-error ASan/UBSan each pass 37 checks; provenance and hashes are under `results/`. The harness extracts selected source snippets and uses fake Vulkan objects; it does not execute the whole compositor or real Vulkan.

Run from this directory:

```sh
rtk proxy ./run-source-check.sh /run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe results
```
