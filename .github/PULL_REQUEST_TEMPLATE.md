## What this changes

<!-- One paragraph. Say what you measured, not what you expect. -->

Closes #

## Bit-exactness

The reference codec in `ref/` is the specification. Tick what applies.

- [ ] This does **not** change the bitstream, and every existing conformance vector still decodes to the same hashes.
- [ ] This **does** change the bitstream. Then, in this same commit:
  - [ ] `ref/` updated
  - [ ] `tests/vectors/` regenerated, and the commit message says so
  - [ ] `docs/SYNTAX.md` updated
  - [ ] `docs/PAPER.md` updated where it now disagrees
- [ ] GPU output is bit-identical to `ref/` for the conformance corpus on every device I ran (list them below).

Devices and ICDs this was run on:

<!-- e.g. lavapipe 24.x, RADV 24.x on RX 6800, NVIDIA 555 on RTX 3060, Adreno 650 on Pico 4 -->

## Decisions

- [ ] This encodes a decision, and an ADR is added under `docs/adr/`.
- [ ] No decision here — this implements one already recorded.

## Documentation

- [ ] Component README updated, or no user-visible change.
- [ ] Numbers produced by this change are in the tree (a report, a results JSON, a table), not only in this description.

## Checks

- [ ] `ctest` passes locally, and tests that need an absent dependency skip rather than fail.
- [ ] `clang-format --style=file` clean on the files I touched.
- [ ] One component per commit where it was possible.

<!-- How this project is run, what the labels mean, and what "done" means:
     .github/PROCESS.md -->

See [.github/PROCESS.md](https://github.com/nerdrx/nx-warp/blob/main/.github/PROCESS.md) for how issues flow and what counts as done.
