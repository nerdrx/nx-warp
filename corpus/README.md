# The NX Warp test corpus

Every quality number in this project is a number *about some material*. This
directory is the record of what that material is, where it came from, and how to
get exactly the same bytes on another machine.

**The corpus is not in the repository.** One second of 2048x2048 stereo 4:4:4 at
90 Hz is about 2 GB. What is in the repository is `MANIFEST.json` — names,
geometry, provenance, licence and SHA-256 — plus two scripts that turn it into
files and check them:

```sh
python3 corpus/fetch.py                  # dry run: what exists, what does not
python3 corpus/fetch.py --sync           # generate the synthetic entries
python3 corpus/verify.py                 # check hashes
```

Files land under `$NXW_CORPUS`, defaulting to
`/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/corpus`. `fetch.py` refuses a
root inside the repository (it would get committed) and a root under `/tmp` (it
is tmpfs here and would eat RAM).

The synthetic entries are **deterministic**: `gen_synthetic.py` renders from a
seeded procedural panorama with its own built-in 5x7 bitmap font, so the same
manifest gives byte-identical files on any machine. That is what makes pinning
their hashes meaningful, and it is why a PSNR measured here is comparable with a
PSNR measured on a CI runner.

Current synthetic corpus: **19 files, 46.8 MB** across 8 sequences.

---

## Content classes

The classes exist because the paper's claims are class-specific. A codec that is
2 dB better on textured terrain and 3 dB worse on text has not improved.

| Class | What it is | Why the paper needs it |
|---|---|---|
| `synthetic-vr` | stereo views rendered out of a procedural equirectangular panorama with a scripted head pose | geometrically exact for the rotation-only reprojection of PAPER 2.2, so a warp failure here is the codec's fault, not the material's |
| `head-rotation` | the same, with a pose log spanning a wide range of angular velocity | PAPER 2.11 item 1 splits BD-rate at the 20 % highest-velocity frames; that split needs frames on both sides of it in one sequence |
| `text-panel` | text at two scales, hard edges, saturated colour, static | the 4:2:0 chroma-fringe case; PAPER 5.2 locks text to lossless quad layers, and this is where that decision is checked |
| `stereo-pair` | side-by-side stereo with real near-field disparity | Phase 4 inter-view prediction (the `STEREO` tile mode) has nothing to predict from without it |
| `natural` | public camera-captured sequences | not VR material, but the sanity check every codec is expected to survive, and the only class with decades of published anchors to cross-check our BD-rate machinery against |
| `wivrn-capture` | real frames + pose logs from a WiVRn NX session | **the Phase 1 and Phase 2 gates are stated on VR captures, not on synthetic material.** Synthetic sequences prove the plumbing; these decide the phases |

The panorama the synthetic classes are rendered from deliberately contains
textured noise terrain, text panels at two scales, checkerboards at three
frequencies, a zone plate, a near-black gradient, saturated colour bars, a star
field, near-field moving discs and a head-locked HUD panel — the full list and
the reasoning are in `tools/quality/README.md` section 1a.

## Schema

`MANIFEST.json` is `{schema, description, default_root, root_env, classes,
entries[]}`.

| Field | Type | Meaning |
|---|---|---|
| `name` | string | unique; also the sequence base name the generator is given |
| `kind` | `synthetic` \| `external` \| `capture` | decides what `fetch.py` does with it |
| `class` | string | a key of the top-level `classes` object |
| `resolution` | `"WxH"` | the **coded** picture, so a 256-per-eye SBS sequence is `512x256` |
| `frames` | int | frame count (0 for a capture that does not exist yet) |
| `fps` | float | nominal rate; rate figures are `bytes * 8 / frames * fps` |
| `pix_fmt` | string[] | pixel formats materialised, e.g. `["yuv444p", "yuv420p"]` |
| `pose_log` | string \| null | per-frame pose JSON, relative to the corpus root |
| `source` | string | where the material came from, in words |
| `license` | string | the honest licence position, including "we are not sure" |
| `note` | string | why this entry is in the corpus at all |
| `generator` | object | `synthetic` only: the `gen_synthetic.py` arguments |
| `url_verified` | bool | `external` only: whether the URL was actually reached, and when |
| `files[]` | array | `{path, sha256, bytes}`, plus `url` for `external` |

`path` is relative to the corpus root. Hashes are SHA-256 of the file's bytes.
A `null` or empty `sha256` means "not pinned yet" — `verify.py` reports that as a
failure once the file is present, because an unpinned file in a corpus is a file
nobody can reproduce.

### Adding an entry

1. Add the object to `entries`, with empty `sha256` and `null` `bytes`.
2. `python3 corpus/fetch.py --sync --record --only <name>` — generates it and
   writes the hashes back.
3. `python3 corpus/verify.py --only <name>` — confirm.
4. Commit `MANIFEST.json`. **Never commit the data.**

### Changing an entry

Changing a `generator` block, or the generator itself, changes the bytes and
therefore the hashes. Re-record them, and put that change in **its own commit**:
it invalidates every quality number measured before it, and a reviewer needs to
see that as a single unambiguous event rather than buried in a feature diff.

---

## The three kinds

### `synthetic` — generated locally

`fetch.py --sync` drives `tools/quality/capture/gen_synthetic.py` (reading its
CLI; the generator is never modified) with the entry's `generator` block, under
the harness's own CPU discipline (`chrt -i 0 taskset -c … nice -n 19`, from
`nxq/cpu.py`).

| Entry | Class | Coded size | Frames | Formats | What it is for |
|---|---|---|---|---|---|
| `vr-static-256` | synthetic-vr | 512x256 | 8 | 444 + 420 | at-rest coding; the denominator for every "better on motion" claim |
| `vr-pan-256` | head-rotation | 512x256 | 12 | 444 | sustained panning, ~30 deg/s |
| `vr-turn-256` | head-rotation | 512x256 | 12 | 444 | a brisk turn, ~150 deg/s; where resampling blur (2.11 item 2) shows first |
| `vr-mixed-256` | head-rotation | 512x256 | 24 | 444 + 420 | **the Phase 2 kill-test profile**: rest, turn, rest |
| `vr-mixed-512` | head-rotation | 1024x512 | 12 | 420 | the same profile a resolution up, to catch resolution dependence |
| `panel-static-256` | text-panel | 512x256 | 6 | 444 + 420 | no objects, no HUD: text, checkers, colour bars, still camera |
| `stereo-near-256` | stereo-pair | 512x256 | 8 | 444 | 12 near-field discs, so the eyes carry real disparity |
| `mono-mixed-256` | synthetic-vr | 256x256 | 12 | 444 | one eye, a 4x4 tile grid — small enough that a per-tile table fits on a screen |

Each writes `<name>.<pix>.yuv` (headerless planar 8-bit), `<name>.<pix>.json`
(the harness sequence sidecar) and one shared `<name>.poses.json`.

`mono-mixed-256` is the one to reach for while developing a tile-level tool:
16 tiles, so `nxvc-example-tilewalk` prints a table you can read.

**Resolution is deliberately small.** These sequences prove that a measurement
pipeline is wired up correctly; they do not decide a phase. Nothing at 256 px per
eye produces the 100–400 Mbit band the Phase 1 gate is stated in, and
`compare.py` will correctly refuse to give a verdict rather than invent one. Go
to `--eye-width 2048 --full` when the answer matters, and put it on the scratch
volume.

### `external` — downloaded, never automatically

Public sequences, listed so a result can be cross-checked against material other
people have also measured. **Nothing is fetched without `--download`**, every
file is subject to `--max-mb` (default 64), and the size is checked with a HEAD
request before any body is transferred.

| Entry | Size | Source | Licence position |
|---|---|---|---|
| `xiph-foreman-cif` | 43.5 MB | Xiph.org `derf` collection | a long-standing free research sequence redistributed by Xiph; **no explicit licence file is published with it**. Use for evaluation; do not redistribute |
| `xiph-mobile-cif` | 43.5 MB | Xiph.org `derf` collection | same |
| `xiph-park-joy-1080p50` | 1.4 GB | SVT High Definition Multi Format set, redistributed by Xiph | Sveriges Television released the set for research use and retains copyright. **Not redistributable.** Far above the default cap, so `--download` refuses it until you raise `--max-mb` on purpose |

All three URLs were reached and their sizes confirmed on 2026-09-04
(`url_verified: true` in the manifest). Two honest caveats:

* **Their SHA-256 is not pinned**, because nothing large was downloaded to
  produce this manifest. The first person to fetch one should run
  `fetch.py --download --record` and commit the hash.
* **The licence column is a summary, not legal advice.** The `derf` collection
  is the de-facto standard research corpus and has been redistributed by Xiph
  for two decades, but the individual clips carry different terms and several
  carry none in writing. That is fine for measuring a codec and not fine for
  shipping the clips. If a published result depends on one of these, check its
  terms before publishing, not after.

They are also **not VR material**: 30 fps camera content in 4:2:0 at CIF, with no
pose log, no stereo, and no foveation. Their value is that a codec which does
something insane on `mobile_cif` is broken in a way no amount of VR-specific
material will reveal, and that the anchors have decades of published numbers.

`.y4m` needs one pass to raw planar before the harness will take it:

```sh
ffmpeg -i $NXW_CORPUS/external/foreman_cif.y4m -pix_fmt yuv420p \
       -f rawvideo $NXW_CORPUS/foreman_cif.yuv420p.yuv
```

then write a sidecar with `nxq.sequence.Sequence`, or use
`tools/quality/capture/import_media.py video`, which does both.

### `capture` — recorded by hand

Real WiVRn NX frames. **Nothing here has been recorded yet**, and the three
entries are registered as placeholders because PAPER 2.11 item 1 names them
explicitly: 60 s each of VRChat, Beat Saber and Alyx, as raw frames plus pose
logs. They are the material the parity kill test is run on.

The capture route exists today with no source changes to WiVRn — `encoder: "raw"`
plus `WIVRN_DUMP_VIDEO`, `WIVRN_DUMP_HEAD` and `WIVRN_DUMP_TIMINGS` — and
`python3 tools/quality/capture/wivrn_capture.py plan` prints the full recipe.
Read section 1c of `tools/quality/README.md` first: the dumped frame is
post-foveation and post-colour-conversion, and there is a specific configuration
(stream extent ≥ render extent, `render_scale` 1.0, `foveation_strength` 0,
adaptive foveation off, FSR1 off, motion smoothing off) that degenerates the
foveation pass to an identity. Frames captured without that configuration are
not comparable with anything.

Once recorded:

```sh
python3 tools/quality/capture/wivrn_capture.py convert --in dump-0.yuv \
    --w 1440 --h 1440 --out $NXW_CORPUS --name wivrn-vrchat-1440 --pix yuv420p,yuv444p
python3 tools/quality/capture/wivrn_capture.py poses --timings timings.csv \
    --head head.csv --out $NXW_CORPUS/wivrn-vrchat-1440.poses.json
python3 corpus/fetch.py --record --only wivrn-vrchat-1440
```

**These are private recordings of someone's session.** They are never committed,
never published, and never attached to a bug report. The manifest records their
existence and hashes so results are reproducible *by the person who has them*;
that is the most a private capture can offer.

---

## Using the corpus

With the examples:

```sh
export NXW_CORPUS=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/corpus
./build/bin/nxvc-example-roundtrip --in $NXW_CORPUS/vr-mixed-256.yuv444p.yuv \
    --w 512 --h 256 --pix yuv444p --qp 26
```

With the quality harness, which takes the sidecar rather than raw geometry:

```sh
cd tools/quality
python3 compare.py --seq $NXW_CORPUS/vr-mixed-256.yuv444p.json \
    --codec-cmd ../../build/bin/nxv --anchors x264-intra \
    --qp 16,22,28,34 --anchor-qp 16,22,28,34 \
    --out $NXQ_SCRATCH/results/mixed.json
```

The sidecar carries `pose_log`, which is what lets `compare.py` do the
angular-velocity split automatically. That is the whole reason the pose log is a
manifest field rather than an afterthought.

## Exit codes

`fetch.py` and `verify.py` both return **77** when nothing is materialised —
ctest's "skipped". A developer machine with no corpus is not a broken machine,
and a corpus check that goes red on a fresh clone teaches people to ignore red.
A hash *mismatch*, by contrast, is always **1**: the file is not what it claims
to be, and every number measured on it is suspect.
