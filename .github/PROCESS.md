# How NX Warp is run

This file describes the project's tracking: what the labels mean, what the milestones gate, how
an issue flows, and what counts as done. It lives in `.github/` because `docs/` holds the design
paper and the normative syntax, and process notes have no business sitting next to a
specification.

The one rule that outranks everything here is in
[CONTRIBUTING.md](../CONTRIBUTING.md): **the CPU reference decoder in `ref/` is the
specification.** Process exists to protect that, not to compete with it.

## Labels

Labels are defined once in [`.github/labels.yml`](labels.yml) and synced to the repository by
[`.github/workflows/labels.yml`](workflows/labels.yml) on every push to `main`. Edit the file,
not the repository — a label created by hand is gone at the next sync.

Every issue carries **one area**, **one type**, **one phase** and **one priority**. Status labels
are added as circumstances demand and removed when they stop being true.

### area: — where the work lands

`area:bitstream` `area:prediction` `area:transport` `area:ratecontrol` `area:vulkan`
`area:android` `area:windows` `area:wivrn` `area:tools` `area:docs` `area:spec` `area:ci`

An issue can carry two areas when it genuinely spans them — a bitstream change that the transport
must also honour, for instance. Three means the issue should probably be split.

### type: — what kind of work it is

| Label | What it means | What closes it |
|---|---|---|
| `type:bug` | Behaviour differs from the reference or the spec | A fix plus a regression vector or test |
| `type:feature` | A capability to build | The acceptance criteria, all ticked |
| `type:experiment` | Measure something | A number in the tree |
| `type:risk` | An open risk from the paper | The kill or verify test run, and its result recorded |
| `type:decision` | A choice to make | An ADR under `docs/adr/` |

### phase: — which gate it belongs to

`phase:0` through `phase:4`, matching the milestones and the roadmap in paper 7.3. The phase
label and the milestone say the same thing; the label exists so filters compose.

### Priority

| Label | Meaning |
|---|---|
| `P0` | Blocks the current phase gate. If it is not moving, that is a problem worth raising. |
| `P1` | Needed for the current phase to exit. |
| `P2` | Wanted, not gating. |
| `P3` | Nice to have, or a later phase. |

Priority is relative to the *current* phase. A `P0` on `phase:4` is not urgent today; it is what
will be urgent first when Phase 4 opens.

### Status

- `needs-hardware` — cannot be finished without a Pico 4, a Windows machine, or a specific GPU.
  These are the issues most likely to sit still, so they are labelled rather than left to be
  rediscovered.
- `needs-fto-review` — part of the scoped freedom-to-operate review in paper 6.12. All of these
  must close before Phase 3 ships.
- `blocked` — waiting on another issue or an outside answer. Say which, in a comment, or the
  label is noise.

### Contribution

- `good-first-issue` — self-contained, specified in enough detail to start, and needs no hardware.
- `help-wanted` — needs attention, or a device that is not available here.

## Milestones

One per phase, with the exit criteria from paper 7.3 in the description. A milestone closes when
every criterion in its description is met and demonstrated in the tree — not when its issues
happen to all be closed.

| Milestone | Gate |
|---|---|
| Phase 0 — Adreno gate | K1 to K6 measured on the Pico 4; the pure-versus-hybrid decision recorded |
| Phase 1 — Intra codec | Bit-exact on lavapipe, RADV and Adreno; within 1 dB of x264 intra; Pass B under 5 ms p50; 24 h fuzz clean |
| Phase 2 — Pose-warped inter | Cross-vendor determinism green; within 10 percent of x265 at rest and 30 percent better on motion; no drift under 5 percent loss |
| Phase 3 — WiVRn NX integration | Glass-to-glass under 40 ms at 150 Mbit; one hour on the Pico 4 without a crash; FTO review done |
| Phase 4 — Stereo, foveation, depth | 25 percent saving at equal FovVideoVDP on the fovea; decode time unchanged |

## How an issue flows

1. **Filed** through one of the five forms in [`ISSUE_TEMPLATE/`](ISSUE_TEMPLATE), which exist so
   that the fields the project actually needs — pass threshold, first mismatching pixel, whether
   the bitstream changes — are asked for rather than remembered.
2. **Labelled** with area, type, phase, priority. If it needs a device it gets `needs-hardware`
   immediately, because that changes who can pick it up.
3. **Worked** on a branch. One component per commit where possible; the tree is built by several
   people at once.
4. **Reviewed** through a pull request using the template, which asks the bit-exactness questions
   before the reviewer has to.
5. **Closed** by the merge, or by a comment carrying the number the issue asked for.

An issue is not closed because it stopped feeling urgent. If it is no longer worth doing, say
why, and close it with that reason written down.

## Definition of done

Different types are done differently, and pretending otherwise is how measurement quietly turns
into opinion.

- **A feature** is done when its acceptance criteria are ticked, the tests are registered under
  the component prefix and skip cleanly without their optional dependencies, and any bitstream
  change moved `ref/`, `tests/vectors/`, `docs/SYNTAX.md` and the paper together.
- **An experiment** is done when the number is in the tree — a results JSON, a report under
  `tools/quality/reports/`, a filled table. Not in a comment. Not in a screenshot.
- **A risk** is done when its kill or verify test has been run and the result recorded, whichever
  way it went. A risk closed because nobody hit it is a risk that is still open.
- **A decision** is done when an ADR exists under `docs/adr/` with context, decision and
  consequences.
- **A bug** is done when a test or a conformance vector would have caught it, and now does.

## Things that are deliberately not here

- **Branch protection** is not configured by this process document, and no automation here
  touches it.
- **Wiki and projects** are disabled. The paper, the syntax document and these issues are the
  whole record; a fourth place to write things down would only disagree with the other three.
- **`docs/`** is not process territory. It holds the design paper, the normative syntax and the
  integration plan.
