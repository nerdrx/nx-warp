# 9. Conformance

## 9.1 The requirement

A decoder conforms to this specification if, for **every** conforming
bitstream, it produces sample arrays **bit-identical** to those produced by the
reference decoder, and rejects every non-conforming bitstream that this
specification requires it to reject.

**The tolerance is zero.** Not a PSNR threshold, not a maximum absolute
difference, not "visually indistinguishable" — zero. One differing least
significant bit is a conformance failure.

This is stricter than the tolerance a still-image codec would need, and the
reason is architectural rather than aesthetic: the encoder builds its
references by *running the decoder*, and predicts each frame from what it
believes the client holds [PAPER 2.6]. A single differing bit in one sample
becomes a wrong prediction, which becomes a wrong residual, which propagates
through every subsequent frame that references it and is corrected only by the
next intra refresh — up to two seconds later [PAPER 2.6]. There is no error
term the design can absorb, which is why clause 3.4 forbids floating point
outright rather than bounding it.

## 9.2 The reference

`docs/SYNTAX.md` [R-18] and the CPU reference codec in `ref/` [R-20] are
jointly the specification. Where the document and the code disagree, one of
them is defective and must be repaired; **neither may be "interpreted"**
[SYNTAX preamble]. This umbrella document set adds no normative behaviour of
its own: where it appears to, that is a defect in this document set.

The CPU reference decoder is the specification and SPIR-V is validated against
it, never the other way round [PAPER 3.7].

## 9.3 Conformance vectors

The vectors live in `tests/vectors/`. Each is a `.nxv` bitstream, and
`tests/vectors/vectors.md5` pins, per vector:

```
name  stream_md5  decoded_md5  width  height  pix  alpha  frames
```

Both digests matter and they test different things:

* **`decoded_md5`** is the conformance requirement: any decoder claiming
  conformance MUST reproduce it.
* **`stream_md5`** additionally pins the *encoder*, so that a change which
  silently alters the produced bytes is caught. It is not a conformance
  requirement for a decoder — any encoder producing a conforming bitstream is
  conforming — but it is what makes the vector set stable enough to be a
  reference.

At the time of writing the set contains **32 vectors** covering the Phase 1
(intra-only) surface: QP sweep, 4:2:0 and 4:4:4, gradient / checker / noise /
flat material, lossless with and without alpha, transform skip, `res_level`
cycling and level-2, per-tile QP and `res_level` maps, a 4:2:0 tile inside a
4:4:4 stream, the built-in weighting matrices, odd picture sizes (200x140), a
single-tile picture (64x64), a wide one-row picture (320x64), and a
three-frame stream.

`nxv-vectors --check tests/vectors` verifies both digests
[SYNTAX 12].

### 9.3.1 What the vector set does not yet cover

Every gap here is a place where a decoder could be wrong and conformance
testing would not notice:

* **No inter vectors.** No `WARP_SKIP`, `WARP_MV`, `STATIC_MV` or `STEREO`
  stream exists, which follows from clause 6.7 having no syntax for the
  homography. [pending WARP.md]
* **No multi-eye, multi-layer or 10-bit vectors.**
* **No custom probability-table vector**, so the normalisation procedure of
  clause 6.6.2 — the one place a decoder divides, and the most easily
  mis-implemented step in the format — is not pinned by any vector.
  [pending SYNTAX.md]
* **No custom quantisation-matrix vector.**
* **No rejection vectors.** Clause 4 and clause 5 impose roughly forty "MUST
  reject" conditions and no vector exercises any of them. A decoder that
  accepts everything passes the current suite completely.
* **No `nsub_log2` sweep**, so only the eight-lane schedule is pinned.

Recorded as Annex C issue C-19.

## 9.4 How a decoder claims conformance

A decoder claims conformance by stating four things:

1. **The subset.** Phase 1 (clause 8.5) or full version 1; the `tools` bits it
   implements; the profiles and levels it accepts. A decoder that implements a
   subset is conforming *to that subset* only if it **refuses**, cleanly and
   with the right status, everything outside it — an unsupported status for a
   tool it does not implement, a malformed status for a stream that is
   actually malformed. Silently decoding something it does not fully implement
   is the one behaviour that is never conforming.
2. **The vector result.** Every `decoded_md5` in
   `tests/vectors/vectors.md5` reproduced exactly, for the vectors within its
   claimed subset.
3. **Robustness.** No read outside the supplied buffer, and no output produced
   from a stream it was required to reject, under the fuzzers in `tests/ref/`.
   A conforming decoder never produces output from a stream it must reject
   [SYNTAX 1].
4. **Agreement with the reference implementation** on any additional material
   the implementer tests with — captured streams, generated material, fuzzer
   corpora. The reference decoder is available to every implementer, so
   "it matched the vectors" is a floor, not a ceiling.

A Vulkan decoder additionally claims conformance by reproducing every
`decoded_md5` from the same bitstreams as the CPU reference
[SYNTAX 12], on every vendor it claims support for. Bit-exactness across
vendors is not an aspiration here — it is the property the format was
constructed to have (clause 3.4), so a vendor-specific difference is a bug in
the shader, not a tolerance to be documented.

## 9.5 Encoder conformance

An encoder conforms if every bitstream it emits is a conforming bitstream. It
is not otherwise constrained: mode decision, rate control, motion search and
quantisation are entirely free (clause 0.3).

One property is worth stating even though it is not a requirement, because the
whole loss-recovery design depends on it: an encoder that does **not** run the
real decoder to build its references will drift from its client, and the drift
is invisible until it becomes a visible artefact that persists for up to two
seconds. The reference encoder runs the reference decoder for exactly this
reason [PAPER 2.10, 6.6].

## 9.6 Checking this document set

`spec/tools/check_spec.py` is not a conformance test. It checks that *this
document set* is internally consistent — that every syntax element declared in
clause 4 has semantics in clause 5, and that the `[pending]` markers are
counted rather than forgotten. It is registered with CTest as `spec.check` and
passes while pending markers remain, because they are the honest state of the
document; `--strict` fails on them and is what a release build would use.
