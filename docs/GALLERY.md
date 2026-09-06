# Gallery

Every visual result in this repository, as a picture, with the command that
regenerates it.

**House rules.** One entry per figure. Each entry carries the date, the fixture,
the settings, *the number it illustrates*, and an exact regenerating command.
PNGs live in `docs/assets/<topic>-<what>.png`, cropped at 2-3x and kept under
about 500 kB; anything larger stays in `nx-scratch/` with a pointer from here.
These are the figures of a later paper, so they are captioned as figures and
numbered, and a figure that illustrates no number does not belong here.

Append; do not renumber. If a measurement is superseded, add the new figure and
leave the old one with a note saying what replaced it.

---

## Figure 1 — Rate-distortion on the vrroom corpus

![Figure 1](assets/vrroom-rd.png)

**Date** 2026-09-06 · **Fixture** `nx-scratch/fixtures/vrroom` (all four
trajectories) · **Settings** `--atlas on --row-present on`, `--eyes 2`, QP 26 /
34 / 40, effort 0, planar rd; modes `all-ATLAS` (`--atlas-picture-disp 0`),
`all-PICTURE` (`--atlas-picture-period 1`), `D=8`
(`--atlas-picture-disp 8`).

**The number it illustrates.** The dominance results of ADR-0029's "Re-run on
rendered content" table: at QP 26 the atlas wins at rest (38.94 dB / 1643 B/f
against 38.02 / 4208) and under object motion (38.79 / 3510 against 37.97 /
5668), and loses at mid (36.62 / 9780 against 38.03 / 7031) and fast (32.65 /
13843 against 38.21 / 6566). The `D=8` curve sits on whichever of the two wins
at each velocity without being told which, and at fast turn it lies exactly on
`all-PICTURE` because it spends 100 % of frames there.

```
python3 tools/quality/capture/gen_vrroom.py --out nx-scratch/fixtures/vrroom
python3 nx-scratch/atlasprice/vrtable.py rest      # and mid, fast, objmotion
python3 tools/quality/plot_vrroom.py --in nx-scratch/atlasprice/work5 --out docs/assets
```

---

## Figure 2 — The effort ladder and the planar mode do not pay

![Figure 2](assets/vrroom-effort.png)

**Date** 2026-09-06 · **Fixture** vrroom, all four · **Settings** `D=8`, QP 34,
`--int-rdoq 0|1`, `--rdoq-effort 3`, `--intra-dir on|layer`.

**The number it illustrates.** `int_rdoq` 0, `int_rdoq` 1 and the full trellis
land within **0.03 dB** of each other on all four fixtures — the three
left-hand columns sit on the zero line. `--intra-dir layer` ("planar prefer")
costs **0.12 to 0.27 dB** *and* more bytes on every fixture, which is a
dominance result in the wrong direction. Plotted as a delta from effort 0 /
planar rd because absolute bars would hide a 0.03 dB spread inside the axis.

```
python3 tools/quality/plot_vrroom.py --in nx-scratch/atlasprice/work5 --out docs/assets
```

---

## Figures 3-6 — The vrroom corpus, one frame per trajectory

| | |
|---|---|
| ![Figure 3](assets/vrroom-rest.png) **Fig 3** rest, 2.7 deg/s | ![Figure 4](assets/vrroom-mid.png) **Fig 4** mid, 26.2 deg/s |
| ![Figure 5](assets/vrroom-fast.png) **Fig 5** fast, 99.1 deg/s | ![Figure 6](assets/vrroom-objmotion.png) **Fig 6** object motion, head at rest |

**Date** 2026-09-06 · **Fixture** `nx-scratch/fixtures/vrroom`, frame 8, left
eye, 544x408 crop at 2x · **Settings** Blender 5.2 EEVEE, `Standard` view
transform, stereo 63 mm, 1088x1088 an eye, 100 degrees, 90 Hz, 32 frames.

**The number it illustrates.** Not a number — the *content*, which is the
premise of every measurement above it. `gen_synthetic.py` produced a
band-limited procedural panorama with no specular, no thin geometry, no text
and no independently moving objects, and docs/LOWPOLY-MODE.md called it
"unusually kind". These four frames are what replaced it: text at reading
distance, a mirror-like specular floor, thin high-contrast bars, checkerboard
walls, two textured humanoid meshes that move on their own. The measured
angular velocities are 2.7 / 26.2 / 99.1 deg/s, the fast clip peaking at
796 deg/s across its one-frame 8-degree step.

Full-resolution frames (627-646 kB each) are at
`nx-scratch/fixtures/vrroom/<track>-frame8.png`.

```
python3 tools/quality/capture/gen_vrroom.py --out nx-scratch/fixtures/vrroom
ffmpeg -f rawvideo -pix_fmt yuv420p -s 2176x1088 -i nx-scratch/fixtures/vrroom/mid.yuv420p.yuv \
  -vf "select=eq(n\,8),crop=544:408:180:400,scale=1088:816:flags=neighbor" \
  -frames:v 1 docs/assets/vrroom-mid.png -y
```

---

## Figures 7-9 — Coarse landings are blocky, not soft

| | | |
|---|---|---|
| ![Figure 7](assets/coarse-seam-baseline.png) **Fig 7** baseline, seam 1.14 | ![Figure 8](assets/coarse-seam-T8.png) **Fig 8** T=8, seam **1.53** | ![Figure 9](assets/coarse-seam-T2.png) **Fig 9** T=2, seam **1.81** |

**Date** 2026-09-06 · **Fixture** `nx-scratch/atlasenc/turn` (fast turn), frame
12, 256x256 crop at 2x, nearest-neighbour · **Settings** `--atlas on
--atlas-picture-disp 8`, QP 26, `--atlas-coarse-disp 0 | 8 | 2`.

**The number it illustrates.** The seam ratio of ADR-0029's single-level coarse
refresh section: **1.14 → 1.53 → 1.81** against a source of 0.93, while
high-frequency energy *falls* 3.04 → 2.76 → 2.61. Both moving at once is the
failure mode: the tiles get softer inside while the 64-sample grid becomes
visible, which is blocking rather than the low-poly degradation the project
asks for. Visible in the figures as sphere silhouettes that are round at
baseline and acquire straight, tile-aligned cuts by T=2.

```
B=build/bin; T=nx-scratch/atlasenc/turn/turn
$B/nxv-enc --in $T.yuv420p.yuv --w 1088 --h 1088 --pix yuv420p --qp 26 \
  --poses $T.poses.json --inter on --threads 1 --atlas on \
  --atlas-picture-disp 8 --atlas-coarse-disp 8 --out T8.nxv
$B/nxv-dec --in T8.nxv --out T8.yuv
ffmpeg -f rawvideo -pix_fmt yuv420p -s 1088x1088 -i T8.yuv \
  -vf "select=eq(n\,12),crop=256:256:384:384,scale=512:512:flags=neighbor" \
  -frames:v 1 docs/assets/coarse-seam-T8.png -y
python3 nx-scratch/atlasprice/seams.py 12 source=$T.yuv420p.yuv baseline=T0.yuv coarse=T8.yuv
```

---

## Figures 10-11 — The seated trajectories, and the true rest floor

| | |
|---|---|
| ![Figure 10](assets/vrroom-still.png) **Fig 10** still, 0.043 deg/s | ![Figure 11](assets/vrroom-objmotion-still.png) **Fig 11** objmotion-still |

**Date** 2026-09-06 · **Fixture** `nx-scratch/fixtures/vrroom`, frame 8, left
eye, 544x408 crop at 2x · **Settings** as Figures 3-6.

**The number it illustrates.** `rest` was named for a head at rest and is not
one: it moves the atlas's tile corners **0.97 samples in one frame and 3.83
over four**, so a whole-sample identity is never available in it. `still` is a
seated head — 0.030 deg of postural drift at 0.11 Hz plus 0.001 deg of tremor
at 8 Hz — and measures **0.043 deg/s** with corner displacement of **0.016
samples** after a frame and 0.016 after thirty, sixty times smaller.

On it the atlas does exactly what it is for: **100 % skip, zero decoder warps,
150 B/frame at 41.28 dB** for a stereo 1088x1088 pair. At QP 34 and 40 it
converges to the structural floor of **119 B/frame** — 85.7 kbit/s at 90 Hz —
which is frame header plus `warp_ext()` plus the `row_present` bitmap and
nothing else. `objmotion-still` holds the head there and walks the meshes:
2800 B/f, which prices independent object motion at about **2650 B/frame** on
its own.

The high seam ratios in this row (3.27 at QP 26, 6.88 at QP 40 — the **QP**
axis, not the time axis) are inherited from the single intra frame: at
119 B/frame nothing is ever re-coded, so what is displayed is frame 0's
quantisation warped forward, and at QP 40 that frame is blocky. It is a real
artefact, and it is the cost of a floor this low. Figures 12-13 measure it per
frame and show it is flat, that the atlas is not the mechanism, and that
re-coding to remove it costs up to 7x the bytes and makes it worse.

On the ADR-0028 integer decision the same clip is **125 B/frame at the same
41.28 dB** — 25 bytes cheaper, because that path has no `NEAR_SKIP` to spend
and so lands six bytes above the structural floor rather than thirty. The full
per-trajectory mode histogram both decisions produce is in
[ENCODER-DECISION.md](ENCODER-DECISION.md) section 7.

```
python3 tools/quality/capture/gen_vrroom.py --out nx-scratch/fixtures/vrroom   --tracks still,objmotion-still
python3 nx-scratch/atlasprice/vrtable.py still
python3 nx-scratch/atlasprice/encdec_still.py       # float decision
python3 nx-scratch/atlasprice/encdec_still_int.py   # integer decision
```

---

## Figures 12-13 — The still clip's seams are frame 0's, and nothing adds to them

| | |
|---|---|
| ![Figure 12](assets/still-seam-f1.png) **Fig 12** frame 1, seam **6.925** | ![Figure 13](assets/still-seam-f31.png) **Fig 13** frame 31, seam **6.820** |

**Date** 2026-09-06 · **Fixture** `nx-scratch/fixtures/vrroom`, `still`, left
eye, 256x256 crop at 384,384 (tile-aligned) at 2x, nearest-neighbour ·
**Settings** `--atlas on --row-present on --atlas-picture-disp 8`, QP 40.

**The number it illustrates.** Whether the `still` row's high seam ratio is
something the atlas *does* over the clip. It is not. Measured per frame, on a
clip that codes 119 B/frame and re-codes nothing:

| | frame 0 | frame 1 | frame 31 | min | max |
|---|---|---|---|---|---|
| QP 26 | 3.262 | 3.262 | 3.270 | 3.262 | 3.277 |
| QP 40 | 6.925 | 6.925 | **6.820** | 6.820 | 6.925 |

It is **flat, and at QP 40 it falls**. The `3.27 -> 6.88` quoted in Figures
10-11 is the QP axis — QP 26 against QP 40 — not the time axis. The whole
value is present at **frame 0**, which is the all-intra frame: before any warp,
before an atlas entry exists. Frames 1 and 31 differ in 2.1 % of their samples
by a mean of 0.035 and a maximum of 11 (5.4 % and 0.089 inside this crop),
which is why the two figures look identical, because they nearly are.

**The atlas is not the mechanism.** With `--atlas off` — the plain picture
codec, no atlas at any point — the trace is 3.262 flat and 6.925 flat, the same
numbers. On a synthetic exactly-zero-motion clip (`still` frame 0 repeated 32
times at one fixed pose) the atlas reproduces **3.262 for all 32 frames**,
identical to the picture model: the per-tile advance of 13.12.3 is exact, and
neighbouring entries do not drift apart. The atlas's entire contribution is the
+0.5 % wobble visible in the min/max above, from sub-sample corner rounding.

**It is the format, and deliberately.** `docs/SYNTAX.md` states there is no
deblocking filter and no loop filter; a tile-boundary step from intra
quantisation is therefore structural. What removes it is re-coding under
motion: on `rest` the boundary gradient collapses **3.452 -> 1.756** across the
clip while the interior gradient barely moves (1.166 -> 1.044), and the atlas
erases seams *harder* than the picture model does (seam 1.682 against 2.508 at
frame 31). The still clip is not growing seams; the moving clips are erasing
theirs.

```
python3 nx-scratch/atlasprice/seamframe.py still 26 40   # seam ratio per frame
python3 nx-scratch/atlasprice/seamdiag.py                # split by config, + the zero clip
B=build/bin; W=nx-scratch/atlasprice/work9
ffmpeg -f rawvideo -pix_fmt yuv420p -s 2176x1088 -i $W/still.q40.yuv \
  -vf "select=eq(n\,31),crop=256:256:384:384,scale=512:512:flags=neighbor" \
  -frames:v 1 docs/assets/still-seam-f31.png -y
```

---

## The seam ratio, since every entry above quotes it

Mean `|x[i] - x[i-1]|` over sample pairs that straddle the 64-sample tile grid,
divided by the same over pairs inside tiles, on the decoded luma. **1.0 means a
tile edge looks like any other pair; above 1 the grid is visible.** It is
scale-free, so configurations at different bitrates can be compared directly,
and it is reported for every visual result from now on. `nx-scratch/atlasprice/seams.py`.
