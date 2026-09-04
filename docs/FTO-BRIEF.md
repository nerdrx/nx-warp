# NX Warp freedom-to-operate brief

**This is a scoping document for a human review. It is not legal advice, it was
not written by counsel, and no conclusion in it may be relied on as clearance.**
Its job is to hand an attorney a worked starting point — claims pulled,
mechanisms mapped, prior art dated — so the paid hours go to judgement rather
than retrieval. Every "does not match" below is an engineer's reading of claim
language against `docs/SYNTAX.md`, which is exactly the thing patent lawyers are
paid to distrust.

Companion documents: `spec/annex-b-patents.md` (the tool-by-tool provenance
record and the five-item review agenda), `stereo/PATENT_NOTES.md` (the STEREO
mode's own scoping note), `docs/RESEARCH-INDUSTRY.md` section 6.

## Sourcing and verification status

* **Verified.** The Google Patents page was fetched and the quoted claim text was
  extracted from the `claims` section of that page; dates are that page's
  `priorityDate` / `filingDate` / `publicationDate` fields.
* **Reported.** From a search result or secondary page, not the patent itself.
* **Estimated.** Expiry marked *estimated* is twenty years from the filing date
  shown, with no allowance for term adjustment, terminal disclaimers or lapse;
  real expiry can differ by years. Only the two expiry dates attributed to Google
  Patents are otherwise.

No patent number here was written from memory; every one was resolved to a
fetched page. Numbers seen only in a search result are marked **unverified** and
must be re-derived by the reviewer.

---

## 1. Executive summary

Eleven live filings and one expired one were pulled and read. The headline is
that **the filing the project has been treating as its lead risk is much
narrower than its title**, and that **the real exposure is not in the codec at
all but in the system NX Warp is deployed inside**.

1. **Intel US 12348767 B2 is not the problem the industry note thought it was.**
   `docs/RESEARCH-INDUSTRY.md` 6.1 reads its *title and disclosure*. The
   **granted claims recite neither foveated quality signalling per macroblock nor
   a pose-derived global motion predictor**: all twenty are one idea in three
   statutory flavours — analyse encoding statistics of the *foveated region of
   previously encoded frames*, derive quantization parameters from them, encode.
   Our per-tile `qp_delta` (SYNTAX 4.1) comes from a gaze map and the rate
   controller, not from a prior frame's fovea statistics. But the continuation
   **US 2025/0280147 A1 is pending**, so Intel is still prosecuting this
   disclosure and the global-motion half of it remains unclaimed. **Read it
   first, for the prosecution risk rather than the granted claims.**
2. **AMD/ATI US 10432988 B2 is the genuine blocker candidate, and it blocks
   WiVRn NX rather than nxvc.** Claim 1 is four steps — receive user feedback,
   predict the viewpoint of the next frame, render *a portion* of it using the
   prediction, encode, transmit — with no codec limitation anywhere in it. Any
   split-rendering VR streamer doing pose prediction reads on it on its face, and
   the family is actively continued (US 12120364 B2, 2024). Our defences are
   validity and the "render a portion" element, neither of which is a comfort.
3. **The Microsoft rANS position is better than the specification assumes, and
   for a different reason than the specification gives.** The defence in SYNTAX
   9.5 and Annex B.4 — "one state width, one probability precision, no
   adaptivity" — answers a claim Microsoft did not get. US 11234023 B2's claim 1
   turns on a **two-phase decode structure with conditional symbol emission**, a
   pipeline that can stall; ours emits one symbol per iteration and renormalizes
   conditionally, Duda's and Giesen's published construction unchanged. The live
   risk is the **pending continuation US 2022/0109891 A1**, whose
   claims recite a header syntax element signalling whether decoder state is
   flushed. We flush unconditionally and signal nothing, so we are outside it as
   drafted — but it is pending, and pending claims move.
4. **Nothing found reads on the concealment-as-prediction identity** (SYNTAX
   13.6, TRANSPORT 9) — the design's most distinctive claimable idea. The nearest
   live filing, Cisco US 8494049 B2, requires a reference-frame *message*,
   multiple decoders and a handshake we do not have. Strongest candidate for a
   defensive publication.
5. **Every depth-based neighbour missed us because we ship no depth to the
   client.** Valve US 11303875 B2, Qualcomm US 11455705 B2 and Apple
   US 12429695 B2 each require depth data, per-pixel motion vectors or layered
   render targets to reach the client. PAPER 2.1's decision to keep depth
   encoder-side is doing more FTO work than any other choice in the design, as
   `stereo/PATENT_NOTES.md` 5.1 predicted.

**Genuine blocker, one:** AMD/ATI US 10432988 B2 and its continuations, against
the *deployment*, on the strength of claim 1's four unqualified steps. Everything
else in this set is a defensible non-match, a validity argument, or a watch item.

---

## 2. The mechanisms, stated for a reader who will not open the syntax

Twelve things NX Warp does that a claim could read on.

| # | Mechanism | Clause |
|---|---|---|
| M1 | A per-eye 3x3 homography, quantised to fixed point, transmitted once per frame, derived by the encoder from the pose delta | SYNTAX 3.1.1 |
| M2 | Per-tile prediction by warping a reference picture through M1, plus a coded quarter-pel vector | SYNTAX 13.1, 13.3 |
| M3 | Near-skip: a tile whose prediction is good enough carries the stored vector and no residual | SYNTAX 13.1 `WARP_SKIP` |
| M4 | Pose bytes carried opaquely, never interpreted by the decoding process | SYNTAX 3.2 |
| M5 | Per-tile `res_level` (64/32/16) and `qp_delta`, driven by a gaze foveation map | SYNTAX 4.1, 4.2 |
| M6 | Tile classes A/B/C by eccentricity, driving FEC strength, not coding | TRANSPORT 1, 6 |
| M7 | Per-tile reference selection from a four-slot ring, chosen from receiver feedback | SYNTAX 13.2, TRANSPORT 9 |
| M8 | Concealment identical to a legitimate skip, so the encoder can replay it exactly | SYNTAX 13.6, TRANSPORT 9 |
| M9 | Rolling intra refresh, 1/180 of tiles per frame, plus feedback-driven refresh | TRANSPORT 9 |
| M10 | `STEREO`: one scalar horizontal disparity per tile from the other eye of the same frame | SYNTAX 13.1, `stereo/PATENT_NOTES.md` |
| M11 | Static-table rANS, 32-bit state, `M = 2^10`, 8 interleaved lanes, per-tile state reset | SYNTAX 9.1, 9.5 |
| M12 | Layered mode: an enhancement layer predicting **pixels** with a two-hypothesis blend (`wgt`) | SYNTAX 4.1, Annex B.2 |

Two negatives matter as much as the positives and should be quoted to counsel:
**the decoder receives no depth and no camera parameters**, and **the decoder
performs no floating-point arithmetic** (SYNTAX 1), which is why M1 is a
transmitted matrix rather than a decoder-side derivation from M4.

---

## 3. Claim charts

### 3.1 Intel — US 12348767 B2 — *lead item*

*Verified.* "Adaptive foveated encoder and global motion predictor", Intel Corp.
Priority 2017-11-23; filed 2024-04-24 (continuation); granted 2025-07-01;
anticipated expiration **2037-11-23** (as stated by Google Patents). Family:
US 2024/0357159 A1, **US 2025/0280147 A1 (pending)**. 20 claims; independent
claims 1 (method), 8 (CRM), 15 (apparatus), textually identical in substance.

> **1.** A method of generating an encoded video, the method comprising:
> analyzing encoding statistics of one or more previously encoded frames, wherein
> a previously encoded frame has been generated by encoding a frame in a video,
> wherein the encoding statistics correspond to a foveated region of each of the
> one or more previously encoded frames; determining one or more quantization
> parameters for a current frame in the video based on the encoding statistics of
> the one or more previously encoded frames; and encoding the current frame using
> the one or more quantization parameters.

| Element | NX Warp | Verdict |
|---|---|---|
| "analyzing encoding statistics of one or more previously encoded frames" | The rate controller consumes prior-frame statistics (`docs/RATECONTROL.md`) | **matches** |
| "wherein the encoding statistics **correspond to a foveated region**" | Statistics are gathered per tile and per band; whether any aggregate is *of the foveated region as such* must be answered against `rc/` | **arguable — the decisive element** |
| "determining one or more quantization parameters for a current frame based on [those] statistics" | `base_qp` and per-tile `qp_delta` (SYNTAX 4.1) | **matches** |
| "encoding the current frame using the ... quantization parameters" | SYNTAX 6.5 | **matches** |

The claim requires no eye tracking, no head-mounted display, no motion
prediction and no global motion. Dependent claims 7/14 ("used in a virtual
reality system") and 5/12 (two different QPs for two portions) would both read on
us. The whole non-infringement weight therefore rests on the word *foveated*
qualifying *encoding statistics* — on whether our controller closes a loop on
fovea-region bit counts specifically. **If it does, we are inside claim 1.**
Section 6 says how to change that cheaply.

**The pending continuation is the larger risk.** US 2025/0280147 A1 (filed
2025-05-19, published 2025-09-04, *verified*) claims at claim 1 "One or more
non-transitory computer-readable media storing a video, the video comprising an
encoded frame that includes a focus region and a region having a lower quality
than the focus region, the video produced by a process comprising: analyzing
encoding statistics ... corresponding to a focus region ...". Claim 15 adds "a
head-mounted display" and "a sensor to collect focus-related information, the
focus region identified based on the focus-related information" — gaze-driven
foveation on a headset, as a **product-by-process claim on the bitstream
itself**, which would reach a stored `.nxv` file. Pull the file wrapper: the
global-motion-from-HMD-pose disclosure is still available to be claimed in a
further continuation, and that is the claim that would read on M1.

### 3.2 AMD / ATI Technologies — US 10432988 B2 — *genuine blocker candidate*

*Verified.* "Low latency wireless virtual reality systems and methods", ATI
Technologies ULC and Advanced Micro Devices Inc. Priority and filing 2016-04-15;
granted 2019-10-01; anticipated expiration **2037-12-24** (as stated by Google
Patents — well beyond the nominal 2036 term, so substantial term adjustment).
Family: US 2017/0302972 A1, US 10880587 B2, US 11553222 B2, **US 12120364 B2**
(continuation, filed 2023-01-06, granted 2024-10-15, *verified*). Independent
claims 1 (server method) and 7 (server apparatus); the fetched page also carries
client-side independent claims.

> **1.** A method of processing Virtual Reality (VR) data, the method comprising:
> receiving user feedback information; using one or more server processors to:
> predict, based on the user feedback information, a user viewpoint of a next
> frame of a sequence of frames of video data to be displayed; render a portion of
> the next frame of video data to be displayed using the prediction; and encode the
> portion of the next frame of video data to be displayed; and transmit the encoded
> and formatted portion of the next frame of video data to be displayed.

| Element | NX Warp / WiVRn NX | Verdict |
|---|---|---|
| "receiving user feedback information" | The pose stream and the feedback packet (TRANSPORT 8) | **matches** |
| "predict ... a user viewpoint of a next frame" | Standard in the runtime; `docs/RESEARCH-INDUSTRY.md` 7.4 proposes ALVR's filter | **matches** |
| "render a portion of the next frame ... using the prediction" | The compositor renders from the predicted pose; whether it renders **a portion** is the one contestable element | **arguable** |
| "encode the portion" / "transmit the encoded and formatted portion" | nxvc; TRANSPORT 3 | **matches** |

Nothing in this claim is about compression. It is a claim on split rendering with
pose prediction, and NX Warp cannot design around it because NX Warp is not what
infringes — the pipeline it sits in is. Three observations for counsel:

* The only textual escape is "**a portion of** the next frame": construed as
  *less than the whole frame*, a renderer producing the full frame at the
  predicted pose is outside it; construed as *some part of*, everything is inside.
  Order the prosecution history.
* Validity is where the real work is. Levoy's 1995 split rendering (section 4),
  the 2014–2016 ALVR/Moonlight/in-home-streaming record, and Oculus's own 2016
  ASW disclosures predate or straddle the 2016-04-15 priority date — a *pool* of
  art, so an obviousness argument, which is expensive.
* AMD has kept the family alive eight years and took a continuation in 2024.

**This item decides whether Phase 3 ships as a product or as a specification**,
and it is the one entry on this page where a codec design change buys nothing.

### 3.3 Varjo — US 11568574 B1

*Verified.* "Foveation-based image encoding and decoding", Varjo Technologies Oy.
Priority and filing 2021-08-18; granted 2023-01-31; anticipated expiration
2041-08-29 (Google Patents). Family: US 2023/0057755 A1, WO 2023/021234 A1.
27 claims; independent claims 1 (encode), 7, 17 (decode).

> **1.** An encoding method comprising: generating a curved image by creating a
> projection of a visual scene of an extended-reality environment onto an inner
> surface of an imaginary 3D geometric shape, the imaginary 3D geometric shape
> being curved in at least one dimension, wherein a centre of the imaginary 3D
> geometric shape corresponds to a position of a user's eye ...; dividing the
> curved image into an input portion and a plurality of input rings; encoding the
> input portion and the plurality of input rings ... into a first planar image and
> a second planar image ...; packing the plurality of input rings ... into a
> corresponding row of the second planar image; and communicating, to a display
> apparatus, the first planar image, the second planar image and information
> indicative of a size of the input portion and sizes of the plurality of input
> rings.

Four structural elements are absent: no spherical projection (we encode flat eye
buffers); a uniform 64x64 grid rather than concentric rings (SYNTAX 3.3); no
packing or remapping; and one picture per eye rather than a pair of planar images
plus ring-size side information. Decode claim 17 fails on the same elements.
**Clear non-match** — Annex B.3.1's "adjacent to per-tile res_level" should
become a portfolio watch.

### 3.4 Microsoft — US 11234023 B2 and US 2022/0109891 A1

*Verified.* "Features of range asymmetric number system encoding and decoding",
Microsoft Technology Licensing LLC. Filed 2019-06-28, granted 2022-01-25; expiry
*estimated* 2039-06-28. Family: WO 2020/263438 A1, **US 2022/0109891 A1 — status
Pending** (application 17/552,295, filed 2021-12-15). Independent claims 1, 19,
20.

> **1.** A computer system comprising: an encoded data buffer ...; and a range
> asymmetric number system ("RANS") decoder configured to perform operations using
> a two-phase structure for RANS decoding operations, the operations comprising:
> during a first phase of the two-phase structure, selectively updating, depending
> on a determination of whether or not an output symbol from a previous iteration
> was generated, state of the RANS decoder using probability information for the
> output symbol from the previous iteration ...; during a second phase ...,
> selectively merging a portion of the encoded data from an input buffer into the
> state of the RANS decoder; and during the second phase ..., selectively
> generating, depending on a determination of whether or not the state of the RANS
> decoder includes sufficient information to generate an output symbol for a
> current iteration, the output symbol ..., the state ... including sufficient
> information ... if the state of the RANS decoder is greater than a threshold.

Against SYNTAX 9.5: "a RANS decoder" **matches**; "a **two-phase structure**"
**does not match**, our decode step being one phase (mask, table lookup, state
update, conditional renormalize); "**selectively** updating ... depending on
whether an output symbol from a previous iteration was generated" **does not
match**, since every iteration generates a symbol and no such predicate exists;
"selectively merging a portion of the encoded data into the state" is
**arguable**, renormalization being conditional on `x < 2^16`; and "**selectively
generating** ... depending on whether the state ... is greater than a threshold"
**does not match**, because `x >= L` is an invariant maintained by
renormalization, not a gate on emission.

**Three of five elements absent.** The claim describes a decoder that can stall —
a hardware or SIMD pipeline emitting zero or one symbol per cycle. Ours cannot
stall by construction. Note for the record that the defence written into SYNTAX
9.5 and Annex B.4 **defends against a claim that was not granted**. It remains a
good design decision and it does answer the family's *disclosure*, which is about
switching symbol widths and probability models, but the annex should be corrected
to say what the granted claim actually recites.

**The continuation is the live item.** Its independent claims 21, 30 and 40
recite a bitstream header carrying "a syntax element that indicates whether or not
the state of the RANS decoder is to be flushed/re-initialized", and claim 40 is a
**bitstream claim** — a medium holding data organized that way. We transmit
`4 * active_lanes` bytes of initial state at the head of every tile payload and
always re-initialize; no syntax element says *whether*, because the answer is
always yes. Outside it as drafted — but the application is pending, an examiner
may allow broader language, and a bitstream claim reaches files rather than
implementations. **Docket it; re-read at allowance.**

### 3.5 ZeniMax Media (now Microsoft) — US 11503326 B2

*Verified.* "Systems and methods for game-generated motion vectors", Zenimax
Media Inc. Priority 2017-04-21, filed 2020-01-07, granted 2022-11-15; expiry
*estimated* 2037-04-21. Parent US 10701388 B2 (*verified*); a further
continuation numbered 12003756 appeared in search results and is **unverified**.
Independent claims 1, 9, 17.

> **17.** A computer-implemented method for encoding video in a system comprising a
> graphics engine and a video encoder, the method comprising: receiving, at the
> video encoder, motion information from the graphics engine; receiving, at the
> video encoder, a video frame to be encoded; and with the video encoder, encoding
> the video frame ..., wherein the encoding the video frame includes: responsive to
> the receiving the motion information from the graphics engine, bypassing motion
> estimation for one or more blocks of the video frame; using one or more per-block
> motion vectors in motion compensation for the one or more blocks ...; and
> outputting, in a bitstream, the encoded video data.

| Element | NX Warp | Verdict |
|---|---|---|
| "receiving, at the video encoder, motion information from the graphics engine" | The encoder receives a **pose** from the XR runtime, not vectors from the renderer | **arguable — turns on whether a pose is "motion information" and a runtime a "graphics engine"** |
| "bypassing motion estimation for one or more blocks" | `WARP_SKIP` (M3) skips search and residual where the warp suffices | **arguable, leaning matches** |
| "using one or more **per-block** motion vectors in motion compensation" | The homography is per frame and per eye (M1); per-tile vectors are searched, not supplied | **does not match** |
| "outputting, in a bitstream, the encoded video data" | Yes | **matches** |

The distinction that saves us is the one that makes the format interesting:
**the engine-derived quantity is a 3x3 matrix for the whole picture, not a
per-block motion vector**, and the per-tile vectors that exist are the encoder's
own search results. Test that against the doctrine of equivalents: "convert one
or more motion vectors into one or more per-block motion vectors" (claim 1) is a
short step from "evaluate a homography at a tile centre", and an encoder fast
path that instantiated the warp as per-tile vectors for a search-skipping stage
would take it. Note the assignee — ZeniMax was acquired by Microsoft in 2021 and
has a litigation history in exactly this area.

### 3.6 The confirmation pass: five filings that miss on structural elements

Each was fetched and its independent claims read in full; each fails on at least
two elements that are not close questions.

**Apple, US 12429695 B2**, "Video compression methods and apparatus" (*verified*;
priority 2017-05-30, filed 2024-01-24, granted 2025-09-30; expiry *estimated*
2037-05-30; independent claims 1, 11). Claim 1 requires receiving motion vectors
that "include head motion vectors determined ... by one or more sensors" —
**arguable** against our pose stream — then "rendering ... a frame comprising one
or more layers **at different depths** overlaid on a base layer" and "encoding the
rendered frame **along with associated motion vectors corresponding to the ...
layers**". Both **do not match**: our `layer` field is a scalability layer, not a
depth plane, and no per-layer vectors are transmitted. Watch Apple's XR
video-coding publications on the usual eighteen-month lag.

**Qualcomm, US 11455705 B2**, "Asynchronous space warp for remotely rendered VR"
(*verified*; priority 2018-09-27, filed 2019-05-20, granted 2022-09-27; expiry
*estimated* 2039-05-20; EP 3856376 A1 in family; independent claims 1, 16, 21,
22). The server claim requires **per-pixel depth**, classification of pixels by
depth value, "a motion vector for each pixel", extrapolation of those, and
**transmission of the extrapolated per-pixel vectors** with the encoded content;
the client claim requires receiving them and "warping the decoded first graphical
content". Four **does not match** findings: no depth anywhere, one vector per
64x64 tile plus one matrix per eye, nothing extrapolated is transmitted, and —
the distinction that separates NX Warp from the whole ATW/ASW family — **everyone
else warps the decoded picture on the way to the display; we warp the reference
on the way into the prediction** (SYNTAX 13.3). Claim language drafted around
"warping the decoded content" does not reach a prediction loop.

**Valve, US 11303875 B2**, "Split rendering between a head-mounted display (HMD)
and a host computer" (*verified*; filed 2019-12-17, granted 2022-04-12; expiry
*estimated* 2039-12-17; WO 2021/126854 A1, EP 4045965 A1, CN 114730093 A;
independent claims 1, 7, 12). Every independent claim requires the HMD to receive
**depth data**, **classify pixels as foreground and background from it**, and
reproject the two classes **at different rates**, the background rate below the
frame rate. Three **does not match** findings. M9 varies *coding* refresh by
eccentricity, not reprojection rate by depth class, and the claim ties its rate
limitation to depth classification throughout.

**Google, US 10319114 B2**, "Foveated compression of display streams"
(*verified*; filed 2017-07-13, granted 2019-06-11; expiry *estimated* 2037-07-13;
independent claims 1, 11). Requires high-acuity pixels in a **first buffer** and
low-acuity pixels in a **second**, *reorganizing* the first to match the second's
line length, **multiplexing** them into a display stream, then compressing. We
have one buffer per eye and signal acuity per tile (M5). **Does not match** on
three elements; evidence that per-region resolution over a headset link is a
crowded field.

**Cisco, US 8494049 B2**, "Long term reference frame management with error video
feedback for compressed video communication" (*verified*; priority 2007-04-09,
filed 2007-04-18, granted 2013-07-23; expiry *estimated* **2027-04-18**;
independent claims 1, 14, 23) — the nearest live art to M7 and M8. Claim 1
requires a **reference frame message** commanding decoders to create a long-term
reference, the same data sent to **a plurality of decoders**, feedback on receipt
of that message, **repetition until every decoder acknowledges**, and only then
use of the long-term reference on error. We have no LTR and no message (the ring
is four slots addressed by `frame_number mod 4`, SYNTAX 13.2), we are unicast,
there is no handshake, and reference choice is per tile from the exactness rule
(TRANSPORT 9) with no error trigger. **Does not match** on all four. Its value is
as a date anchor: feedback-driven reference selection was patented practice by
2007 and standardised earlier (section 4).

---

## 4. Prior art, per mechanism, with dates

Everything here is offered as art *for* us — to argue that a claim reading on us
is invalid, or to document that the idea came from a public source.

| Mechanism | Prior art | Date | Status |
|---|---|---|---|
| M1, M2 — warping a reference by transmitted global parameters | MPEG-4 Part 2 global motion compensation / sprite warping | ISO/IEC 14496-2, 1999 (*reported*, Annex B.1 [I-7]) | Expired |
| M1, M2 | AV1 global motion, corner-then-interpolate | 2018 | Royalty-free under the AOM licence **for AV1 implementations only**; cited as art, not as a licence |
| M1–M3 — split rendering with a server-side residual against a client-side approximation | Levoy, *Polygon-Assisted JPEG and MPEG Compression of Synthetic Images*, SIGGRAPH 1995 | 1995 (*verified*) | Public. Server renders high and low quality, subtracts, sends the difference; client renders the low-quality image and adds it. The oldest published statement of the idea AMD's family sits on |
| M2 interpolation | Keys, cubic convolution | 1981 | Expired |
| M5, M6 — foveated coding | Geisler and Kortum, **US 6252989 B1**, Univ. of Texas | filed 1997-12-23, granted 2001-06-26 (*verified*) | **Expired — lifetime.** Claim 1 recites a foveated Laplacian pyramid, subsets "corresponding to one or more fixation points" (claim 5), applied to video (claim 8). Their 1996–1998 papers are public alongside it |
| M7 — per-segment reference picture selection driven by decoder feedback | ITU-T H.263 Annex N (Reference Picture Selection) and Annex R (Independent Segment Decoding) | 1998 (*reported*) | Expired. NEWPRED, its MPEG-4 successor, selects a reference **per image segment** — the closest published ancestor of M7 |
| M8 — deterministic concealment the encoder can replay | H.263 Annex N feedback modes; the "encoder tracks decoder state" idea in RFC-era videoconferencing | 1998 | Expired, but nothing found states the *identity* between the concealment path and a normal skip path. That identity looks novel |
| M9 — rolling intra refresh | H.263/MPEG-2 era intra refresh; VVC gradual decoding refresh restates it | 1990s | Expired |
| M10 — disparity-compensated prediction of a dependent view by a translational block vector | **MPEG-2 Multiview Profile**, ISO/IEC 13818-2 Amendment 3 | promoted to IS **September 1996** (*reported*, two sources) | Expired. The anchor `stereo/PATENT_NOTES.md` 6.1 asks for, and it appears to hold: base view standard-compatible, dependent view predicted inter-view. Confirm against the text |
| M10 | H.264 MVC (Annex H) | 2009 | HEVC/AVC pool territory; cited as art only |
| M11 — rANS | Duda, public-domain construction | 2009–2014 | Public domain per author |
| M11 — interleaved rANS on one byte stream | Giesen, public-domain post and reference code | 2014 | Public domain |
| M12 — enhancement layer with a two-hypothesis blend | H.263 Annex O spatial scalability and bi-prediction weights | 1998 | Expired (Annex B.1) |

One gap: **no expired art was found for the combination of per-tile resolution
selection with per-tile reference selection under feedback.** Each half is old;
the pairing is ours, and that is where a defensive publication does most good.

---

## 5. Prioritised list

**Read in this order.**

1. **Intel US 2025/0280147 A1 (pending) and the file wrapper of US 12348767 B2.**
   The only item where a *pending* claim could be written onto our exact
   combination, over a disclosure containing an unclaimed pose-derived global
   motion predictor. Ask whether a further continuation can still reach M1.
2. **AMD/ATI US 10432988 B2 claim 1, and the claims of US 12120364 B2.** The
   only item found that reads on the deployed system on its face. Two narrow
   questions: how is "render **a portion** of the next frame" construed in the
   prosecution history, and does the pre-2016 split-rendering art invalidate it?
3. **Intel US 12348767 B2 claim 1 against `rc/`.** A one-hour engineering
   question — does the rate controller close a loop on fovea-region statistics? —
   whose binary answer decides infringement.
4. **Microsoft US 2022/0109891 A1 (pending).** Claim 40 is a bitstream claim.
   Docket it; re-read at allowance.
5. **ZeniMax/Microsoft US 11503326 B2 claim 17** under the doctrine of
   equivalents: is a homography evaluated at tile centres equivalent to
   "converting motion vectors into per-block motion vectors"?
6. **The MPEG-2 Multiview Profile text**, to confirm the M10 anchor and close the
   STEREO item of Annex B.3 cheaply.
7. Everything else — Varjo, Apple, Qualcomm, Valve, Google, Cisco — as a
   confirmation pass; non-match on structural elements in all six.

**Not yet done:** a proper landscape search rather than a targeted one.
Meta/Oculus produced only reprojection and frame-extrapolation families here, all
depth- or motion-vector-dependent — a suspiciously thin result for the largest
filer in the field, which probably reflects the search rather than the portfolio.
Qualcomm's foveated-compression family was seen only through secondary pages, and
no published NVIDIA CloudXR or Apple foveated-streaming applications were located.
**Treat the absence of a Meta blocker here as unproven, not as a finding.**

## 6. Cheap design changes that remove risk

Ordered by ratio of risk removed to work required.

1. **Keep transmitting the matrix rather than deriving it from the pose in the
   decoder.** The obvious FTO move — derive global motion from the pose
   client-side — is the wrong one here, and SYNTAX 3.1.1 already went the other
   way because the decoder does no floating-point arithmetic. That leaves us
   *further* from "deriving a global motion predictor from HMD position
   information": the decoder never interprets a pose (M4), and what is on the wire
   is an ordinary global-motion parameter set of the MPEG-4 GMC lineage. Say so in
   the specification, and refuse the derive-from-pose simplification if it is ever
   proposed on compression grounds — it would trade 72 bytes a frame for a claim
   element.
2. **Make the rate controller's foveal statistics non-circular.** If `rc/` sets
   QP from measured bit counts of previous frames' fovea regions, change it to set
   QP from the foveation map and the frame budget, using prior-frame statistics
   only at whole-frame granularity. Cheap, and it removes the only element of
   Intel claim 1 that is in doubt.
3. **Write the depth negative into the normative text**, as
   `stereo/PATENT_NOTES.md` 5.5 recommends: "no conforming decoder consumes depth
   or camera parameters; depth-derived quantities exist only in the encoder and
   are not recoverable from the bitstream." One sentence in
   `spec/06-decoding-process.md` defeats an element in Valve, Qualcomm and the
   whole 3D-HEVC family at once.
4. **Add the same sentence for the display path**: the decoder warps *references
   into predictions*, never the decoded picture for display; any ATW/ASW the
   runtime performs is outside the codec. One sentence, and the section 3.6
   distinction becomes a design constraint rather than an accident.
5. **Do not let the encoder instantiate the warp as per-tile motion vectors fed
   to a search-bypass stage** — the one plausible future optimisation that would
   walk into ZeniMax claim 17. Record it in Annex B.2 with the other deliberate
   avoidances.
6. **Defensive publication of M8 and of the M5+M7 pairing.** Nothing found reads
   on either; publishing dates them and makes later filings by others harder. An
   afternoon and a DOI — the highest value per hour on this list.
7. **Correct Annex B.4**: replace the recorded rANS defence with the actual
   non-match (no two-phase structure, no conditional emission), keeping the
   fixed-precision rationale as a secondary argument against the disclosure. And
   **downgrade Varjo US 11568574 B1 in Annex B.3.1** to "different mechanism
   (ring-packed spherical remap); portfolio watch only".

## 7. What this document does not establish

It does not clear NX Warp. It reads eleven claim sets against a specification and
records where the words do not meet. It does not address contributory or induced
infringement, the AOM licence's scope when AV1 is cited as art, whether an HEVC
pool member's claims reach a non-HEVC codec (`stereo/PATENT_NOTES.md` question 6,
still open), or any jurisdiction outside the United States — the EP members of
the AMD and Valve families were seen here only as identifiers.

The five-item agenda of `spec/annex-b-patents.md` B.3 is now: item 1 partially
answered and reframed (the Intel disclosure, not the Intel claims, is the risk;
AMD is the real one); item 2 unchanged and cheap to close once MPEG-2 MVP is
confirmed; item 3 (LCEVC) **not reached** — V-Nova's US members were not resolved
to numbers in this pass and no claim was read, so it remains fully open; item 4
answered as to the granted claim and re-pointed at the pending continuation; item
5 (concealment identity) searched and nothing found, which is the best result on
the page.
