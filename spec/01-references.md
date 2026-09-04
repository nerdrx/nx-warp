# 1. References

Every reference is labelled. Normative references (`[R-n]`) are indispensable
for applying this specification. Informative references (`[I-n]`) record prior
art, rationale and freedom-to-operate context; nothing in this specification
depends on them.

A reference marked **[pending citation]** is one whose exact identifier,
edition or version could not be verified from inside this repository. It is
listed because the work is real and load-bearing; the marker means *check the
identifier before publication*, not *this may be fictitious*.

---

## 1.1 Normative references

### Language and process

**[R-1]** IETF RFC 2119, *Key words for use in RFCs to Indicate Requirement
Levels*, S. Bradner, March 1997. Together with IETF RFC 8174, *Ambiguity of
Uppercase vs Lowercase in RFC 2119 Key Words*, B. Leiba, May 2017.

### Arithmetic and sample representation

**[R-2]** IEEE Std 754-2019, *IEEE Standard for Floating-Point Arithmetic*.
Referenced **only** for the bit patterns `binary16` and `binary32` used to
describe the opaque pose payload (clause 5.3.2). The decoding process performs
no floating-point arithmetic.

**[R-3]** Khronos Group, *SPIR-V Specification*, version 1.6. Referenced for
the exact semantics of arithmetic shift, `OpSMulExtended` / `OpUMulExtended`,
and the undefinedness of out-of-range shift amounts, which clause 3 relies on
when it forbids them. [pending citation] — pin the revision used.

**[R-4]** Khronos Group, *Vulkan Specification*, version 1.3 (core), with
`VK_EXT_subgroup_size_control` (promoted to Vulkan 1.3) and
`VK_KHR_pipeline_executable_properties`. Normative only for a Vulkan
implementation of the decoding process; a CPU decoder does not need it.
[pending citation] — pin the revision used.

### Coding tools

**[R-5]** C. Loeffler, A. Ligtenberg and G. S. Moschytz, *Practical fast 1-D
DCT algorithms with 11 multiplications*, Proc. IEEE ICASSP 1989, vol. 2,
pp. 988–991. The flow graph of the 8-point inverse transform in clause 6.4
[SYNTAX 6.1–6.3]. The nine-bit integer constants in Annex A.1 are this
specification's own; they are not taken from any other codec.

**[R-6]** N. Ahmed, T. Natarajan and K. R. Rao, *Discrete Cosine Transform*,
IEEE Transactions on Computers, vol. C-23, no. 1, pp. 90–93, January 1974. The
transform the factorisation of [R-5] computes.

**[R-7]** H. S. Malvar and G. J. Sullivan, *YCoCg-R: A Color Space with RGB
Reversibility and Low Dynamic Range*, Joint Video Team document JVT-I014r3,
9th JVT meeting, San Diego, July 2003. The reversible lifting transform of
clause 6.3 [SYNTAX 5.1].

**[R-8]** J. Duda, *Asymmetric numeral systems: entropy coding combining speed
of Huffman coding with compression rate of arithmetic coding*,
arXiv:1311.2540, 2013 (revised 2014). Earlier: J. Duda, *Asymmetric numeral
systems*, arXiv:0902.0271, 2009. The rANS construction of clause 6.6
[SYNTAX 9.5]. The author placed the construction in the public domain.

**[R-9]** F. Giesen, *Interleaved entropy coders*, arXiv:1402.3392, 2014, and
the accompanying public-domain reference code `ryg_rans`. The interleaving of
multiple rANS states over a single byte stream, clause 6.6.3. [pending citation]
— confirm the arXiv identifier and the repository revision cited.

**[R-10]** R. G. Keys, *Cubic convolution interpolation for digital image
processing*, IEEE Transactions on Acoustics, Speech, and Signal Processing,
vol. 29, no. 6, pp. 1153–1160, December 1981. The `a = -1/2` cubic convolution
kernel from which the integer Catmull-Rom taps of Annex A.4 are derived; see
also E. Catmull and R. Rom, *A class of local interpolating splines*, in
Computer Aided Geometric Design, Academic Press, 1974.

### Transport and security

**[R-11]** IETF RFC 5116, *An Interface and Algorithms for Authenticated
Encryption*, D. McGrew, January 2008. The AEAD interface
(`seal`/`open`, 16-byte tag, 12-byte nonce) that clause 7.2 and
[TRANSPORT 4] use.

**[R-12]** NIST Special Publication 800-38D, *Recommendation for Block Cipher
Modes of Operation: Galois/Counter Mode (GCM) and GMAC*, November 2007, with
FIPS PUB 197, *Advanced Encryption Standard (AES)*. The AES-256-GCM AEAD.

**[R-13]** IETF RFC 8439, *ChaCha20 and Poly1305 for IETF Protocols*, Y. Nir
and A. Langley, June 2018. The negotiated AEAD fallback.

**[R-14]** IETF RFC 5869, *HMAC-based Extract-and-Expand Key Derivation
Function (HKDF)*, H. Krawczyk and P. Eronen, May 2010. Per-path subkey
derivation [TRANSPORT 4.1].

**[R-15]** IETF RFC 5510, *Reed-Solomon Forward Error Correction (FEC)
Schemes*, J. Lacan, V. Roca, J. Peltotalo and S. Peltotalo, April 2009. The
apt reference for the systematic block Reed-Solomon code over GF(256) used by
[TRANSPORT 6]; see also I. S. Reed and G. Solomon, *Polynomial codes over
certain finite fields*, Journal of the SIAM, vol. 8, no. 2, pp. 300–304, 1960.
IETF RFC 8681, *Sliding Window Random Linear Code (RLC) Forward Erasure
Correction Schemes for FECFRAME*, V. Roca and B. Teuwen, January 2020, is
**not** the scheme in use: NX Warp uses a block code with group boundaries
aligned to row bands so that FEC adds no latency [TRANSPORT 6.1], not a sliding
window. It is listed as [I-14] for comparison.

**[R-16]** IETF RFC 768, *User Datagram Protocol*, J. Postel, August 1980.

### Runtime environment

**[R-17]** Khronos Group, *The OpenXR Specification*, version 1.1, with
`XR_KHR_composition_layer_depth`. Referenced for the pose and field-of-view
conventions the encoder derives the homography from, and for the depth layer an
encoder may use as a search seed. No part of the decoding process depends on
it. [pending citation] — pin the version used.

### Repository sources with normative standing

**[R-18]** `docs/SYNTAX.md`, *NX Warp v1 bitstream syntax*, this repository.
Normative source for clauses 4, 5, 6.1–6.6 and Annex A.

**[R-19]** `docs/TRANSPORT.md`, *NX Warp transport: normative wire format and
state machines*, this repository. Normative source for clause 7.

**[R-20]** `ref/`, the CPU reference codec, this repository. Co-normative with
[R-18] by its own clause 0: where the document and the code disagree, one of
them is defective and must be repaired; neither is to be interpreted.
Constant tables transcribed into Annex A cite this source by file and commit.

**[R-21]** `docs/WARP.md` — *does not exist at the time of writing.* Intended
normative source for the integer pose warp, clause 6.7. Until it lands, that
clause cites `warp/include/nxvc/warp.h` and `warp/ref/warp_ref.cpp` by commit
and is marked provisional. [pending WARP.md]

**[R-22]** `docs/STEREO.md` — *does not exist at the time of writing.*
Intended normative source for inter-view prediction, clause 6.8.
[pending STEREO.md]

**[R-23]** `docs/HYBRID.md` — *does not exist at the time of writing.*
Intended normative source for the HEVC/AVC base-layer path, clause 6.9.
[pending HYBRID.md]

---

## 1.2 Informative references

**[I-1]** `docs/PAPER.md`, *A Vulkan Compute Video Codec for Low-Latency VR
Streaming*, design paper draft 1, this repository. Rationale for every decision
in this specification. Explicitly non-normative.

**[I-2]** `docs/RATECONTROL.md` — *does not exist at the time of writing.*
Intended informative description of the encoder's rate control and the
degradation ladder of [I-1] clause 4.6.1. [pending RATECONTROL.md]

**[I-3]** ITU-T Recommendation H.264 | ISO/IEC 14496-10, *Advanced video coding
for generic audiovisual services*. Cited for the tools NX Warp deliberately
resembles (4x4 integer transform, intra 16x16 DC transform, quarter-pel motion)
and for Annex H (Multiview Video Coding), against which clause 6.8's expected
gains are quoted. [pending citation] — confirm the Annex letter for MVC.

**[I-4]** ITU-T Recommendation H.265 | ISO/IEC 23008-2, *High efficiency video
coding*. Cited for the tools NX Warp deliberately avoids (its transform
matrices, SAO, merge/AMVP, its CABAC context design) and for the multiview and
3D extensions relevant to Annex B. [pending citation] — confirm the Annex
letters for MV-HEVC and 3D-HEVC.

**[I-5]** Alliance for Open Media, *AV1 Bitstream & Decoding Process
Specification*, version 1.0.0 with Errata 1, 2019. Cited for the
corner-then-interpolate structure of global motion warping, which is prior art
for clause 6.7, and as the model for this document set's editorial style.

**[I-6]** ISO/IEC 23094-2, *Low Complexity Enhancement Video Coding* (MPEG-5
Part 2, LCEVC). Cited in Annex B as the closest relative of the layered mode.

**[I-7]** ISO/IEC 14496-2, *Coding of audio-visual objects — Part 2: Visual*
(MPEG-4 Part 2), global motion compensation / sprite warping. Prior art for
clause 6.7.

**[I-8]** SMPTE ST 2042-1, *VC-2 Video Compression*. The BBC's royalty-free
intra codec; the source of the bit-plane fallback lineage and of the 5/3
wavelet reserved as a v2 tool. [pending citation] — confirm the edition year.

**[I-9]** D. Le Gall and A. Tabatabai, *Sub-band coding of digital images using
symmetric short kernel filters and arithmetic coding techniques*, Proc. IEEE
ICASSP 1988, pp. 761–764. The 5/3 filter pair behind the reserved
`XFORM_WAVELET` tool.

**[I-10]** J. M. Shapiro, *Embedded image coding using zerotrees of wavelet
coefficients*, IEEE Transactions on Signal Processing, vol. 41, no. 12,
pp. 3445–3462, December 1993 (EZW), and A. Said and W. A. Pearlman, *A new,
fast, and efficient image codec based on set partitioning in hierarchical
trees*, IEEE Transactions on Circuits and Systems for Video Technology, vol. 6,
no. 3, pp. 243–250, June 1996 (SPIHT). The lineage of the reserved
`ENT_BITPLANE` fallback.

**[I-11]** D. Marpe, H. Schwarz and T. Wiegand, *Context-based adaptive binary
arithmetic coding in the H.264/AVC video compression standard*, IEEE
Transactions on Circuits and Systems for Video Technology, vol. 13, no. 7,
pp. 620–636, July 2003. The coder NX Warp's entropy stage is measured against.

**[I-12]** M. W. Marcellin and T. R. Fischer, *Trellis coded quantization of
memoryless and Gauss-Markov sources*, IEEE Transactions on Communications,
vol. 38, no. 1, pp. 82–93, January 1990. The prior art cited by [I-1] 1.9 for
the encoder's trellis quantisation, which is not part of the decoding process.

**[I-13]** Oculus VR, *Asynchronous Spacewarp*, developer documentation, 2016.
The precedent — and the cautionary tale — for reusing coded motion vectors as a
motion field, [I-1] 2.8. [pending citation] — a stable identifier for this
document should be found or the reference dropped.

**[I-14]** IETF RFC 8681, *Sliding Window Random Linear Code (RLC) Forward
Erasure Correction (FEC) Schemes for FECFRAME*, V. Roca and B. Teuwen, January
2020. Listed for comparison only; see [R-15] for why NX Warp uses a block code
instead.

**[I-15]** Microsoft Corporation, patents granted in 2022 concerning rANS
encoder and decoder features including selective switching of state precision.
The fixed single state width and single probability precision of clause 6.6 are
chosen to stay outside these claims. Identifiers are deliberately not quoted
here: the freedom-to-operate review of Annex B.3 must obtain them from counsel.
[pending review]
