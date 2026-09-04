# 0. Scope

## 0.1 What this specification defines

This document set specifies the **NX Warp video coding format, version 1**: a
tile-parallel video codec designed to be decoded by GPU compute shaders on a
head-mounted display, using the head pose that produced each frame as its
inter-frame predictor.

It specifies:

* the syntax of the NX Warp bitstream (clause 4);
* the meaning of every syntax element (clause 5);
* the decoding process that maps a conforming bitstream to decoded pictures,
  bit-exactly and in a defined order (clause 6);
* the properties of the transport encapsulation that a decoder must understand
  in order to know which references it holds and which samples are concealed
  (clause 7);
* profiles, levels and capability signalling (clause 8);
* what it means for a decoder to conform, and how conformance is demonstrated
  (clause 9);
* the normative constant tables the decoding process reads (Annex A).

## 0.2 The output of the decoding process

The decoding process specified in clause 6 produces, for each coded frame, an
array of decoded samples per plane per eye, together with the per-tile state
that the next frame's prediction reads. The mapping from those samples to
photons — compositing, lens correction, the runtime's own reprojection — is
outside this specification.

A conforming decoder produces **bit-identical** samples to the reference
decoder for every conforming bitstream. There is no tolerance (clause 9.4).

## 0.3 What this specification does not define

* **The encoder.** Any encoder that produces a conforming bitstream is a
  conforming encoder. Encoder behaviour appears here only where it constrains
  what a bitstream may contain (for example, the constraint that a lane's
  initial rANS state is at least `L`), or as explicitly informative material.
* **Rate control, mode decision, motion search, foveation mapping.** These are
  encoder concerns. `docs/PAPER.md` clause 4.6 and `docs/RATECONTROL.md`
  *(does not exist yet)* describe them; nothing there changes the decoding
  process. [pending RATECONTROL.md]
* **The GPU implementation.** `docs/PAPER.md` clause 3 designs a Vulkan
  compute decoder in two dispatches. That design is *one* conforming
  implementation. This specification is written so that a scalar CPU decoder
  and a Vulkan decoder are equally conforming, and clause 9 requires them to
  agree bit for bit.
* **Session establishment, key exchange, congestion control, and the
  server-side pipeline.** Clause 7 takes from `docs/TRANSPORT.md` only what a
  decoder must know; the rest of `docs/TRANSPORT.md` is normative for the
  transport library and out of scope here.
* **The pose semantics.** The 26 pose bytes in the frame header are opaque to
  the decoding process (clause 5.3.2). Their interpretation belongs to the
  application and to `docs/WARP.md`. [pending WARP.md]

## 0.4 Structure of a stream

Informative summary; the normative form is clause 4.

```
stream        := stream_header  extension_area  frame*
frame         := frame_header  [quant_matrices]  table_set*  tile_row*
tile_row      := tile_row_header  tile*
tile          := tile_header  [mv]  [alpha_value]  payload
payload       := interleaved rANS substreams over coding units
```

A **tile** is 64x64 luma samples and is an independently decodable bitstream:
no entropy state, no prediction and no filter crosses a tile boundary within a
frame. There is no deblocking filter and no loop filter in version 1
[SYNTAX 10]. Inter prediction reads the reference picture across tile
boundaries, which is why reference eligibility is defined per 3x3 tile
neighbourhood (clause 7.3).

## 0.5 Versions and phases

| Name | Meaning |
|---|---|
| version 1 | The format specified here. `version` in the stream header is 1 |
| Phase 1 | The intra-only subset, clause 8.5. A Phase 1 decoder parses the full v1 syntax and refuses inter streams cleanly [SYNTAX 12] |
| Phase 2 | Adds the pose-warped inter predictor [pending WARP.md] |
| v2 | Reserved tools, signalled by tool bits that a v1 decoder refuses (clause 8.4). Not specified here |

## 0.6 Relationship to the design paper

`docs/PAPER.md` is a design paper and is **informative**. Where it and a
component specification disagree, the component specification is normative and
the disagreement is recorded in Annex C. The paper is cited in this document
set only for rationale, for the numbers behind a decision, and where no
component document has yet been written — in which case the citation is always
accompanied by a `[pending ...]` marker.
