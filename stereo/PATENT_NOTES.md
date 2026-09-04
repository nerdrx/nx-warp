# STEREO mode: prior-art and FTO scoping notes

**Not legal advice.** An engineer's map of where the nearest patented mechanisms are, written to
scope the freedom-to-operate review that PAPER 6.12 requires before Phase 3 ships. Everything here
is from memory of the public standards literature and is marked where it is uncertain. No patent
numbers are cited, because any number written from memory would be worse than useless — finding the
actual families is the reviewer's job, and this document's job is to tell the reviewer what to
search for and what our answer is.

PAPER 6.12 names "view synthesis prediction in 3D-HEVC against the STEREO mode" as one of four FTO
items. This document is the input to that item.

---

## 1. What our mode actually is, stated for a patent attorney

For each 64x64 tile of the **dependent (right) view** of frame N, the encoder may signal a mode in
which the prediction is formed by:

1. selecting the already-decoded **independent (left) view of the same frame N** as the reference
   picture;
2. reading one **scalar horizontal displacement** for the tile from the bitstream, coded at
   quarter-pixel precision as a delta from the same tile's displacement in the previous frame;
3. sampling the reference at `(x + displacement, y)` with a fixed 16-phase 4-tap integer filter,
   with clamp-to-edge at picture boundaries;
4. adding a coded transform residual.

The decoder receives: a mode index and one displacement. **It receives no depth information, no
camera parameters, no disparity map, no view-synthesis parameters, and no per-sample or sub-block
partitioning.** It performs no projection, no forward warping, no hole detection and no hole
filling. The prediction is dense by construction because it is a translation.

Depth, when the application supplies it, is used **only inside the encoder**, for two purposes that
never reach the bitstream: to seed the displacement search, and to decide whether to offer the mode
at all (the disocclusion and border guards). An encoder that ignores depth entirely produces a
legal bitstream that a decoder cannot distinguish, and the experiment measured that path recovering
95 percent of the gain (`stereo/RESULTS.md`).

That last paragraph is the centre of the FTO position and it should be the first thing the reviewer
reads.

---

## 2. 3D-HEVC view synthesis prediction, and why we are not it

**What 3D-HEVC VSP does** (Annex I of HEVC, standardised around 2015; description from memory,
verify against the specification):

- The bitstream carries **coded depth maps** as separate components alongside the texture views.
  Depth is normative decoder input.
- Backward VSP (BVSP) takes a depth block associated with the current texture block, converts each
  depth sample to a disparity using **transmitted camera parameters** (focal length, baseline,
  z-near/z-far), and warps the reference texture at a **sub-block granularity** — 8x4 or 4x8
  sub-blocks, each with its own derived disparity, believed derived from the maximum of the
  sub-block's depth corners.
- The disparity vector for a block is derived by **NBDV / DoNBDV**: neighbouring-block disparity
  vector derivation, which inspects spatial and temporal neighbours and optionally refines the
  result using the decoded depth map.
- The family also includes depth modelling modes (DMM) for coding depth edges, advanced residual
  prediction (ARP), illumination compensation (IC), and depth-based motion-vector inheritance.
- More generally, the DIBR (depth-image-based rendering) literature that VSP descends from does
  forward projection with splatting and explicit hole filling, which is what "view synthesis" means
  outside the codec.

**Mechanism differences, in the order a claim chart would take them:**

| 3D-HEVC VSP | NX Warp STEREO |
|---|---|
| Depth maps coded in the bitstream and decoded | No depth anywhere in the bitstream or the decoder |
| Camera parameters transmitted and used by the decoder to convert depth to disparity | No camera parameters transmitted; the encoder does any conversion it likes, privately |
| Disparity derived per 8x4/4x8 sub-block from decoded depth | One scalar per 64x64 tile, read directly from the bitstream |
| Disparity vector derived from neighbouring blocks (NBDV), i.e. blocks depend on each other | Tiles are independent by construction; the only predictor is the **same tile's own value in the previous frame** |
| Warping is depth-driven view synthesis; the DIBR lineage needs hole handling | Prediction is a rigid translation; there are no holes to handle, and disoccluded regions are simply paid for in the residual or refused by the encoder |
| Depth is normative: two decoders must derive the same disparity from the same depth | The displacement is transmitted, so there is nothing to derive |

The strongest statement we can make is the negative one: **every step of VSP that a claim is likely
to recite — decoding depth, converting depth to disparity with signalled camera parameters,
partitioning a block by depth, deriving a vector from neighbours — is absent from our decoder.**
Our encoder does use depth to pick a number, but so does any encoder that uses any side information
to seed a motion search, and a claim that covers "an encoder that uses depth to choose a
displacement it then transmits" would read on a great deal of ordinary practice.

**Where we are *not* safe by this argument:** if a claim is drafted broadly at the level of "coding
a block of a second view by displacement-compensated prediction from a first view of the same time
instant, where the displacement is determined from depth of the scene", the encoder-side-only
distinction is a defence about claim scope and validity, not about non-infringement of the decoder.
The reviewer should look specifically for encoder-side claims and for method claims that do not
recite decoding.

---

## 3. MV-HEVC and MVC: we are much closer to these, which is good news

MV-HEVC (and its H.264 predecessor MVC, Annex H) add inter-view prediction as a **high-level syntax
change only**: pictures of the base view at the same time instant are inserted into the reference
picture lists of the dependent view, and the existing block-level motion compensation then produces
a translational "disparity" vector by ordinary motion estimation. No depth, no block-level tools.

**Our mechanism is structurally the same idea**: an inter-view reference plus a translational
vector per block. The differences are ours being one vector per 64x64 tile with no sub-partitioning,
tile independence (no spatial MV prediction), a fixed integer filter shared with the pose warp, and
no reference-picture-list machinery at all (the mode names the reference).

This matters for FTO in two directions:

- **Prior art.** Disparity-compensated prediction of a dependent view from a base view by a
  translational block vector is old. MPEG-2's Multiview Profile (believed 1996) already did exactly
  this by reusing motion compensation across views, and H.264 MVC (believed 2009) standardised it
  again. If the 1996-era MPEG-2 multiview work is as described, it is expired prior art for the
  *core* mechanism, and that is the single most useful thing this document can point the reviewer
  at. **Verify the MPEG-2 Multiview Profile date and its technical content first.**
- **Licensing exposure.** MV-HEVC and 3D-HEVC are HEVC extensions and are, believed, licensed
  through the HEVC pools (Access Advance runs a multiview/3D programme; verify). We do not
  implement HEVC, so this is only relevant if a pool member's patents have claims broad enough to
  cover a non-HEVC codec — which is exactly the question the review must answer, and is the same
  question already open for the pose warp against Meta/Qualcomm/NVIDIA/Microsoft filings.

---

## 4. Other neighbours worth a search

- **Stereo/multiview streaming for HMDs**: Meta, Qualcomm, Magic Leap, Apple and Sony all have
  filings in the 2016 to 2022 window around split rendering and stereo transmission. Search for
  claims combining "second eye image", "predicted from the first eye image", "head-mounted", and
  "depth". This is the highest-risk area and it is *not* the standards literature. Uncertain and
  unsurveyed.
- **Frame-packed stereo / "one eye plus residual"** schemes in 3D broadcast (2010 to 2013 era).
  Mostly expired or expiring; mechanism is the same disparity-compensated prediction.
- **Foveated stereo coding.** If the eventual design computes disparity through per-eye foveation
  grids (`docs/STEREO.md` 10.1), that combination is newer and less well covered by old prior art.
  Flag it if the design goes that way.
- **The disocclusion guard.** "Deciding not to use inter-view prediction based on a depth-derived
  visibility estimate" is an encoder decision rule. It is the most novel-looking thing we do and
  therefore, symmetrically, the thing most worth checking and possibly the only part of STEREO
  worth a defensive publication.

## 5. What we already do right, and what would strengthen the position

Already right:

1. No depth on the decoder — the single decision that separates us from the entire 3D-HEVC/DIBR
   family. It was made in PAPER 2.1 for compute-budget reasons and it turns out to be the FTO
   answer as well.
2. No spatial dependency between tiles, which sidesteps the NBDV family entirely. Also made for
   another reason (loss resilience, PAPER 2.3).
3. The sampler is shared with the pose warp, whose lineage is MPEG-4 Part 2 global motion
   compensation (expired) and AV1 global motion (royalty-free). We inherit that argument for free.
4. A written record of the public-domain source of every tool, per PAPER 5.7. Keep it for STEREO
   too: this document is the start of it.

Would strengthen it:

5. **Keep depth strictly non-normative and say so in the specification text**, in words: "no
   conforming decoder consumes depth; depth-derived quantities exist only in the encoder and are
   not recoverable from the bitstream". A specification sentence is worth more than an
   implementation fact.
6. **Do not add sub-tile disparity segmentation** (`docs/STEREO.md` 8.2's v2 candidate) without
   running FTO on it first. Partitioning a block into regions by disparity is precisely the
   3D-HEVC/DMM neighbourhood, and it would give up the cleanest distinction we have for a gain the
   experiment has not shown to exist.
7. **Do not add decoder-side hole filling.** Same reason, more so.
8. Consider a **defensive publication** of the mode as specified — the guards in particular — once
   the design is settled. Cheap, and it makes later filings by others harder.

## 6. Concrete questions for the FTO reviewer

1. Does the MPEG-2 Multiview Profile (believed 1996) disclose disparity-compensated prediction of a
   dependent view by a translational block vector from a base view at the same time instant? If
   yes, is it expired? This is the anchor.
2. Are there live claims covering inter-view prediction where the displacement is *transmitted*
   rather than derived from decoded depth, that are not anticipated by MVC/MPEG-2 MVP?
3. Are there claims that recite only encoder behaviour — "determining a disparity from a depth
   buffer and encoding it" — without a decoding step?
4. In the HMD/split-rendering filings (Meta, Qualcomm, Apple, Sony, Magic Leap, NVIDIA), is there
   anything on predicting one eye's streamed image from the other eye's streamed image of the same
   frame?
5. Is the depth-derived disocclusion guard (refusing inter-view prediction where projected
   visibility is poor) claimed anywhere, in codecs or in DIBR?
6. Does the multiview licensing programme attached to the HEVC pools assert against non-HEVC
   implementations?
