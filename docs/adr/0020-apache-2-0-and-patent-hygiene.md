# ADR-0020: Apache-2.0, and only public-domain or expired coding tools

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 1.9, 5.7, design principle 7
- **Affects**: the whole project

## Context

A video codec is the most patent-dense area of consumer software. A project that intends to ship
inside an open-source VR streamer has to be able to say, tool by tool, why each one is safe, and it
has to choose a licence whose patent terms match that ambition.

## Decision

**Licence: Apache-2.0.** It carries an explicit patent grant from contributors and a defensive
termination clause, which is the right shape for a codec: a contributor cannot contribute a tool and
later assert a patent on it against users of the project.

**Tool policy: only public-domain, expired, or explicitly royalty-free coding tools**, with a written
record of the source of each one kept from day one.

Safe by expiry or explicit grant (paper 1.9):

- DCT (1974) and the Loeffler-Ligtenberg-Moschytz factorization (1989), used with the project's own
  9-bit integer constants and a defined two-stage shift, so the coefficient set is not HEVC's
- YCoCg-R (Malvar and Sullivan 2003, offered royalty-free for H.264 FRExt, and past 20 years)
- Exp-Golomb; JPEG-style weighting matrices; dead-zone quantisation; trellis quantisation (1990)
- H.263 and MPEG-2 era bi-prediction weights and spatial scalability
- H.264 baseline tools filed 2002 to 2003, including the 4x4 integer transform and the intra16x16 DC
  Hadamard idea (expired 2023 to 2024 in the US and EU; FRExt-era claims to be checked individually)
- rANS (Duda, placed in the public domain) and Giesen's interleaving (a blog post with public-domain
  code)
- EZW (1993) and SPIHT (1996) for the bit-plane fallback; VC-2 under the BBC's royalty-free declaration
- MPEG-4 Part 2 global motion compensation (1999, expired) as prior art for warped prediction

Deliberately avoided: HEVC-specific tools (its transform matrices, SAO, merge and AMVP design, its
CABAC context tables); AV1 tools reused outside AV1 (the AOM licence covers implementations of AV1
itself, not reuse of CDEF, loop restoration or its adaptive multi-symbol CDF scheme in another codec);
LCEVC's residual temporal prediction and transforms; JPEG XS's specific bit-plane-count coding; and
3D-HEVC view synthesis prediction.

H.264 and HEVC are only ever touched through the device's own licensed hardware decoder, in the hybrid
path.

Specific recorded caveats: Microsoft was granted rANS-related patents in 2022 concerning selective
switching of state precision, which is why v1 fixes state width and probability precision (ADR-0003);
and YCoCg-R had Microsoft patents from the 2003 era which are at or past expiry and should be
verified.

## Consequences

- The codec gives up measurable compression efficiency to stay clear: no CABAC (an estimated 8 percent
  on the same coefficient statistics), no directional intra in v1 (an estimated 25 to 40 percent on
  intra tiles), no HEVC-class transform or in-loop filters.
- Every ADR that adopts a tool is expected to state that tool's patent status, which is what turns the
  policy into a record rather than an intention.
- Contributors must be able to state the origin of any algorithm they add. This belongs in
  [CONTRIBUTING.md](../../CONTRIBUTING.md).
- The policy does not replace the scoped FTO review of the four novel constructions (ADR-0017).
- This is an engineering map, not legal advice, and the paper says so in both places it appears.

## Alternatives considered

- **MIT or BSD-2.** Rejected: no explicit patent grant, which is the one licence term that matters
  most for a codec.
- **GPL or LGPL.** Rejected: it would restrict integration into the environments this codec is meant
  to live in, and the patent story is no better.
- **Use the best available tools and license them.** Rejected: the project cannot pay pool fees, and a
  royalty-bearing codec inside an open-source streamer would be unusable by its own users.

## References

- Paper 1.9 (patent hygiene), 5.7 (patent and royalty summary), design principle 7
- [LICENSE](../../LICENSE), [CONTRIBUTING.md](../../CONTRIBUTING.md)
- ADR-0003, ADR-0004, ADR-0012, ADR-0014, ADR-0017
