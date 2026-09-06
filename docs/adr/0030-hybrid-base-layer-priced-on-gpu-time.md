# ADR-0030: The HEVC base layer buys GPU time, not bytes, and the bytes are why it still loses

- **Status**: Proposed
- **Date**: 2026-09-06
- **Source**: paper 2.9, 6.10; ADR-0014, ADR-0022, ADR-0027, ADR-0029 (cheat 7 and sweep (d))
- **Affects**: `docs/`, `hybrid/` (unbuilt), `vk/decoder/` (unchanged)

## Context

### The hybrid has been priced three times, and every price was in bytes

ADR-0022 measured the base layer as a quality tool at matched total bitrate and found it
dominated. ADR-0027 measured a spatial split and found the periphery 8 to 9 dB down.
ADR-0029's sweep (d), "BASE-LAYER REFRESH", measured it as a refresh policy against the atlas
at equal Wi-Fi bytes and found that **it loses at every velocity**: below the plain atlas at
25 deg/s, 7.5 dB behind the picture model at fast turn, and at rest paying 509 to 1944
B/frame for a base layer from which zero patches were ever applied.

All three answer "does this buy quality per byte". None of them answers the question ADR-0029
itself raised two pages earlier, in cheat 7.

### The axis nobody priced it on

The Pico 4's GPU is the scarce resource, not its radio. ADR-0029's own headline measurement,
at 1088x1088 per eye and 17x17 = 289 tiles:

| stage | measured |
|---|---|
| normative integer warp of one `WARP_SKIP` tile | **34 us** |
| `WARP_SKIP` tiles on a typical frame | **~250 of 289** |
| skip warp, per eye per frame | **8.8 ms** |
| an nxvc coded tile (Pass A + Pass B) | **~41 us** |
| **a base-sourced atlas patch, per 64x64 tile** | **1.9 us** |

The skip warp is most of the decoder, it does not amortise, and it gets *worse* as the codec
gets better: a stream that codes almost nothing is still ~250 tiles an eye that must each be
warped. The base-sourced patch of SYNTAX 13.12.9 replaces that 34 us with 1.9 us, and it runs
on a unit that is otherwise idle -- the hardware HEVC decoder, measured byte-identical with
FFmpeg over 180 frames at 4.7 / 9.6 / 50 Mbit, sustaining 721 fps over two eyes at **zero
Adreno cost**, because it is an ASIC.

The latency objection has also been withdrawn by measurement: MediaCodec at 9.6 Mbit is
**2.76 ms mean, 5.63 ms p99** with `vendor.qti-ext-dec-low-latency.enable`, not the 8 to 20 ms
PAPER 2.9 carried.

So there is a real prize here and this ADR is the first attempt to weigh it. The question is
narrow: **at equal displayed quality, how many Adreno microseconds does a base layer buy, and
what does it cost on the link to buy them.**

**One thing has to be checked before the prize is believed, and the answer turns out to be the
whole result.** The 34 us figure above is the *pre-atlas* decoder, which reconstructed a picture
every frame. ADR-0029 changed that: under the atlas a skipped tile writes no reference pixels at
all, and only a PICTURE frame (13.12.11) reconstructs every tile. So the question "how much skip
warp is there left to replace" is really "how many PICTURE frames are there", and that is a
property of the trajectory, not of the codec. The measurement below reports it per frame,
because without it every GPU number in this ADR would be an over-count.

## Method

Six vrroom trajectories (`nx-scratch/fixtures/vrroom`), 2176x1088 side by side, 1088x1088 per
eye, 32 frames each at 90 Hz, 34x17 = 578 tiles over the pair.

**Today.** `nxv-enc --inter on --atlas on --row-present on --eyes 2` at QP 22/26/30/34/38, giving
a rate-distortion curve per trajectory plus, from `nxv-info --tiles`, the per-frame count of
coded and `WARP_SKIP` tiles.

**The hybrid.** A libx265 base at 544x544 and 1088x1088 per eye at 10 / 15 / 25 Mbit/s,
**zero latency** -- `bframes=0 b-adapt=0 rc-lookahead=0 lookahead-slices=0 scenecut=0`, a VBV
held at the target. This is not a tuning preference. A base layer feeding a 90 Hz headset
cannot reorder frames or look 20 frames ahead, and x265's defaults do both; left on, they make
the base several dB better than anything shippable, which is precisely the direction that would
have made this ADR recommend the wrong thing.

The base is decoded, upsampled back to full size **bilinearly** (a sampler is what the display
pass has, not Lanczos), and compared to the source per 64x64 tile. A tile whose luma MSE against
the upsampled base exceeds `tau` gets an nxvc detail patch; the rest are base-sourced. Two
thresholds, at the MSE of 40 dB (`tau = 6.5`) and 34 dB (`tau = 26`) of local PSNR.

The patch stream is a real bitstream: the same encoder with `--skip-map`, forcing `WARP_SKIP`
on every tile outside the patch set. The hybrid picture is then composited -- upsampled base
everywhere, nxvc where it was patched -- and PSNR-Y measured against the source.

**Two conventions, both stated because both flatter the hybrid.**

* The patch term is the summed tile *payload*, not the file. A real hybrid sends the patch
  tiles and nothing else.
* Those patch tiles predict from nxvc's own warped history, which is a *better* predictor than
  an upsampled low-res base. A real base-predicted patch would cost more, not less.

**The Adreno proxy.** Per frame, over the 578 tiles of the pair:

    today   coded x 41 us  +  skipped x 34 us, but only in a PICTURE frame
    hybrid  coded x 41 us  +  (578 - coded) x 1.9 us

`picture_frame` is frame flags bit 5 (`docs/SYNTAX.md` 275) and is read per frame out of
`nxv-info`. A skipped tile in an ATLAS frame is charged nothing: it writes no reference pixels,
and its cost is the matrix compose the bench puts at 0.0048 ms for the whole picture. Charging
every skip at 34 us -- which the first pass of this measurement did -- inflates today's cost
about twenty-fold at rest and makes the hybrid look like a 91 % saving where the true figure is
negative.

These are ADR-0029's own per-tile figures and they are used only as a *ratio*, so the 1.57x
timestamp-period inflation `docs/ATLAS-DECODER.md` warns about divides out. The proxy over-states
absolute time: at fast turn it reads 20.0 ms a frame against the 16.69 ms
`docs/CLIENT-DECODE-WALL.md` measured for a whole live decode, which is the inflation plus the
fact that not every skip pays the full kernel. It is a proxy for decode-side work and it
deliberately excludes the display pass, which both schemes pay and which is priced separately
below.

## Results

### The headline: the 544-per-eye base at 10 Mbit, `tau` = 34 dB

The honest hybrid proposition -- a genuinely *low-res* base with nxvc patching what it gets
wrong. The nxvc-only column is interpolated on that trajectory's own rate-distortion curve at
the hybrid's PSNR, so this is an equal-quality comparison.

| trajectory | PICTURE frames | hybrid PSNR | hybrid B/f | nxvc-only B/f at that PSNR | link | Adreno today | Adreno hybrid | GPU |
|---|---|---|---|---|---|---|---|---|
| still | 0 % | 40.89 | 13688 | 1300 | **10.5x** | 0.74 ms | 1.80 ms | **-144 %** |
| rest | 6 % | 39.12 | 14315 | 3043 | **4.7x** | 2.25 ms | 2.02 ms | **+10 %** |
| objmotion-still | 0 % | 40.41 | 15244 | 3977 | **3.8x** | 1.88 ms | 2.58 ms | **-37 %** |
| objmotion | 6 % | 39.14 | 15707 | 5207 | **3.0x** | 3.31 ms | 2.79 ms | **+16 %** |
| mid | 47 % | 38.91 | 20484 | 10557 | **1.9x** | 11.43 ms | 4.61 ms | **+60 %** |
| fast | 97 % | 36.15 | 23596 | 4815 | **4.9x** | 19.99 ms | 5.12 ms | **+74 %** |

**One column explains the whole table, and it is not a byte column.** The GPU saving tracks the
PICTURE-frame share and nothing else. That is not a coincidence, it is the mechanism: under the
atlas a `WARP_SKIP` tile in an ATLAS frame writes no reference pixels and costs a matrix
compose -- the bench puts the whole compose at 0.0048 ms -- while in a PICTURE frame every tile
is reconstructed and pays the full 34 us warp. **ADR-0029 already spent the skip warp.** What is
left to save is the PICTURE frames, and the base layer saves them in proportion to how many
there are: nothing at rest, 60 % at 25 deg/s, 74 % at fast turn.

Where there are no PICTURE frames the base layer is **negative**: at `objmotion-still` it costs
37 % more Adreno than doing nothing, because patching 540 tiles at 1.9 us is more work than
leaving them resident in the atlas at ~0. A tool that replaces a free operation with a cheap one
is not an optimisation.

### The link, plainly

**It is 1.9x to 10.5x the bytes, at equal PSNR, on every trajectory.** The cheapest base in the
sweep -- 544 per eye at 10 Mbit -- is **12,666 B/frame on its own**, which is already 6.9x the
entire nxvc stream at rest and 2.4x it at `objmotion`. That floor is paid every frame whether
one tile takes a patch or none does, which is the same thing ADR-0029's sweep (d) found and the
reason it found it.

The two velocities where the GPU prize exists are the two where the link is already carrying the
most: at `fast` the hybrid asks for 23.6 kB/frame against nxvc-only's 4.8 kB at the same quality.
At 90 Hz that is **17 Mbit/s against 3.5**.

### The full-resolution arm, and a warning about the corpus

At 1088 per eye -- which is not a hybrid, it is "send HEVC instead" -- the base layer alone beats
nxvc-only on **every** trajectory by 4 to 5 dB, and on `mid` and `fast` it does so at **fewer
bytes** (12,666 against 15,889 and 14,493).

That number must not be read as a recommendation, and ADR-0029 wrote the reason down before this
ADR existed: **these fixtures are synthetic, bandlimited and unusually cheap for HEVC** -- CRF 26
costs 1795 B/frame here against 5915 B/frame on real content in the hybrid gate. They are also
32-frame clips at `keyint 32`, so HEVC amortises exactly one I-frame over the whole measurement
while nxvc's `D = 8` policy codes a PICTURE frame in 97 % of frames at fast turn. PSNR-Y on
bandlimited synthetic geometry is the best case a mature RDO will ever have. ADR-0022 already
settled that plain HEVC wins a matched-bitrate PSNR contest on content like this; nothing here
disturbs that, and nothing here is evidence about real content.

### The latency

The hybrid adds a hardware decode to the motion-to-photon path: **2.76 ms mean, 5.63 ms p99**
measured on the Pico 4 at 9.6 Mbit with the low-latency vendor key. That is small, and it is
three to seven times better than the 8 to 20 ms this project has been carrying since PAPER 2.9 --
but it lands at exactly the velocities where the GPU prize exists, which are exactly the
velocities where a late frame is most visible.

### The display pass

The base has to be upsampled and composited, and that is a *display*-pass cost the proxy above
excludes. `docs/CLIENT-REPROJECTION.md` measures the display pass at **6.37 ms a frame for 2.37
Mpx** in session (278 ms/s, 2.69 ns/px) and, in isolation, 2.7 ms at 1080x1080 per eye against
8.4 ms at 2160x2160 -- about 1 ns/px. One extra bilinear tap from a small, cache-friendly 544x544
texture plus a select is a fraction of a pass that already runs a 12-tap FSR kernel; **0.2 to 0.5
ns/px, i.e. 0.5 to 1.2 ms a frame, 20 to 50 ms/s**, is the honest bracket. It does not change the
sign of anything above: at fast turn the decode-side saving is ~15 ms a frame.

![The 544-per-eye HEVC base layer, priced on headset GPU time instead of bytes](../assets/hybrid-gpu-time.png)

## Decision

**The HEVC base layer is not adopted, and the reason it is not adopted has changed.**

It is no longer "it does not buy quality per byte" (ADR-0022, ADR-0027) nor "it loses as a
refresh policy at equal bytes" (ADR-0029 (d)). Those remain true. The new reason is sharper and
it is the one that would have to be answered:

> The base layer's GPU prize exists only in PICTURE frames, and PICTURE frames happen only
> during fast motion, which is when the link is fullest and when added latency is most visible.
> It trades the one scarce resource for two others that are scarce at the same instant.

Three things are recorded as decided rather than left implicit.

1. **The skip warp is no longer the target; the PICTURE frame is.** ADR-0029 spent the skip warp,
   and this measurement is the confirmation: at 0 % PICTURE frames the whole decode-side proxy is
   **0.74 ms a frame**, and no base layer can improve on that. Any future proposal aimed at the
   34 us warp must first state its PICTURE-frame share, or it is optimising a term that is
   already zero.

2. **`base_sourced` (flags bit 2, SYNTAX 13.12.9) stays normative and stays unbuilt.** The syntax
   is correct, the Pico 4 measurements behind it are correct, and the 1.9 us patch is real. What
   is missing is a session in which it pays, not a mechanism.

3. **The adoption condition is now a number.** The base layer becomes the right answer for a
   session that spends **more than about 40 % of its frames as PICTURE frames** *and* has link
   headroom of roughly **2x** at its operating quality. On Wi-Fi at 1088 per eye, no trajectory
   in this corpus satisfies both. A tethered or 60 GHz link satisfies the second trivially, and
   that is the configuration in which this ADR should be re-opened.

The 8 to 20 ms hardware-decode latency figure in PAPER 2.9, ADR-0014 and `docs/ERRATA.md` is
**wrong for this decoder at this bitrate** and is superseded by 2.76 ms mean / 5.63 ms p99. That
correction stands whatever happens to the rest of this ADR.

## Consequences

* **Nothing is built.** No encoder work, no decoder work, no syntax change. `hybrid/` stays
  unpopulated and the `base_sourced` clause stays as ADR-0029 wrote it.
* **A cheaper PICTURE frame is now the highest-value open item on the decode side**, and it is
  worth more than the base layer was: at fast turn it is 20.0 ms a frame of Adreno against a
  budget already measured at 92 % occupied. Two directions the sweep did not price -- a partial
  PICTURE frame that rebuilds only the tiles whose displacement demanded it, and a `D` that
  adapts to the measured GPU budget rather than to displacement alone -- are the follow-ups.
* **The corpus needs real content before any of these numbers decide anything at 1088 per eye.**
  The full-resolution arm of this measurement says plain HEVC beats nxvc by 4 to 5 dB on this
  fixture set. That is a statement about the fixtures. Until there is a rendered-and-captured
  corpus with real texture detail, no ADR should quote a PSNR contest against HEVC at full
  resolution as evidence either way.
* **What must be measured to find out whether this was right:** the PICTURE-frame share of a real
  session on a real headset, logged per two-second window next to the decode duty. If that share
  turns out to sit above 40 % in ordinary use -- rather than only on a 99 deg/s synthetic turn --
  the trade in this ADR is worth re-running against a live link.

## Alternatives considered

**Adopt it motion-gated: base layer on above some angular velocity.** Rejected. It has the
correct shape -- the prize really does appear with the PICTURE frames -- but the gate would turn
the base layer on precisely when the link is fullest, and it must turn it on *before* the motion
to be useful, because the base is a whole-picture stream whose first frame is an I-frame. A
predictor that gets it wrong pays 12.7 kB/frame for nothing, which is ADR-0029 (d)'s "zero
patches applied on any frame" failure with a worse trigger.

**Adopt it at a lower base bitrate.** Rejected on the floor, not on the quality. 10 Mbit/s is
already the bottom of the range this ADR was asked to sweep and it is 12,666 B/frame -- 6.9x the
whole nxvc stream at rest. The base's cost is a floor paid every frame, so lowering it lowers
quality without removing the term that makes the comparison lose.

**Use the base only to service PICTURE frames, keeping ATLAS frames pure nxvc.** Not rejected --
**untested, and the most promising variant.** It removes the negative results (`still`,
`objmotion-still`) by construction, because it spends nothing when there are no PICTURE frames.
It was not measured here because it needs an encoder that can emit a base layer intermittently
and a client that can tolerate the base's own GOP structure being cut, neither of which exists.
It is the shape any future attempt should take.

**Keep charging the base's bytes as free because they travel to a different decoder.** Rejected,
for the reason ADR-0029 (d) already gave: they are bytes on the same link.

**Blame the atlas configuration.** Considered and checked. The first pass of this measurement ran
`--atlas on` with no PICTURE-frame trigger, which is the configuration ADR-0029 measures as
losing 5.9 dB at fast turn; comparing a hybrid against that would have been a strawman and would
have made the hybrid look 6.5 dB better than it is. The tables above are all against
`--atlas-picture-disp 8`, ADR-0029's recommendation.

## References

- ADR-0014 — one layered bitstream serves pure compute and hybrid decode
- ADR-0022 — hybrid mode is not a quality tool
- ADR-0027 — no spatial hybrid
- ADR-0029 — the per-tile atlas; cheat 7 (the idle HEVC ASIC, the 1.9 us patch, the 2.76 ms
  decode) and sweep (d) (base-layer refresh at equal Wi-Fi bytes)
- `docs/SYNTAX.md` 13.12.9 (`base_sourced`), 13.12.11 (PICTURE frames), 275 (frame flags bit 5)
- `docs/ATLAS-DECODER.md` — the 1.57x timestamp-period inflation these per-tile figures inherit
- WiVRn NX `docs/CLIENT-DECODE-WALL.md` and `docs/CLIENT-REPROJECTION.md` (branch
  `nx-warp-reproject`) — the 92 % GPU duty, the 8.25 ms skip term, the display pass at 6.37 ms
- PAPER 2.9, 6.10; `docs/ERRATA.md` — the superseded 8-20 ms decode-latency figure

## Appendix: the full sweep

Regenerate with the commands in docs/GALLERY.md. Section 2 is every one of the 72 hybrid
operating points; `patch B/f` is the summed tile payload, `link` is the ratio against the
nxvc-only curve interpolated at the hybrid's own PSNR.

### 1. The nxvc-only curve (today)

| trajectory | QP | B/frame | PSNR-Y | coded tiles/f | WARP_SKIP tiles/f | PICTURE frames | Adreno proxy ms/f |
|---|---|---|---|---|---|---|---|
| still | 22 | 1848 | 44.20 | 18 | 560 | 0 % | 0.74 |
| still | 26 | 1341 | 41.28 | 18 | 560 | 0 % | 0.74 |
| still | 30 | 1037 | 38.04 | 18 | 560 | 0 % | 0.74 |
| still | 34 | 827 | 35.32 | 18 | 560 | 0 % | 0.74 |
| still | 38 | 651 | 32.44 | 18 | 560 | 0 % | 0.74 |
| rest | 22 | 5651 | 41.29 | 34 | 544 | 6 % | 2.53 |
| rest | 26 | 2927 | 38.98 | 27 | 551 | 6 % | 2.25 |
| rest | 30 | 1672 | 36.43 | 21 | 557 | 6 % | 2.08 |
| rest | 34 | 920 | 34.16 | 18 | 560 | 6 % | 1.98 |
| rest | 38 | 647 | 32.05 | 18 | 560 | 6 % | 1.97 |
| objmotion-still | 22 | 6582 | 43.36 | 53 | 525 | 0 % | 2.18 |
| objmotion-still | 26 | 3906 | 40.30 | 46 | 532 | 0 % | 1.88 |
| objmotion-still | 30 | 2518 | 37.21 | 39 | 539 | 0 % | 1.58 |
| objmotion-still | 34 | 1621 | 34.22 | 28 | 550 | 0 % | 1.16 |
| objmotion-still | 38 | 974 | 31.24 | 22 | 556 | 0 % | 0.88 |
| objmotion | 22 | 9377 | 41.47 | 67 | 511 | 6 % | 3.82 |
| objmotion | 26 | 4781 | 38.80 | 53 | 525 | 6 % | 3.31 |
| objmotion | 30 | 2734 | 36.05 | 41 | 537 | 6 % | 2.88 |
| objmotion | 34 | 1619 | 33.53 | 29 | 549 | 6 % | 2.41 |
| objmotion | 38 | 935 | 30.99 | 21 | 557 | 6 % | 2.09 |
| mid | 22 | 15889 | 40.85 | 113 | 465 | 47 % | 12.27 |
| mid | 26 | 8329 | 37.79 | 79 | 499 | 47 % | 11.43 |
| mid | 30 | 4492 | 34.95 | 57 | 521 | 47 % | 10.85 |
| mid | 34 | 2506 | 32.10 | 42 | 536 | 47 % | 10.44 |
| mid | 38 | 1288 | 29.05 | 26 | 552 | 47 % | 10.12 |
| fast | 22 | 14493 | 41.13 | 93 | 485 | 97 % | 20.30 |
| fast | 26 | 7551 | 38.28 | 69 | 509 | 97 % | 20.13 |
| fast | 30 | 4069 | 35.35 | 48 | 530 | 97 % | 19.99 |
| fast | 34 | 2292 | 32.52 | 34 | 544 | 97 % | 19.89 |
| fast | 38 | 1249 | 29.66 | 24 | 554 | 97 % | 19.82 |

### 2. Every hybrid operating point

| traj | base/eye | Mbit | tau | base B/f | patch B/f | total B/f | base PSNR | hybrid PSNR | patch set /f | nxvc coded /f | nxvc-only B/f at equal PSNR | link cost | nxvc-only PSNR at equal bytes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| still | 544 | 10 | 40dB | 12666 | 1022 | 13688 | 35.81 | 41.60 | 195 | 18 | 1389 | 9.85x | off the curve |
| still | 544 | 10 | 34dB | 12666 | 1022 | 13688 | 35.81 | 40.89 | 114 | 18 | 1300 | 10.53x | off the curve |
| still | 544 | 15 | 40dB | 18960 | 1022 | 19982 | 35.84 | 41.62 | 195 | 18 | 1393 | 14.35x | off the curve |
| still | 544 | 15 | 34dB | 18960 | 1022 | 19982 | 35.84 | 40.92 | 114 | 18 | 1304 | 15.32x | off the curve |
| still | 544 | 25 | 40dB | 31547 | 1022 | 32569 | 35.86 | 41.63 | 194 | 18 | 1394 | 23.36x | off the curve |
| still | 544 | 25 | 34dB | 31547 | 1022 | 32569 | 35.86 | 40.94 | 114 | 18 | 1306 | 24.94x | off the curve |
| still | 1088 | 10 | 40dB | 12666 | 1022 | 13688 | 48.26 | 48.07 | 12 | 18 | above the curve | n/a | off the curve |
| still | 1088 | 10 | 34dB | 12666 | 1022 | 13688 | 48.26 | 48.30 | 1 | 18 | above the curve | n/a | off the curve |
| still | 1088 | 15 | 40dB | 18960 | 1022 | 19982 | 50.06 | 49.47 | 6 | 18 | above the curve | n/a | off the curve |
| still | 1088 | 15 | 34dB | 18960 | 1022 | 19982 | 50.06 | 50.04 | 0 | 18 | above the curve | n/a | off the curve |
| still | 1088 | 25 | 40dB | 31547 | 1022 | 32569 | 52.11 | 51.62 | 1 | 18 | above the curve | n/a | off the curve |
| still | 1088 | 25 | 34dB | 31547 | 1022 | 32569 | 52.11 | 52.11 | 0 | 18 | above the curve | n/a | off the curve |
| rest | 544 | 10 | 40dB | 12666 | 1698 | 14364 | 35.68 | 39.25 | 197 | 24 | 3164 | 4.54x | off the curve |
| rest | 544 | 10 | 34dB | 12666 | 1649 | 14315 | 35.68 | 39.12 | 117 | 24 | 3043 | 4.70x | off the curve |
| rest | 544 | 15 | 40dB | 18960 | 1698 | 20657 | 35.73 | 39.27 | 194 | 24 | 3178 | 6.50x | off the curve |
| rest | 544 | 15 | 34dB | 18960 | 1649 | 20609 | 35.73 | 39.15 | 116 | 24 | 3072 | 6.71x | off the curve |
| rest | 544 | 25 | 40dB | 32826 | 1693 | 34519 | 35.77 | 39.29 | 193 | 24 | 3198 | 10.79x | off the curve |
| rest | 544 | 25 | 34dB | 32826 | 1649 | 34475 | 35.77 | 39.18 | 116 | 24 | 3096 | 11.13x | off the curve |
| rest | 1088 | 10 | 40dB | 12666 | 1108 | 13774 | 46.77 | 45.47 | 17 | 19 | above the curve | n/a | off the curve |
| rest | 1088 | 10 | 34dB | 12666 | 1081 | 13747 | 46.77 | 46.78 | 1 | 18 | above the curve | n/a | off the curve |
| rest | 1088 | 15 | 40dB | 18960 | 1081 | 20040 | 48.62 | 47.96 | 6 | 18 | above the curve | n/a | off the curve |
| rest | 1088 | 15 | 34dB | 18960 | 1081 | 20040 | 48.62 | 48.60 | 0 | 18 | above the curve | n/a | off the curve |
| rest | 1088 | 25 | 40dB | 31732 | 1081 | 32813 | 50.41 | 49.91 | 2 | 18 | above the curve | n/a | off the curve |
| rest | 1088 | 25 | 34dB | 31732 | 1081 | 32813 | 50.41 | 50.41 | 0 | 18 | above the curve | n/a | off the curve |
| mid | 544 | 10 | 40dB | 12666 | 8468 | 21134 | 35.57 | 38.26 | 197 | 104 | 9193 | 2.30x | off the curve |
| mid | 544 | 10 | 34dB | 12666 | 7817 | 20484 | 35.57 | 38.91 | 118 | 90 | 10557 | 1.94x | off the curve |
| mid | 544 | 15 | 40dB | 18960 | 8378 | 27337 | 35.65 | 38.36 | 194 | 103 | 9396 | 2.91x | off the curve |
| mid | 544 | 15 | 34dB | 18960 | 7668 | 26627 | 35.65 | 38.99 | 117 | 89 | 10739 | 2.48x | off the curve |
| mid | 544 | 25 | 40dB | 32177 | 8196 | 40373 | 35.70 | 38.48 | 193 | 103 | 9636 | 4.19x | off the curve |
| mid | 544 | 25 | 34dB | 32177 | 7873 | 40050 | 35.70 | 39.11 | 117 | 90 | 10996 | 3.64x | off the curve |
| mid | 1088 | 10 | 40dB | 12666 | 5828 | 18495 | 45.95 | 43.57 | 27 | 56 | above the curve | n/a | off the curve |
| mid | 1088 | 10 | 34dB | 12666 | 4757 | 17423 | 45.95 | 45.95 | 2 | 47 | above the curve | n/a | off the curve |
| mid | 1088 | 15 | 40dB | 18960 | 4929 | 23888 | 47.89 | 46.75 | 7 | 49 | above the curve | n/a | off the curve |
| mid | 1088 | 15 | 34dB | 18960 | 4746 | 23705 | 47.89 | 47.88 | 0 | 47 | above the curve | n/a | off the curve |
| mid | 1088 | 25 | 40dB | 31547 | 4741 | 36288 | 49.78 | 49.28 | 2 | 48 | above the curve | n/a | off the curve |
| mid | 1088 | 25 | 34dB | 31547 | 4651 | 36198 | 49.78 | 49.78 | 0 | 47 | above the curve | n/a | off the curve |
| fast | 544 | 10 | 40dB | 12666 | 12167 | 24833 | 36.13 | 34.39 | 196 | 125 | 3348 | 7.42x | off the curve |
| fast | 544 | 10 | 34dB | 12666 | 10930 | 23596 | 36.13 | 36.15 | 104 | 103 | 4815 | 4.90x | off the curve |
| fast | 544 | 15 | 40dB | 18960 | 12321 | 31281 | 36.23 | 34.50 | 193 | 126 | 3420 | 9.15x | off the curve |
| fast | 544 | 15 | 34dB | 18960 | 11080 | 30040 | 36.23 | 36.33 | 103 | 104 | 5008 | 6.00x | off the curve |
| fast | 544 | 25 | 40dB | 31931 | 12464 | 44395 | 36.30 | 34.55 | 191 | 125 | 3457 | 12.84x | off the curve |
| fast | 544 | 25 | 34dB | 31931 | 10982 | 42913 | 36.30 | 36.41 | 102 | 103 | 5087 | 8.44x | off the curve |
| fast | 1088 | 10 | 40dB | 12666 | 8801 | 21467 | 45.86 | 42.02 | 28 | 84 | above the curve | n/a | off the curve |
| fast | 1088 | 10 | 34dB | 12666 | 7282 | 19948 | 45.86 | 45.84 | 2 | 75 | above the curve | n/a | off the curve |
| fast | 1088 | 15 | 40dB | 18960 | 8136 | 27096 | 47.87 | 45.53 | 9 | 80 | above the curve | n/a | off the curve |
| fast | 1088 | 15 | 34dB | 18960 | 7166 | 26126 | 47.87 | 47.85 | 0 | 74 | above the curve | n/a | off the curve |
| fast | 1088 | 25 | 40dB | 31547 | 7291 | 38838 | 49.83 | 49.28 | 2 | 75 | above the curve | n/a | off the curve |
| fast | 1088 | 25 | 34dB | 31547 | 7152 | 38698 | 49.83 | 49.83 | 0 | 74 | above the curve | n/a | off the curve |
| objmotion | 544 | 10 | 40dB | 12666 | 3156 | 15822 | 35.61 | 39.28 | 195 | 50 | 5397 | 2.93x | off the curve |
| objmotion | 544 | 10 | 34dB | 12666 | 3041 | 15707 | 35.61 | 39.14 | 118 | 43 | 5207 | 3.02x | off the curve |
| objmotion | 544 | 15 | 40dB | 18973 | 3150 | 22123 | 35.67 | 39.30 | 193 | 50 | 5424 | 4.08x | off the curve |
| objmotion | 544 | 15 | 34dB | 18973 | 3009 | 21983 | 35.67 | 39.17 | 117 | 43 | 5242 | 4.19x | off the curve |
| objmotion | 544 | 25 | 40dB | 33225 | 3145 | 36370 | 35.71 | 39.32 | 192 | 50 | 5450 | 6.67x | off the curve |
| objmotion | 544 | 25 | 34dB | 33225 | 3011 | 36236 | 35.71 | 39.20 | 117 | 43 | 5283 | 6.86x | off the curve |
| objmotion | 1088 | 10 | 40dB | 12676 | 2157 | 14833 | 46.50 | 44.92 | 21 | 32 | above the curve | n/a | off the curve |
| objmotion | 1088 | 10 | 34dB | 12676 | 1832 | 14508 | 46.50 | 46.50 | 2 | 27 | above the curve | n/a | off the curve |
| objmotion | 1088 | 15 | 40dB | 19103 | 1951 | 21054 | 48.36 | 47.65 | 6 | 28 | above the curve | n/a | off the curve |
| objmotion | 1088 | 15 | 34dB | 19103 | 1889 | 20992 | 48.36 | 48.34 | 0 | 27 | above the curve | n/a | off the curve |
| objmotion | 1088 | 25 | 40dB | 31857 | 1874 | 33732 | 50.21 | 49.73 | 2 | 27 | above the curve | n/a | off the curve |
| objmotion | 1088 | 25 | 34dB | 31857 | 1861 | 33718 | 50.21 | 50.21 | 0 | 27 | above the curve | n/a | off the curve |
| objmotion-still | 544 | 10 | 40dB | 12666 | 2692 | 15358 | 35.71 | 40.88 | 194 | 44 | 4310 | 3.56x | off the curve |
| objmotion-still | 544 | 10 | 34dB | 12666 | 2578 | 15244 | 35.71 | 40.41 | 116 | 38 | 3977 | 3.83x | off the curve |
| objmotion-still | 544 | 15 | 40dB | 18960 | 2692 | 21651 | 35.75 | 40.91 | 193 | 44 | 4330 | 5.00x | off the curve |
| objmotion-still | 544 | 15 | 34dB | 18960 | 2583 | 21543 | 35.75 | 40.45 | 116 | 38 | 4007 | 5.38x | off the curve |
| objmotion-still | 544 | 25 | 40dB | 31547 | 2652 | 34198 | 35.78 | 40.92 | 192 | 44 | 4341 | 7.88x | off the curve |
| objmotion-still | 544 | 25 | 34dB | 31547 | 2547 | 34094 | 35.78 | 40.48 | 115 | 38 | 4023 | 8.47x | off the curve |
| objmotion-still | 1088 | 10 | 40dB | 12666 | 1827 | 14493 | 47.53 | 47.15 | 13 | 28 | above the curve | n/a | off the curve |
| objmotion-still | 1088 | 10 | 34dB | 12666 | 1801 | 14467 | 47.53 | 47.55 | 1 | 26 | above the curve | n/a | off the curve |
| objmotion-still | 1088 | 15 | 40dB | 18960 | 1837 | 20797 | 49.35 | 48.80 | 6 | 27 | above the curve | n/a | off the curve |
| objmotion-still | 1088 | 15 | 34dB | 18960 | 1841 | 20800 | 49.35 | 49.33 | 0 | 26 | above the curve | n/a | off the curve |
| objmotion-still | 1088 | 25 | 40dB | 31547 | 1824 | 33371 | 51.13 | 50.68 | 1 | 26 | above the curve | n/a | off the curve |
| objmotion-still | 1088 | 25 | 34dB | 31547 | 1839 | 33385 | 51.13 | 51.13 | 0 | 26 | above the curve | n/a | off the curve |

