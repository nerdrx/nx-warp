# Compositor-pose display

**Status:** measured proposal, desktop only. Nothing here changes the
bitstream, the atlas, or anything conformance compares. It is a claim about
where the *display* warp can be skipped, and a table saying how often.

## The idea

The Pico compositor re-warps every submitted layer to the latest pose. Today
the decoder's display pass warps each atlas entry from its own source pose to
the current pose, and the compositor then warps the result again. The second
warp is unavoidable — it is the compositor's job and it happens whether or not
we want it.

So the proposal is to stop fighting it: submit the composite stamped with a
**dominant pose** rather than the current one. Entries already at that pose are
not warped at all; only entries at some *other* pose are warped, and only as far
as the dominant pose. The compositor's own re-warp carries the whole layer the
rest of the way, for free.

## Why "the dominant pose" is exact, not approximate

An entry's pixels were captured at the pose of the frame that last coded it,
and [SYN] 13.12.3 step 1 advances **every** valid entry by the same per-frame
homography. Two entries with the same `src_frame` have therefore had the same
sequence of matrices composed into them, so they hold **bit-identical `C`**.

That makes "which entries share a pose" an integer grouping of `src_frame`, not
a floating-point comparison of matrices. The dominant pose is the mode of that
histogram. There is no tolerance to tune and no two implementations can
disagree about it — the same property 13.12.10.1 leans on for the rolling rank.

![dominant-pose share and warped-tile count](assets/atlasdec-dominant-pose.png)

![the display path](assets/atlasdec-display-path.png)

*Gallery figures 10 and 11.*

## (a), (b) The measurement

`nxv-posestats` (in `ref/tools/`) encodes a pose-consistent pan at five
rotation rates, decodes it with the reference, and histograms `src_frame` over
the valid entries after every frame. 1088x1088, 289 tiles, one eye, 60 frames,
QP 28, `D` = the 13.12.11.1 mode trigger. Averages are over frames 1..59; frame
0 is the intra frame and says nothing about steady state.

`warp:new` is the proposal — entries not at the dominant pose. `warp:today` is
what the display pass warps now — every entry the frame did not code.
`warp:PIC` is a PICTURE frame's assembly, for scale.

**D = 8** (the trigger as briefed):

| arm | deg/frame | at 90 Hz | PICTURE frames | poses | dominant share | warp:new | warp:today | warp:PIC |
|---|---|---|---|---|---|---|---|---|
| rest | 0.00 | 0 deg/s | 0 / 59 | 1.0 | **100.0 %** | **0.0** | 289.0 | 289 |
| creep | 0.10 | 9 deg/s | 0 / 59 | 3.0 | **61.7 %** | **110.8** | 167.5 | 289 |
| slow | 0.25 | 22.5 deg/s | 0 / 59 | 2.0 | 95.3 % | 13.4 | 13.4 | 289 |
| mid | 0.50 | 45 deg/s | 59 / 59 | 1.0 | 100.0 % | 0.0 | 0.0 | 289 |
| fast | 1.37 | 123 deg/s | 59 / 59 | 1.0 | 100.0 % | 0.0 | 0.0 | 289 |

**D = 64**, which keeps ATLAS frames through real motion so the mode switch
cannot mask the effect:

| arm | PICTURE frames | poses | dominant share | warp:new | warp:today |
|---|---|---|---|---|---|
| rest | 0 / 59 | 1.0 | 100.0 % | 0.0 | 289.0 |
| creep | 0 / 59 | 3.0 | 61.7 % | 110.8 | 167.5 |
| slow | 0 / 59 | 2.0 | 95.3 % | 13.4 | 13.4 |
| mid | 0 / 59 | 1.0 | 100.0 % | 0.0 | 0.0 |
| fast | 0 / 59 | 1.0 | 100.0 % | 0.0 | 0.0 |

The behaviour the proposal lives on, per frame, at `creep`
(`frame:dominant%/distinct poses`):

```
1:100%/1  2:60%/2   3:60%/2   4:55%/3   5:55%/3   6:89%/3  7:89%/3
8:56%/3   9:56%/4  10:54%/3  11:54%/3  12:86%/3  13:86%/3
```

It does not decay monotonically — it **oscillates**, between about 50 % and
about 89 %, on the encoder's refresh cycle, and over the whole run it spans
49 % to 100 % with one to four distinct poses live at a time. The dominant pose
is whichever cohort was refreshed together most recently; when the next cohort
lands it takes the majority and the share jumps back up.

That matters for the proposal in a way the average does not show: the *worst*
frames are around 50 %, so the display pass has to carry both paths at close to
equal load, and the win is a steady ~34 % rather than a mode it can specialise
for.

## What the numbers say

**The win is real but narrow, and it is at the SLOW end — the opposite of
where the atlas usually pays off.**

* **At rest the proposal removes the display warp entirely**: one pose, 100 %
  dominant, 0 warped tiles against 289. This is the whole of its value and it
  is a large one, because at rest is also where a headset is expected to hold a
  crisp picture indefinitely.
* **At creep (9 deg/s) it removes about a third**: 110.8 warped against 167.5,
  a 34 % reduction, because the dominant pose is an *older* pose that a
  majority of entries still sit at while a minority is being refreshed.
* **At `slow` and above it buys nothing**, and the reason is worth stating
  plainly: by then most tiles are being coded every frame, so the dominant pose
  *is* the current pose and the entries not at it are exactly the entries the
  frame did not code. Both schemes warp the same set. At 0.25 deg/frame that is
  13.4 tiles of 289 — already almost nothing.
* **At mid and fast the mode switch has already solved it.** At D = 8 every
  frame is a PICTURE frame, which puts every entry at one pose by construction;
  at D = 64, where ATLAS frames persist, the encoder recodes essentially every
  tile anyway. Either way there is nothing left to skip.

So this competes with the mode switch rather than composing with it. **A
PICTURE frame is the same idea paid for in the decoder**: it forces the
dominant share to 100 % by rebuilding the atlas, and the compositor-pose scheme
gets the same display saving without the assembly, but only while the atlas
happens to hold a majority at one pose.

## (c) What the display pass needs

Three things, all small, and none of them normative:

1. **One dominant pose per eye**, published beside the frame. The eyes are
   independent — the table is eye-minor and each eye's histogram is its own —
   so this is two poses per frame, not one. It is the pose the composite is
   *stamped* with, so it must be a pose the compositor can accept for a
   submitted layer.
2. **A per-entry "at dominant pose" flag.** It costs nothing to derive: it is
   `src_frame == dominant_src_frame`, an integer compare on a field the table
   already publishes, so it needs no new syntax and no new byte in the 64. The
   decoder can hand it over as a bitmap beside the table, or the client can
   compute it from the table it already has.
3. **Two paths in the pass**, chosen per tile: a straight sample for entries at
   the dominant pose, and the existing warp for the rest — to the dominant pose
   rather than to the current one. Since the warp is the same operation with a
   different target matrix (`C_tile . H(dominant <- entry source)`), this is a
   matrix change, not a second kernel.

The one-tap 8-bit view (`nxvc_vk_decoder_set_atlas_view`,
`NXVC_VKD_ATLAS_VIEW_R8`) is what the straight-sample path should read, and the
1.086 ms/pair it already measures is the floor this proposal is trying to
approach for the majority of tiles.

## (d) Exactness

**This is display-only and the normative atlas is untouched.** No pixel of the
atlas, no byte of the 64-byte table, and no decision the decoder makes about
either depends on any of this. 13.12.5's display warp is explicitly not
compared by conformance — that is the entire point of the atlas — so choosing a
different target pose for it cannot make a stream decode differently, cannot
change what a conformance vector compares, and cannot diverge two decoders.

What it *does* change is the picture a viewer sees, by moving one filtered warp
from the decoder to the compositor. That is a quality question, not a
correctness one, and it is the one thing this document does not answer: whether
the compositor's re-warp of an already-warped layer is visually equal to a
single warp to the current pose. It should be better — one fewer resampling of
the majority of tiles — but that is an argument, not a measurement.

## Caveats, and what is missing

* **Synthetic material.** A pose-consistent pan of a fixed multi-frequency
  scene, one QP, the reference encoder's own mode decisions. The first version
  of this tool generated a *new* scene every frame, which made every tile
  unskippable and reported a uniform 100 % dominant share at every rate — a
  number about the generator, not the codec. Real content will sit somewhere
  between that and this.
* **No device numbers.** Everything above is a tile COUNT, not a time. What the
  counts are worth in milliseconds needs the Pico, and the headset is on hold.
  The count is the right thing to decide on first: it is what determines
  whether there is anything to measure.
* **One eye.** The dominant pose is per eye and the two eyes are refreshed
  independently, so a stereo run may show a lower joint share. Worth a second
  pass if this goes forward.
* **The interesting regime is narrow.** Between "nothing is coded" and "almost
  everything is coded" there is a band — around 9 deg/s here — where the atlas
  holds a genuine mosaic. Whether a real head spends time there is the question
  that decides this proposal, and it is a question about head-motion traces,
  not about the codec.

## Verdict

**Worth building only if the at-rest case is worth it on its own.** At rest it
removes the display warp completely, which is a clean and large win in the
state a headset spends a lot of its time in. Everywhere else it is either a
third off a small number, or nothing, or already covered by the PICTURE-frame
mode switch. It is cheap — an integer histogram, a bitmap, and a different
matrix in a pass that already exists — so the cost side is small too; the
question is entirely whether the at-rest and creep cases justify a second code
path in the display pass.
