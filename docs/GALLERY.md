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

## Figure 10 — The atlas rarely holds one pose, except at rest

![Figure 10](assets/atlasdec-dominant-pose.png)

**Date** 2026-09-06 · **Fixture** synthetic pose-consistent pan, a fixed
multi-frequency scene sampled through the yaw (`nxv-posestats`) · **Settings**
1088x1088, 289 tiles, one eye, 4:2:0, QP 28, inter + atlas, `D=8`
(`atlas_picture_disp`), 60 frames, five rotation rates.

**The number it illustrates.** The dominant-pose share of
`docs/COMPOSITOR-POSE-DISPLAY.md`, and with it the whole case for the proposal.
**At rest the display pass warps 0 tiles against today's 289** — one pose, 100 %
dominant, the flat purple pair in the lower panel. At `creep` (0.10 deg/frame,
9 deg/s) the share **oscillates between 49 % and 89 %** on the encoder's refresh
cycle rather than decaying smoothly, averaging 61.7 %, and the warped count
falls from 167.5 to 110.8 — 34 %. At `slow` and above the two schemes coincide
(13.4 against 13.4), and at `mid` and `fast` the `D=8` trigger makes every frame
a PICTURE frame, which puts every entry at one pose by construction and leaves
nothing to skip. The upper panel's flat 100 % lines for `mid`/`fast` are the
mode switch doing the proposal's job already.

```
cmake --build build --target nxv-posestats
./build/bin/nxv-posestats --size 1088 1088 --frames 60 --disp 8 --csv > pose_d8.csv
python3 tools/quality/plot_pose.py --csv pose_d8.csv --out docs/assets --disp 8
```

---

## Figure 11 — The compositor's warp is not avoided, it is used

![Figure 11](assets/atlasdec-display-path.png)

**Date** 2026-09-06 · **Fixture** none — a diagram of the proposed display path
· **Settings** n/a.

**The number it illustrates.** The two paths Figure 10 counts, and which entry
takes which: entries at the dominant pose are sampled with **no warp at all**
(100 % of them at rest, about 62 % at 9 deg/s), the rest are warped to the
*dominant* pose rather than the current one, and the Pico compositor's own
re-warp — which happens whether or not anyone wants it — carries the composite
the remaining distance. It also carries the exactness claim the document makes:
the atlas, the 64-byte table and everything conformance compares are untouched,
because 13.12.5's display warp is not normative.

```
python3 tools/quality/plot_pose.py --csv pose_d8.csv --out docs/assets --disp 8
```

---

## The seam ratio, since every entry above quotes it

Mean `|x[i] - x[i-1]|` over sample pairs that straddle the 64-sample tile grid,
divided by the same over pairs inside tiles, on the decoded luma. **1.0 means a
tile edge looks like any other pair; above 1 the grid is visible.** It is
scale-free, so configurations at different bitrates can be compared directly,
and it is reported for every visual result from now on. `nx-scratch/atlasprice/seams.py`.
