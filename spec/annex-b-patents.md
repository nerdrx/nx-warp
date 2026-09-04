# Annex B — Intellectual property hygiene (informative)

**This annex is informative and is not legal advice.** It records the design's
own account of why each tool was chosen, so that a freedom-to-operate review has
a list to work from rather than a codebase to reverse-engineer. Nothing here has
been reviewed by counsel. The review required before release is B.3.

Sources: [PAPER 1.9] and [PAPER 6.12], with the tool-by-tool mapping to the
clauses of this specification added.

## B.1 Tools relied on, and why they are believed safe

| Tool | Clause | Basis |
|---|---|---|
| DCT | 6.4 | Ahmed, Natarajan and Rao, 1974 [R-6]. Expired |
| Loeffler-Ligtenberg-Moschytz 8-point factorisation | 6.4.2 | 1989 [R-5]. Expired. The nine-bit constants of Annex A.1 are this format's own, deliberately not another codec's matrix |
| YCoCg-R reversible lifting | 6.3 | Malvar and Sullivan, 2003 [R-7]. Offered royalty-free for H.264 FRExt, and in any case past twenty years |
| Exp-Golomb codes | 6.6.6 | Long-standing, no known claims |
| JPEG-style weighting matrices, dead-zone quantisation | 6.4.1 | 1980s–1990s practice. Expired |
| Trellis quantisation | Encoder only | Marcellin and Fischer, 1990 [I-12]. Expired, and not part of the decoding process in any case |
| H.263 / MPEG-2 era bi-prediction weights and spatial scalability | 6.9 | H.263 Annex O, 1998. Expired |
| H.264 baseline tools filed 2002–2003, including the 4x4 integer transform and the intra 16x16 DC transform idea | 6.4, 6.7 | Expired 2023–2024 in the US and EU. **FRExt-era claims must be checked individually** |
| rANS | 6.6 | Duda placed the construction in the public domain [R-8] |
| Interleaved rANS over one byte stream | 6.6.4 | Giesen's public-domain blog post and reference code [R-9] |
| EZW and SPIHT | Tool bit `ENT_BITPLANE` | 1993 and 1996 [I-10]. Expired |
| VC-2 / Dirac Pro | Tool bit `XFORM_WAVELET` | BBC royalty-free declaration [I-8] |
| MPEG-4 Part 2 global motion compensation (sprite warping) | 6.7 | 1999 [I-7]. Expired. Warps corner points with integer arithmetic and interpolates — the same structure |
| AV1 global motion | 6.7 | Royalty-free under the AOM licence for AV1 implementations; cited as prior art for the corner-then-interpolate structure, not as a licence [I-5] |
| Cubic convolution / Catmull-Rom interpolation | 6.7.5, Annex A.4 | Keys, 1981 [R-10]. Expired |
| AES-GCM, ChaCha20-Poly1305, HKDF, Reed-Solomon | 7.2 | Published standards, no known encumbrance [R-12], [R-13], [R-14], [R-15] |

## B.2 Tools deliberately avoided

Each of these was considered and rejected on IP grounds, not on merit. Avoiding
them is a *constraint on this specification*, so a future edition that reaches
for one of them reopens the question.

| Avoided | Why | What was done instead |
|---|---|---|
| HEVC-specific tools: its transform matrices, SAO, merge/AMVP design, its CABAC context tables | Live claims | Own transform constants (Annex A.1); no loop filter at all (clause 6.1); one vector per tile with a temporal predictor (clause 6.7); own contexts (Annex A.5) |
| AV1 tools such as CDEF, loop restoration and its adaptive multi-symbol CDF scheme | The AOM patent licence covers implementations of AV1 itself, not reuse of its tools inside another codec | Static per-frame rANS tables (clause 6.6.2), no post-filter |
| LCEVC's residual temporal prediction and its transforms | Patented, V-Nova [I-6] | The layered mode predicts **pixels**, not residuals, with the same DCT and rANS tools as the base and an explicit two-hypothesis blend [PAPER 1.7] |
| JPEG XS bit-plane-count coding | RAND pool | Not used; the bit-plane fallback follows the EZW/SPIHT and VC-2 lineage |
| 3D-HEVC view synthesis prediction | Live claims, and the closest patented relative of both pose-warped and inter-view prediction | Clause 6.8's disparity is a plain per-tile shift with no depth at the decoder and no view synthesis [STEREO 2.1] |
| H.264 CABAC | Expired 2023–2024 in most jurisdictions, but complex to keep in lockstep across lanes anyway | rANS (clause 6.6) |

H.264 and HEVC are only ever touched through the device's own licensed hardware
decoder, in the hybrid path (clause 6.9). This specification never asks a
decoder to implement either.

## B.3 The scoped freedom-to-operate review

[PAPER 6.12] scopes the review before Phase 3 ships to exactly four items. All
four are still open; this annex adds a fifth that the specification work
surfaced.

1. **Pose delta as global motion parameters for a streamed VR frame**
   (clause 6.7). The nearest relatives are expired MPEG-4 GMC and royalty-free
   AV1 global motion, and the *mechanism* is therefore believed safe. Neither
   prior art ties the parameters to a tracked head pose, which is exactly the
   novel step. Meta, Qualcomm, NVIDIA and Microsoft filings on pose-based
   prediction for split rendering, roughly 2016 to 2020, must be searched.
   [pending review]
2. **3D-HEVC view synthesis prediction against `STEREO`** (clause 6.8).
   [pending review]
3. **The LCEVC family against the enhancement layer** (clause 6.9).
   [pending review]
4. **The 2022 Microsoft rANS patents against the fixed-precision construction**
   of clause 6.6 [I-15]. The design's defence is structural and deliberate: one
   state width, one probability precision, no adaptivity, chosen precisely to
   stay outside claims concerning selective switching of state precision. That
   defence should be verified rather than assumed, and if it fails, note that it
   fails against a *design decision* that is cheap to revisit, not against an
   architectural commitment. [pending review]
5. **Added here: the concealment-as-prediction identity** (clause 6.11) — that a
   lost tile is reconstructed by the same deterministic warp as a skipped tile
   so the encoder can replay it and keep its reference model exact. This is the
   design's most distinctive claimable idea and it is not on the paper's list.
   [pending review]

## B.4 Notes for whoever runs the review

* The specific caveat [PAPER 1.9] raises about item 4 is that the fixed state
  width and fixed probability precision were chosen *for this reason*. That
  intent is documented in [SYNTAX 9.5] and reproduced in clause 6.6.1, which
  should help rather than hurt.
* The identifiers of the Microsoft patents are deliberately not quoted anywhere
  in this repository. Obtain them from counsel; do not seed the review from a
  developer's recollection.
* Annex A.1 exists partly for this review: it lets an examiner confirm by
  inspection that the transform constants are not another codec's.
* The tool-bit mechanism (clause 8.4) means a tool found to be encumbered can be
  disabled by negotiation without a format revision, for every tool except the
  four that version 1 makes mandatory: the DCT, YCoCg-R, rANS, and DC-plane
  intra.
