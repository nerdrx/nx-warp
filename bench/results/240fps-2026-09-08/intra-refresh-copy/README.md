# Copy the reference after a complete INTRA refresh

The materialized-reference shortcut now also recognizes an ATLAS frame that
accepted every tile as full-resolution INTRA. WRITEBACK has reset all entries to
valid identity transforms, so a following PICTURE frame can copy the existing
reference instead of running ASSEMBLE over every pixel. Partial updates,
non-INTRA tiles and reduced-resolution tiles cannot establish this proof.
The existing conservative reset/import handling and force-assembly control remain.

## Validation and scope

- Final host conformance: 161 streams checked, 3 unsupported skipped, zero failures.
- Native Pico three-frame reset → PICTURE → ATLAS fixture: output hashes with
  copy enabled and forced assembly match (see `final-reset-output.sha256`).
- The transition actually exercises the shortcut: frame 1 drops from 8 to 6
  dispatches. Initial prototype GPU times were 142.14 → 100.77 ms for that frame;
  these **cold first-use samples are not steady-state performance evidence**.
  The prototype also admitted complete inter refreshes; the final guard is
  deliberately restricted to INTRA, matching this fixture.
- RADV synchronization validation passes the native transition (`refresh-sync.log`).
- The 120-frame dense motion fixture did not exercise additional shortcuts under
  the broader prototype: its dispatch counts were unchanged. A faster wall-time
  sample there is therefore **not attributed to this patch**.

Reproduce with the native fixture in
[the staging regression](../native-pose-transition/README.md), using the current
`nxvc-vkdec` normally and with `NXVC_VKD_ATLAS_FORCE_ASSEMBLE=1`.
Use `--no-out --atlas-view r8 --stats --throughput` for timing, and separate
`--out /dev/stdout --quiet` runs piped to SHA256 for output comparison.

## Experiment rejected for now: paired chroma prediction

The existing `NXVW_ABL_CHROMAPAIR` shader variant was tested on the native dense
QP40/threshold128 motion fixture with the previously shipped materialized-copy
baseline. Motion-phase median decode wall times (frames 25–95):

| Control before | Paired chroma | Control after |
|---:|---:|---:|
| 66.658 ms | 67.367 ms | 66.209 ms |

This short control/candidate/control comparison provides no speedup. The build
flag was restored to empty and the variant is not enabled in the shipped client.
Raw logs retain cold startup, mode transitions and recovery. There is no new
quality, thermal, live-motion or 240 Hz claim from these experiments.
